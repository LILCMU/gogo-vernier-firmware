#include "control-loop.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "main.h"
#include "slot-helpers.h"
#include "uart-adapter.h"
#include "vernier-adapter.h"

// ---- externs from main.cpp ---------------------------------------------
// main.cpp owns the global singletons (UART, NVS, mutex, cross-task
// flags). control-loop.cpp pulls them in by extern so it can stay
// agnostic of the FreeRTOS task wiring around them.
extern UartAdapter       uart;
extern Preferences       preferences;
extern SemaphoreHandle_t nvsMutex;

// Cross-task FreeRTOS hand-off flags — see the comment block at the
// definition in main.cpp for the cross-task volatile / memory-barrier
// rationale. The publish in cmdSetPeriod must pair with a `volatile`
// read in autoConnectDevice.
extern std::atomic<bool> startAutoConnect;
extern std::atomic<bool> foundSavedDevice;
extern std::atomic<bool> slotHasSavedDevice[VERNIER_MAX_SLOTS];

namespace {

// 16-bit period overflow guard — wire protocol uses uint16_t. Local to
// this TU; the only caller is cmdSetPeriod.
constexpr uint32_t MAX_PERIOD_MS   = 0xFFFFu;

// Sentinel req_seq for a ConnectRequest with no host request behind it
// (boot auto-connect, BOOT button, background reconnect).
constexpr uint32_t NO_HOST_REQUEST = 0xFFFFFFFFu;

// Button polling state. Function-local before the move; kept here so
// only this TU touches them.
unsigned long startPressTime  = 0;
ButtonEvent   prevButtonEvent = BUTTON_RELEASE;

// Who queued a ConnectRequest; decides what a failure does.
//   USER       host C_CONNECT, BOOT short-press, and auto-connect passes
//              after the first (the host re-sending C_SET_PERIOD). A
//              failure never arms the background reconnect.
//   BOOT       the first auto-connect pass after the co-MCU booted. A
//              failure on a slot with a saved name arms the background
//              reconnect: the sensor may just be switched on later.
//   BACKGROUND reconnectLostSensors. Dropped by bleWorker if the slot
//              was disarmed after it was queued.
enum class ConnectOrigin : uint8_t
{
    USER,
    BOOT,
    BACKGROUND,
};

// ConnectRequest queued by enqueueConnect, drained by bleWorkerEntry.
// Layout on RV32: u8 slot + bool force + u8 origin + 1 B pad +
// u32 req_seq = 8 B.
struct ConnectRequest {
    uint8_t       slot;
    bool          force;
    ConnectOrigin origin;
    uint32_t      req_seq; // NO_HOST_REQUEST when not host-driven.
};

// Loop-task-only timing of the background reconnect. The state itself
// (VernierAdapter::ReconnectState) lives on the adapter because other
// tasks change it; this is just when reconnectLostSensors() may try.
struct ReconnectSchedule {
    // ReconnectState at the previous pass. Leaving OFF (re)starts the
    // schedule, whichever task armed the slot.
    VernierAdapter::ReconnectState seen = VernierAdapter::ReconnectState::OFF;
    uint32_t armedAtMs     = 0;     // millis() when the slot left OFF (give-up clock)
    uint32_t nextAttemptMs = 0;     // millis() at which the next rescan may start
    uint32_t backoffMs     = 0;     // current gap between rescans; grows after each failure
    bool     inFlight      = false; // a rescan this schedule queued has not resolved
};
ReconnectSchedule reconnectSchedule[VERNIER_MAX_SLOTS];

QueueHandle_t bleWorkQueue = nullptr;
TaskHandle_t  bleWorkerTaskHandle = nullptr;

// Held across emitDevList's snapshot and send. uartHandler, loop and
// bleWorker all emit T_DEV_LIST; without it a snapshot taken before a
// slot changed state could be sent after the newer one, and the host
// would act on the stale table (e.g. free a slot that is READY).
StaticSemaphore_t devListMutexBuf;
SemaphoreHandle_t devListMutex = xSemaphoreCreateMutexStatic(&devListMutexBuf);

// Who has seen the boot handshake (T_HELLO + T_STATUS). The host's
// T_HELLO handler wipes its whole slot table, so the handshake must go
// out before any per-slot frame and never between two connects.
//   SILENT    nothing sent yet.
//   ANNOUNCED sent before any host command (auto-connect or button
//             connect with a silent host); the host may not have been
//             listening.
//   HEARD     sent in answer to a host command.
// Written by the UART task (dispatchHostCommand) and bleWorker
// (announceIfSilent).
enum class HostLink : uint8_t
{
    SILENT,
    ANNOUNCED,
    HEARD,
};
std::atomic<HostLink> hostLink{HostLink::SILENT};

}  // namespace

static void emitDevList();           // forward decl — defined below for grouping
static void publishConnectResult(uint8_t slot, bool ok, ConnectOrigin origin);

static void sendBootHandshake()
{
    uart.sendHello();
    uart.sendStatus(true, UartAdapter::CORE_STATE_READY);
}

static void announceIfSilent()
{
    HostLink expected = HostLink::SILENT;
    if (hostLink.compare_exchange_strong(expected, HostLink::ANNOUNCED,
                                         std::memory_order_relaxed))
        sendBootHandshake();
}

// Per-slot identity, stats and field names: everything the host needs
// to render a connected slot.
static void sendSlotFrames(uint8_t slot)
{
    VernierAdapter &dev = slots[slot];
    uart.sendDeviceInfo(dev.deviceName(), dev.orderCode(), dev.serialNumber(), slot);
    // H6: hold the per-slot status mutex across the multi-field
    // read so vernierHandler's 10 s refresh can't tear the
    // battery/charge/rssi triple mid-evaluation.
    dev.lockStatus();
    uart.sendDeviceStats(dev.batteryPercent(), dev.chargeState(), dev.rssi(), dev.droppedSamples(), slot);
    dev.unlockStatus();
    sendDeviceFieldsFor(uart, dev, slot);
}

// enqueueConnect(slot, force, req_seq, origin):
//   Single producer-side helper used by cmdConnect, buttonHandler,
//   autoConnectDevice and reconnectLostSensors. Atomically claims the
//   slot (CAS IDLE →
//   REQUESTED) and queues a ConnectRequest. Returns false — a no-op,
//   leaving state untouched — if the slot is already
//   REQUESTED / CONNECTING / READY, so callers can fire-and-forget
//   without their own busy-guard and duplicate enqueues (a re-armed
//   autoConnectDevice pass) don't clobber an in-flight request.
//   Returns true once the request is queued.
//
//   req_seq is carried through to the worker for log/trace
//   correlation; bleWorker no longer dispatches an ACK from
//   publishConnectResult (early-ACK happens in cmdConnect at
//   enqueue time, host learns outcome via T_DEV_LIST piggyback).
static bool enqueueConnect(uint8_t slot, bool force, uint32_t req_seq, ConnectOrigin origin)
{
    // Precondition: caller has validated slotInRange. configASSERT is
    // a no-op under NDEBUG so this costs nothing in release builds
    // but catches future bypasses at QA time.
    configASSERT(slotInRange(slot));

    // Claim the slot atomically: CAS IDLE → REQUESTED. Fails if the
    // slot is already REQUESTED / CONNECTING / READY, in which case a
    // duplicate enqueue (e.g. a second autoConnectDevice pass after
    // the host re-arms startAutoConnect) is a no-op — we must NOT
    // touch the state of a slot that already has a live queued
    // request, or its worker-side CAS REQUESTED→CONNECTING will fail
    // and the connect gets silently skipped.
    if (!slots[slot].tryRequestConnect())
        return false;

    // The queue is sized at VERNIER_MAX_SLOTS and a slot stays
    // non-IDLE for the whole time its request is queued (claim sets
    // REQUESTED, worker CASes to CONNECTING on dequeue). So at most
    // VERNIER_MAX_SLOTS requests are ever in flight — once we've won
    // the claim above, the queue is guaranteed to have room. Evaluate
    // the send outside configASSERT (a no-op macro under NDEBUG must
    // not swallow the call) and assert the invariant.
    ConnectRequest req{slot, force, origin, req_seq};
    const BaseType_t sent = xQueueSend(bleWorkQueue, &req, 0);
    configASSERT(sent == pdTRUE);
    return true;
}

// Outcome of a user connect (host C_CONNECT, BOOT short-press).
enum class UserConnect : uint8_t
{
    QUEUED,    // a new proximity connect is queued for the slot
    TOOK_OVER, // the slot's background reconnect attempt is queued or
               // connecting; it now answers the user (ReconnectState ADOPTED)
    BUSY,      // the slot is READY or connecting for someone else
};

// A user connect on a slot. On a slot waiting for its lost sensor it
// first claims the background reconnect (ARMED → ADOPTED) so the slot's
// real state shows on the wire and a failure re-arms it (see
// publishConnectResult). If the claim beat a queued background attempt
// to the slot, a fresh proximity connect is queued as usual; if an
// attempt for the saved name is already queued or connecting, that
// attempt answers the user.
static UserConnect requestUserConnect(uint8_t slot, uint32_t req_seq)
{
    const bool adopted = slots[slot].tryAdoptReconnect();
    if (enqueueConnect(slot, /*force=*/true, req_seq, ConnectOrigin::USER))
        return UserConnect::QUEUED;
    return adopted ? UserConnect::TOOK_OVER : UserConnect::BUSY;
}

// Persist the name of the sensor just connected to `slot` under the
// slot's NVS key (deviceName0..N, read back at boot so each slot can
// auto-connect its sensor), flag the slot for that boot auto-connect,
// and make the name the slot's connect(false) target, so a background
// reconnect rescans for this sensor, not for whatever a proximity
// connect would pick. bleWorker only, after the CONNECTING → READY CAS.
static void saveConnectedSensor(uint8_t slot, const char *name)
{
    char key[NVS_KEY_MAX_LEN];
    snprintf(key, sizeof(key), NVS_KEY_DEVICE_NAME_FMT, (unsigned)slot);
    NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false);
    // C_FORGET tears the slot down (IDLE) before it takes nvsMutex to
    // remove the key and clear the flag. Re-checked under that mutex: a
    // Forget that already tore the slot down skips the save, and one
    // that has not removes the key and clears the flag after this.
    if (!slots[slot].isReady())
    {
        log_i("slot %u: disconnected before its name was saved", (unsigned)slot);
        return;
    }
    log_d("NVS slot %u: saving %s = %s", (unsigned)slot, key, name);
    // Preferences skips the flash write when the stored value is the
    // same, so re-saving on every reconnect costs no wear.
    if (nvs) preferences.putString(key, name);
    // Fill-then-flag publish: store the per-slot data flag relaxed,
    // then release the "any saved?" gate so a reader that acquire-loads
    // foundSavedDevice sees this write.
    slotHasSavedDevice[slot].store(true, std::memory_order_relaxed);
    foundSavedDevice.store(true, std::memory_order_release);
    slots[slot].setOpenDevice(name);
}

// One sensor, one slot. A sensor just connected to `slot` is no longer
// looked for by another slot that lost it (it came back and was
// connected here with a Connect on another card or the BOOT short-press,
// which firstFreeSlot() steers away from a waiting slot). Without this
// that slot keeps rescanning, until the give-up time, for a sensor that
// is busy here, and takes it back the moment the user forgets it here.
// bleWorker is the only writer
// of _open_device, so reading it here is safe.
static void disarmOtherSlotsWaitingFor(uint8_t slot, const char *name)
{
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (i == slot
            || slots[i].reconnectState() == VernierAdapter::ReconnectState::OFF
            || slots[i].openDevice() != name)
            continue;
        slots[i].setReconnectState(VernierAdapter::ReconnectState::OFF);
        log_i("slot %u: %s connected to slot %u, reconnect stopped",
              (unsigned)i, name, (unsigned)slot);
    }
}

// True when the saved sensor of `slot` is connected to another slot. A
// boot connect that failed for that reason must not arm the slot.
static bool savedSensorReadyElsewhere(uint8_t slot)
{
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
        if (i != slot && slots[i].isReady()
            && slots[i].openDevice() == slots[slot].openDevice())
            return true;
    return false;
}

// publishConnectResult(slot, ok, origin):
//   Side-effect block that runs after dev.connect() resolves.
//   - On success: refresh status, kick off streaming, emit the
//     per-slot frames (T_DEVINFO, T_DEVSTATS, T_FIELDS), publish READY
//     (which ends any background reconnect), then save the sensor's
//     name (saveConnectedSensor) and stop other slots looking for it.
//   - On failure of a user connect that took over a background
//     reconnect: re-arm it (the sensor is still wanted).
//   - On failure of the boot connect of a saved sensor: arm the
//     background reconnect (the sensor may be switched on later).
//   - On any outcome but a failed background attempt: push T_DEV_LIST
//     so the host's slot table stays in sync (D4). A failed background
//     attempt changes nothing the host sees, so it pushes nothing.
//     The host's terminal ACK is not emitted here — cmdConnect ACKs at
//     enqueue time (early-ACK, §Decisions Q3 piggyback); outcome
//     correlates via T_DEV_LIST.
// Caller is responsible for the dev.connect() call itself + the
// slot-range guard. Safe to call from any task that owns access
// to the shared globals (uart's send mutex, nvsMutex via NvsScope).
static void publishConnectResult(uint8_t slot, bool ok, ConnectOrigin origin)
{
    VernierAdapter &dev = slots[slot];
    if (ok)
    {
        dev.getDeviceInfo();
        // If a previous connect (auto-connect on boot) already started
        // streaming, calling startReading() again restarts the GATT
        // subscription and resets the dropped-sample counter mid-experiment.
        // Only kick off the stream when the link isn't already producing.
        if (!dev.isStreaming())
            dev.startReading(dev.samplingPeriod());

        // T_HELLO / T_STATUS are NOT sent here. They're the global boot
        // handshake (peer identity + peer-up), emitted once per session
        // (see HostLink). Emitting T_HELLO per connect made the host's
        // T_HELLO handler reset its whole connection state on every
        // slot, so during a sequential 3-slot auto-reconnect each new
        // connect wiped the previously connected slots back to
        // "connecting" until the next periodic T_FIELDS re-emit. The
        // per-connect path now emits only the per-slot frames + the
        // trailing T_DEV_LIST.
        sendSlotFrames(slot);
    }

    // Terminal state transition AFTER all uart.send* calls so a
    // concurrent vernierHandler can't see state==READY before T_FIELDS
    // has shipped. Compiler barrier forces the sends to commit before
    // the state publish; same single-core RV32 reordering discipline
    // documented in CLAUDE.md §Code Quality Rules.
    __asm__ volatile("" ::: "memory");
    if (ok)
    {
        // CAS CONNECTING→READY. Fails iff a C_DISCONNECT / C_FORGET
        // for this slot landed during the handshake and already set
        // it IDLE. In that case abandon the connect — tear the link
        // back down (idempotent) and leave the slot IDLE — rather
        // than resurrect a slot the host just tore down. The name is
        // saved only after the CAS, so such a Forget is not undone.
        char name[VERNIER_DEVICE_NAME_LEN];
        if (!dev.tryFinishConnecting(name, sizeof(name)))
        {
            dev.disconnect(/*disarm=*/false);
        }
        else if (name[0] == '\0')
        {
            // An empty name would turn the slot's next saved-name
            // connect into a proximity connect.
            log_w("slot %u: connected sensor has no name, not saved", (unsigned)slot);
        }
        else
        {
            saveConnectedSensor(slot, name);
            disarmOtherSlotsWaitingFor(slot, name);
        }
    }
    else
    {
        // The boot connect of a saved sensor failed: it is probably
        // just switched off, so look for it in the background. Pushes
        // T_DEV_LIST below like any failed boot connect, because the
        // host saw this attempt as connecting.
        const bool armedAtBoot = origin == ConnectOrigin::BOOT
                                 && slotHasSavedDevice[slot].load(std::memory_order_relaxed)
                                 && !savedSensorReadyElsewhere(slot)
                                 && dev.tryArmFailedConnect();
        if (armedAtBoot)
            log_i("slot %u: saved sensor not found at boot, reconnecting in the background",
                  (unsigned)slot);
        else
            dev.setState(VernierAdapter::ConnState::IDLE);
        // A user connect that took over a background reconnect failed:
        // keep waiting for the saved sensor. A background attempt that
        // failed leaves the slot ARMED and the host's view unchanged
        // (emitDevList shows it IDLE either way), so skip the push.
        if (!armedAtBoot && !dev.tryRearmAdoptedReconnect()
            && dev.reconnectState() == VernierAdapter::ReconnectState::ARMED)
            return;
    }

    // Per design D4: T_DEV_LIST auto-pushed after every connect attempt
    // (success OR failure — the failure case still tells the host that
    // its connect didn't take, so the slot table stays accurate).
    // Runs after the state publish so the entry's `connected` flag
    // reflects the post-attempt truth.
    emitDevList();
}

// bleWorker entry — drains ConnectRequest items from bleWorkQueue
// one at a time and runs the full connect resolution on its own
// task. Producer set: cmdConnect, buttonHandler, autoConnectDevice,
// reconnectLostSensors.
//
// Per §Decisions Q1: a request for a slot already in REQUESTED /
// CONNECTING / READY is rejected at the enqueue site
// (cmdConnect's busy guard). The worker only ever observes a slot
// it can CAS from REQUESTED → CONNECTING via
// tryAcquireConnecting(); cmdCancelConnect can race-win that CAS
// (REQUESTED → IDLE) in which case the worker dequeues a stale
// request and skips.
//
// Per §Decisions Q3: state transitions piggyback on T_DEV_LIST.
// We emit it once after the CONNECTING CAS so the host can render
// a "connecting" affordance while the BLE handshake settles;
// publishConnectResult emits it again at the tail (READY on ok,
// IDLE on failure).
static void bleWorkerEntry(void *)
{
    for (;;)
    {
        ConnectRequest req{};
        if (xQueueReceive(bleWorkQueue, &req, portMAX_DELAY) != pdTRUE)
            continue;

        // CAS REQUESTED → CONNECTING. If cmdCancelConnect won the race
        // and already flipped the slot to IDLE, the CAS fails and we
        // skip — cmdCancelConnect already emitted the IDLE T_DEV_LIST
        // so no further wire activity is needed.
        VernierAdapter &dev = slots[req.slot];
        if (!dev.tryAcquireConnecting())
        {
            log_i("bleWorker: slot %u cancelled, skipping connect",
                  (unsigned)req.slot);
            continue;
        }
        // A background attempt whose slot was disconnected or forgotten
        // after reconnectLostSensors queued it must not bring the sensor
        // back. Checked after the claim: a C_DISCONNECT / C_FORGET that
        // lands later stores IDLE, and publishConnectResult's
        // CONNECTING → READY CAS then abandons the connect.
        if (req.origin == ConnectOrigin::BACKGROUND
            && dev.reconnectState() == VernierAdapter::ReconnectState::OFF)
        {
            log_i("bleWorker: slot %u no longer reconnecting, skipping",
                  (unsigned)req.slot);
            dev.setState(VernierAdapter::ConnState::IDLE);
            continue;
        }
        announceIfSilent();
        // While the slot waits for its lost sensor the host keeps
        // seeing it disconnected, so an attempt for it announces nothing.
        if (dev.reconnectState() != VernierAdapter::ReconnectState::ARMED)
            emitDevList();
        publishConnectResult(req.slot, dev.connect(req.force), req.origin);
    }
}

void bleWorkerStart()
{
    bleWorkQueue = xQueueCreate(VERNIER_MAX_SLOTS, sizeof(ConnectRequest));
    configASSERT(bleWorkQueue);
    const BaseType_t ok = xTaskCreate(bleWorkerEntry, "BleWorker",
                                      TASK_STACK_BLE_HEAVY, nullptr,
                                      HANDLER_TASK_PRIO,
                                      &bleWorkerTaskHandle);
    configASSERT(ok == pdPASS);
}

// Marshal slot table from slots[] into UartAdapter::DevListEntry[]
// then push T_DEV_LIST. Per design D4 this is auto-emitted on every
// connect / disconnect event so the host's slot table stays in sync
// without polling.
//
// "connected" reflects isReady() (= D2PIO handshake complete + channel
// mask populated) — the same gate the rest of the firmware uses for
// "this slot is producing samples".
//
// A slot waiting for its lost sensor (ReconnectState ARMED) is listed
// IDLE even while a background attempt has it REQUESTED or CONNECTING:
// to the host it stays disconnected until the sensor is READY again.
static void emitDevList()
{
    xSemaphoreTake(devListMutex, portMAX_DELAY);
    UartAdapter::DevListEntry entries[VERNIER_MAX_SLOTS];
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        // Empty / disconnected slots return "" for name + order so the
        // host doesn't read a stale device name from a previous session
        // (VernierAdapter::deviceName/orderCode survive disconnect).
        auto st = slots[i].state();
        if (st != VernierAdapter::ConnState::READY
            && slots[i].reconnectState() == VernierAdapter::ReconnectState::ARMED)
            st = VernierAdapter::ConnState::IDLE;
        const bool live = (st == VernierAdapter::ConnState::READY);
        entries[i].dev       = i;
        entries[i].name      = live ? slots[i].deviceName() : "";
        entries[i].order     = live ? slots[i].orderCode() : "";
        entries[i].connected = live;
        // G008 wire surface — ConnState numeric value. Host distinguishes
        // CONNECTING from IDLE for kid-UX spinner rendering.
        entries[i].state     = static_cast<uint8_t>(st);
    }
    uart.sendDevList(entries, VERNIER_MAX_SLOTS);
    xSemaphoreGive(devListMutex);
}

void autoConnectDevice()
{
    // Normally the host's boot C_SET_PERIOD arms startAutoConnect. If
    // those commands were lost (co-MCU booted late, or reset alone while
    // the GoGo kept running) or all NACKed, start anyway once the host
    // has had AUTO_CONNECT_HOSTLESS_DELAY_MS to speak. loop() is the only
    // caller, so the function-local flag needs no synchronisation.
    static bool started = false;
    if (!started && millis() >= AUTO_CONNECT_HOSTLESS_DELAY_MS
        && foundSavedDevice.load(std::memory_order_acquire))
    {
        log_i("auto-connect: no C_SET_PERIOD after %u ms, starting without the host",
              (unsigned)AUTO_CONNECT_HOSTLESS_DELAY_MS);
        startAutoConnect.store(true, std::memory_order_relaxed);
    }

    if (!startAutoConnect.load(std::memory_order_acquire))
        return;
    startAutoConnect.store(false, std::memory_order_relaxed);
    // Only the first pass is the boot connect, whose failure arms the
    // background reconnect. A later pass (the host re-sending
    // C_SET_PERIOD) counts as a user connect, so it never arms a slot
    // the user disconnected.
    const ConnectOrigin origin = started ? ConnectOrigin::USER : ConnectOrigin::BOOT;
    started = true;

    // Per design D6: walk slots in order, queue a connect for each
    // slot with a saved NVS name. bleWorker drains them sequentially
    // (one in-flight at a time — BLE controller serialises connects
    // anyway). force=false so the worker scans for the slot's
    // saved name rather than the highest-RSSI nearby device.
    //
    // The host sends C_SET_PERIOD once per slot at boot, each
    // re-arming startAutoConnect, so this can run several times. A
    // slot already claimed by an earlier pass fails enqueueConnect's
    // CAS and is skipped silently — that's the expected idempotent
    // no-op, not an error.
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (slotHasSavedDevice[i].load(std::memory_order_relaxed))
        {
            if (enqueueConnect(i, /*force=*/false, NO_HOST_REQUEST, origin))
            {
                log_i("auto-connect: queued slot %u (saved device)",
                      (unsigned)i);
            }
        }
    }
}

void releaseLostLinks()
{
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        // The user connected this sensor and neither disconnected nor
        // forgot it: keep its saved name and look for it in the
        // background. Armed inside releaseIfLinkLost's lock, so a
        // C_DISCONNECT / C_FORGET waiting on that lock disarms the slot
        // after it, and before the push so the list shows it IDLE.
        // reconnectLostSensors() starts the retry schedule.
        const bool saved = slotHasSavedDevice[i].load(std::memory_order_relaxed);
        if (!slots[i].releaseIfLinkLost(/*arm=*/saved))
            continue;
        if (saved)
            log_w("slot %u: BLE link lost, slot freed, reconnecting in the background",
                  (unsigned)i);
        else
            log_w("slot %u: BLE link lost, slot freed", (unsigned)i);
        emitDevList();
    }
}

void reconnectLostSensors()
{
    // One rescan at a time, and only while bleWorker is idle: a user or
    // boot connect never waits behind more than the one attempt already
    // running, and a sensor that stays away costs one 5 s scan per gap.
    bool workerBusy = false;
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        const auto st = slots[i].state();
        if (st == VernierAdapter::ConnState::REQUESTED
            || st == VernierAdapter::ConnState::CONNECTING)
            workerBusy = true;
    }

    const uint32_t now = millis();
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        ReconnectSchedule &s = reconnectSchedule[i];
        const auto rs = slots[i].reconnectState();
        // The slot left OFF since the previous pass: armed by a link
        // loss (releaseLostLinks), a failed boot connect (bleWorker), or
        // already ADOPTED by a user connect on the UART task. Start the
        // schedule here, whoever armed it, so the first rescan waits
        // RECONNECT_FIRST_RETRY_MS and the backoff never starts at 0.
        if (s.seen == VernierAdapter::ReconnectState::OFF
            && rs != VernierAdapter::ReconnectState::OFF)
        {
            s.armedAtMs     = now;
            s.backoffMs     = RECONNECT_FIRST_RETRY_MS;
            s.nextAttemptMs = now + RECONNECT_FIRST_RETRY_MS;
            s.inFlight      = false;
        }
        s.seen = rs;
        if (rs != VernierAdapter::ReconnectState::ARMED)
        {
            // OFF, or ADOPTED by a user connect. If a failed user
            // connect re-arms the slot, the next rescan waits one gap.
            s.inFlight      = false;
            s.nextAttemptMs = now + s.backoffMs;
            continue;
        }
        if (s.inFlight)
        {
            // Still queued or connecting. READY ends the reconnect
            // (publishConnectResult sets OFF), handled above next pass.
            if (slots[i].state() != VernierAdapter::ConnState::IDLE)
                continue;
            s.inFlight  = false;
            s.backoffMs = s.backoffMs * RECONNECT_BACKOFF_FACTOR;
            if (s.backoffMs > RECONNECT_BACKOFF_MAX_MS)
                s.backoffMs = RECONNECT_BACKOFF_MAX_MS;
            s.nextAttemptMs = now + s.backoffMs;
            log_i("slot %u: reconnect attempt failed, next in %u ms",
                  (unsigned)i, (unsigned)s.backoffMs);
            continue;
        }
        // Give up between two attempts once the sensor has been away
        // for RECONNECT_GIVE_UP_MS. The slot keeps its saved name; the
        // host already shows it disconnected, so nothing is pushed.
        if (RECONNECT_GIVE_UP_MS != 0 && now - s.armedAtMs >= RECONNECT_GIVE_UP_MS
            && slots[i].tryGiveUpReconnect())
        {
            s.seen = VernierAdapter::ReconnectState::OFF;
            log_w("slot %u: sensor not back after %u ms, background reconnect stopped",
                  (unsigned)i, (unsigned)(now - s.armedAtMs));
            continue;
        }
        if (workerBusy || static_cast<int32_t>(now - s.nextAttemptMs) < 0)
            continue;
        if (enqueueConnect(i, /*force=*/false, NO_HOST_REQUEST, ConnectOrigin::BACKGROUND))
        {
            s.inFlight = true;
            workerBusy = true;
            log_i("slot %u: reconnect attempt queued", (unsigned)i);
        }
    }
}

void buttonHandler()
{
    if (digitalRead(BOOT_BUTTON_PIN) == LOW)
    {
        if (prevButtonEvent == BUTTON_PRESS)
        {
            if ((millis() - startPressTime) > BUTTON_LONG_PRESS_MS)
            {
                prevButtonEvent = BUTTON_LONG_PRESS;

                // Long-press: disconnect ALL active slots. Host UI
                // disconnect-by-slot routes through C_DISCONNECT
                // instead; the BOOT button is a coarse "tear
                // everything down" escape hatch (e.g. when the
                // host MCU is unavailable or stuck).
                log_i("button long-press: disconnect all slots");
                bool any = false;
                for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
                {
                    // disconnect(true) disarms before the teardown, so no
                    // background attempt is queued for a slot this press
                    // just emptied; an empty slot waiting for its sensor
                    // is only disarmed.
                    if (isSlotOccupied(i))
                    {
                        slots[i].disconnect(/*disarm=*/true);
                        any = true;
                    }
                    else
                    {
                        slots[i].setReconnectState(VernierAdapter::ReconnectState::OFF);
                    }
                }
                if (any) emitDevList();
            }
        }
        else if (prevButtonEvent == BUTTON_RELEASE)
        {
            prevButtonEvent = BUTTON_PRESS;
            startPressTime = millis();

            // Short-press: queue a proximity connect on the lowest
            // free slot. Same first-free allocation as C_CONNECT,
            // force=true so we probe nearby instead of re-using the
            // slot's saved name. Goes through bleWorker so the
            // ~5–7 s BLE handshake doesn't stall loop(); bleWorker
            // pushes T_DEVINFO/T_FIELDS/etc. + T_DEV_LIST when the
            // connect resolves.
            int8_t target_slot = firstFreeSlot();
            if (target_slot < 0)
            {
                log_w("button: all slots full, ignoring connect press");
            }
            else
            {
                const uint8_t slot = static_cast<uint8_t>(target_slot);
                switch (requestUserConnect(slot, NO_HOST_REQUEST))
                {
                case UserConnect::QUEUED:
                    log_i("button: connecting nearby device → slot %u", (unsigned)slot);
                    break;
                case UserConnect::TOOK_OVER:
                    log_i("button: slot %u takes over its reconnect attempt", (unsigned)slot);
                    emitDevList();
                    break;
                case UserConnect::BUSY:
                    // firstFreeSlot returned an IDLE slot but another
                    // producer claimed it before our CAS — rare race,
                    // benign: the slot is already being connected.
                    log_i("button: slot %u already claimed, skipping", (unsigned)slot);
                    break;
                }
            }
        }
    }
    else
    {
        if (prevButtonEvent != BUTTON_RELEASE)
        {
            prevButtonEvent = BUTTON_RELEASE;
        }
    }
}

// ---- per-command handlers ----------------------------------------------
// Each handler takes the parsed MsgPack root and the host's request seq
// and is responsible for emitting its own ack and any follow-up events.
// Returning early on a NACK is fine — the dispatcher does no work after
// the handler returns.

static void cmdConnect(JsonVariantConst root, uint32_t req)
{
    // Slot allocation (unchanged from pre-G007):
    //   - If host specifies `dev`, target THAT slot.
    //   - Otherwise fall back to first-free per design D2.
    // The G007 cutover moves the actual dev.connect() onto bleWorker
    // and replaces the synchronous terminal ACK with an early-ACK
    // ("queued") here at enqueue time. Host correlates outcome via
    // the auto-pushed T_DEV_LIST (§Decisions Q3 piggyback).
    int8_t target_slot = -1;
    const bool hasDev = !root["dev"].isNull();
    if (hasDev)
    {
        // Echo `dev` on every NACK so the host can roll back the
        // per-slot CONNECTING animation against the right card.
        const int requestedRaw = root["dev"].as<int>();
        if (!slotInRange(requestedRaw))
        {
            log_w("C_CONNECT NACK: dev=%d out of range", requestedRaw);
            uart.sendAck(req, false, "dev out of range",
                         static_cast<int16_t>(requestedRaw));
            return;
        }
        target_slot = static_cast<int8_t>(requestedRaw);
    }
    else
    {
        target_slot = firstFreeSlot();
    }
    if (target_slot < 0)
    {
        uart.sendAck(req, false, "all slots full");
        return;
    }
    const uint8_t slot = static_cast<uint8_t>(target_slot);

    // force=true: probe nearby for a NEW pairing rather than the slot's
    // saved name. Host C_CONNECT semantics = "connect to whatever's
    // around" (or the host-specified slot's nearby device). A slot that
    // waits for its lost sensor is the host's to connect: the host sees
    // it disconnected (see requestUserConnect).
    switch (requestUserConnect(slot, req))
    {
    case UserConnect::QUEUED:
        log_i("C_CONNECT: slot %u queued (%s)", (unsigned)slot,
              hasDev ? "host-specified" : "first-free");
        // Early-ACK per §Decisions Q3 / §Wire protocol additions.
        uart.sendAck(req, true, "queued", static_cast<int16_t>(slot));
        break;
    case UserConnect::TOOK_OVER:
        // A background attempt for the slot's saved sensor is queued or
        // connecting. It answers this request; its outcome arrives in
        // T_DEV_LIST like any connect. The push below lifts the IDLE
        // mask, so the list now shows the attempt's real state.
        log_i("C_CONNECT: slot %u takes over its reconnect attempt", (unsigned)slot);
        uart.sendAck(req, true, "reconnecting", static_cast<int16_t>(slot));
        emitDevList();
        break;
    case UserConnect::BUSY:
        // READY, or connecting for another producer (auto-connect,
        // button, an earlier C_CONNECT). NACK busy so the host rolls
        // back its CONNECTING animation.
        log_w("C_CONNECT NACK: slot %u busy", (unsigned)slot);
        uart.sendAck(req, false, "slot busy", static_cast<int16_t>(slot));
        break;
    }
}

static void cmdDisconnect(JsonVariantConst root, uint32_t req)
{
    // v2 wire: `dev` selects which slot to disconnect. Absent (v1
    // host) → default 0, identical to legacy behaviour.
    uint8_t dev = root["dev"] | (uint8_t)0;
    if (!slotInRange(dev))
    {
        uart.sendAck(req, false, "dev out of range",
                     static_cast<int16_t>(dev));
        return;
    }
    // An explicit disconnect ends the background reconnect. Disarmed
    // under the lifecycle lock before the teardown, so the worker skips
    // an attempt already queued and a concurrent link-loss release
    // cannot re-arm the slot after this.
    slots[dev].disconnect(/*disarm=*/true);
    uart.sendAck(req, true, "disconnected", static_cast<int16_t>(dev));
    // D4: T_DEV_LIST after disconnect so host's slot table reflects
    // the slot becoming free.
    emitDevList();
}

static void cmdForget(JsonVariantConst root, uint32_t req)
{
    // Drop the BLE link AND clear the slot's NVS deviceName key so the
    // slot won't auto-reconnect on next boot. Host's "Forget" action in
    // Vernier > Settings.
    uint8_t dev = root["dev"] | (uint8_t)0;
    if (!slotInRange(dev))
    {
        uart.sendAck(req, false, "dev out of range",
                     static_cast<int16_t>(dev));
        return;
    }
    if (!slotHasSavedDevice[dev].load(std::memory_order_relaxed) && !isSlotOccupied(dev))
    {
        // No persistent state and no live link — Forget is a no-op.
        // Reply ok so the host can still bounce the kid back to the
        // main view, but skip the NVS write entirely. Still disarm, so
        // a Forget always leaves the slot with no background reconnect.
        slots[dev].setReconnectState(VernierAdapter::ReconnectState::OFF);
        uart.sendAck(req, true, "already empty", static_cast<int16_t>(dev));
        emitDevList();
        return;
    }
    // Disconnect first so the device's BLE link drops cleanly. Safe to
    // call when already disconnected (no-op). A forgotten sensor is not
    // reconnected in the background either: disconnect(true) disarms
    // under the lifecycle lock, after any link-loss release that holds
    // it, so that release cannot re-arm the slot.
    slots[dev].disconnect(/*disarm=*/true);
    // Clear NVS deviceName{dev} so autoConnectDevice on next boot skips
    // this slot.
    {
        char key[NVS_KEY_MAX_LEN];
        snprintf(key, sizeof(key), NVS_KEY_DEVICE_NAME_FMT, (unsigned)dev);
        NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false);
        if (nvs) preferences.remove(key);
    }
    slotHasSavedDevice[dev].store(false, std::memory_order_relaxed);
    uart.sendAck(req, true, "forgotten", static_cast<int16_t>(dev));
    emitDevList();
}

// Re-send the per-slot frames (T_DEVINFO, T_DEVSTATS, T_FIELDS) of every
// READY slot. Same frames and order as publishConnectResult's burst.
static void resendReadySlotFrames()
{
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
        if (slots[i].isReady())
            sendSlotFrames(i);
}

static void cmdDevList(JsonVariantConst /*root*/, uint32_t req)
{
    // D4: explicit host-side query for the slot table. Always ack'd
    // BEFORE the T_DEV_LIST push so seq ordering on the wire matches
    // the host's request → ack → push expectation.
    uart.sendAck(req, true, "dev list");
    emitDevList();
    // A host asks for the list when it needs to rebuild its slot table
    // (it just booted, or it dropped a silent slot). A host that
    // restarted alone missed the connect burst and would otherwise wait
    // for the next 50-sample T_FIELDS re-emit, so give it the per-slot
    // frames behind the list. Only host-requested lists carry them; the
    // event-driven pushes (connect, disconnect, forget) do not.
    resendReadySlotFrames();
}

static void cmdHello(bool firstContact)
{
    // C_HELLO: a rebooted host asking who is on the link. No T_ACK: the
    // T_HELLO is the answer. On first contact dispatchHostCommand has
    // already sent it, so don't send it twice.
    if (!firstContact)
        uart.sendHello();
}

static void cmdCancelConnect(JsonVariantConst root, uint32_t req)
{
    // C_CANCEL_CONNECT (G007 §Decisions Q4). Aborts a queued C_CONNECT
    // for `dev` — e.g. the kid pressed the wrong slot card and wants
    // to cancel before bleWorker picks the request up.
    //
    // Only effective in REQUESTED state (tryCancelConnect CAS
    // REQUESTED → IDLE). CONNECTING means bleWorker is mid-handshake
    // (NimBLE has no in-flight cancel; cooperative cancel during
    // scan is out of scope — see release-2.1.0.md §Out of scope).
    // READY / IDLE mean there's nothing to cancel — use
    // C_DISCONNECT for an established link.
    int dev_raw = root["dev"] | -1;
    if (!slotInRange(dev_raw))
    {
        uart.sendAck(req, false, "dev out of range",
                     static_cast<int16_t>(dev_raw));
        return;
    }
    const uint8_t dev = static_cast<uint8_t>(dev_raw);
    // CAS REQUESTED → IDLE. Loses cleanly if bleWorker already CAS'd
    // REQUESTED → CONNECTING — without the atomic transition a stale
    // state read could clobber the worker's CONNECTING and leave it
    // dispatching dev.connect() on a slot the host believes is idle.
    if (!slots[dev].tryCancelConnect())
    {
        uart.sendAck(req, false, "not pending",
                     static_cast<int16_t>(dev));
        return;
    }
    // A cancelled user connect that took over a background reconnect
    // leaves the slot waiting for its saved sensor again.
    slots[dev].tryRearmAdoptedReconnect();
    log_i("C_CANCEL_CONNECT: slot %u cancelled", (unsigned)dev);
    uart.sendAck(req, true, "cancelled", static_cast<int16_t>(dev));
    emitDevList();
}

static void cmdSetPeriod(JsonVariantConst root, uint32_t req)
{
    // v2 wire (per D5 A): `dev` selects which slot's period to set —
    // each slot keeps its own rate. Absent (v1 host) → default 0.
    uint8_t dev = root["dev"] | (uint8_t)0;
    if (!slotInRange(dev))
    {
        uart.sendAck(req, false, "dev out of range",
                     static_cast<int16_t>(dev));
        return;
    }
    // Accept the host's value in u32 first so a 90s/120s setting
    // (mentioned as a use case in CLAUDE.md's adaptive-tick comment)
    // doesn't get silently mod-2^16-truncated at the JSON cast.
    uint32_t period32 = root["period_ms"] | (uint32_t)slots[dev].samplingPeriod();
    if (period32 == 0)
        period32 = VERNIER_DEFAULT_PERIOD_MS;
    if (period32 > MAX_PERIOD_MS)
    {
        uart.sendAck(req, false, "period exceeds 16-bit range",
                     static_cast<int16_t>(dev));
        return;
    }
    // Reject sub-min periods. GoGoVernier would forward them to the
    // device which then either rejects with an error frame or thrashes
    // start/stop trying to honour them — neither helps the host. Surface
    // it as a NACK so a misbehaving host learns about its own bug.
    if (period32 < VERNIER_MIN_PERIOD_MS)
    {
        uart.sendAck(req, false, "period below min",
                     static_cast<int16_t>(dev));
        return;
    }
    uint16_t period = static_cast<uint16_t>(period32);
    slots[dev].setSamplingRate(period);
    uart.sendAck(req, true, "rate set", static_cast<int16_t>(dev));

    // Start auto-connect after the host sets sampling rate at boot.
    // acquire-load foundSavedDevice so we see the slotHasSavedDevice[]
    // fills that preceded its release-store; then release-store
    // startAutoConnect so autoConnectDevice's acquire-load sees the
    // same fully-populated array.
    if (foundSavedDevice.load(std::memory_order_acquire))
    {
        startAutoConnect.store(true, std::memory_order_release);
    }
}

void dispatchHostCommand(JsonVariantConst root)
{
    // Boot handshake on first host contact: T_HELLO (peer identity +
    // proto version) then T_STATUS (peer up). If it already went out
    // unprompted, the host may have missed it and the per-slot frames
    // that followed, so repeat it and re-send every connected slot
    // right behind it; the host's T_HELLO reset is undone in the same
    // burst. On a co-MCU reboot the state resets and the next host
    // command re-announces (host + co-MCU normally share board power, so
    // they reboot together; a GoGo-only reboot gets no new T_HELLO).
    const uint8_t  c   = root["c"]   | 0;
    const uint32_t req = root["seq"] | 0;

    const HostLink prev = hostLink.exchange(HostLink::HEARD, std::memory_order_relaxed);
    const bool firstContact = prev != HostLink::HEARD;
    if (firstContact)
    {
        sendBootHandshake();
        // C_DEV_LIST answers with the list and the slot frames itself.
        if (prev == HostLink::ANNOUNCED && c != UartAdapter::C_DEV_LIST)
        {
            resendReadySlotFrames();
            emitDevList();
        }
    }

    switch (c)
    {
    case UartAdapter::C_CONNECT:        cmdConnect(root, req);        break;
    case UartAdapter::C_DISCONNECT:     cmdDisconnect(root, req);     break;
    case UartAdapter::C_FORGET:         cmdForget(root, req);         break;
    case UartAdapter::C_DEV_LIST:       cmdDevList(root, req);        break;
    case UartAdapter::C_SET_PERIOD:     cmdSetPeriod(root, req);      break;
    case UartAdapter::C_CANCEL_CONNECT: cmdCancelConnect(root, req);  break;
    case UartAdapter::C_HELLO:          cmdHello(firstContact);       break;
    default:                        uart.sendAck(req, false, "unknown command"); break;
    }
}
