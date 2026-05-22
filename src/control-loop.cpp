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
extern volatile bool startAutoConnect;
extern volatile bool foundSavedDevice;
extern volatile bool slotHasSavedDevice[VERNIER_MAX_SLOTS];

namespace {

// 16-bit period overflow guard — wire protocol uses uint16_t. Local to
// this TU; the only caller is cmdSetPeriod.
constexpr uint32_t MAX_PERIOD_MS   = 0xFFFFu;

// Sentinel for connectAndReport's "no host request to ack" case.
constexpr uint32_t NO_HOST_REQUEST = 0xFFFFFFFFu;

// Button polling state. Function-local before the move; kept here so
// only this TU touches them.
unsigned long startPressTime  = 0;
ButtonEvent   prevButtonEvent = BUTTON_RELEASE;

// bleWorker queue config (G006). Queue depth matches the per-slot
// natural bound — one in-flight connect per slot at most. Stack +
// priority come from main.h (TASK_STACK_BLE_HEAVY, HANDLER_TASK_PRIO)
// so they stay in sync with uartHandler's matching values; see the
// G006 review note.
constexpr UBaseType_t BLE_WORKER_QUEUE_DEPTH = VERNIER_MAX_SLOTS;

// ConnectRequest queued by enqueueConnect, drained by bleWorkerEntry.
// Layout on RV32: u8 slot + bool force + 2 B pad + u32 req_seq = 8 B.
struct ConnectRequest {
    uint8_t  slot;
    bool     force;
    uint32_t req_seq;   // NO_HOST_REQUEST when not host-driven.
};

QueueHandle_t bleWorkQueue = nullptr;
TaskHandle_t  bleWorkerTaskHandle = nullptr;

}  // namespace

static void emitDevList();           // forward decl — defined below for grouping
static void publishConnectResult(uint8_t slot, bool ok);

// enqueueConnect(slot, force, req_seq):
//   Single producer-side helper used by cmdConnect, buttonHandler
//   and autoConnectDevice. Caller has already validated
//   slotInRange(slot) and that slots[slot].state() is IDLE or
//   FAILED (the busy-guard from §Decisions Q1). Flips the slot to
//   REQUESTED so a concurrent C_CONNECT for the same slot finds it
//   busy, then xQueueSends a ConnectRequest. Rolls the slot state
//   back to IDLE if the queue is full or unavailable. Returns true
//   on successful enqueue.
//
//   req_seq is carried through to the worker for log/trace
//   correlation; bleWorker no longer dispatches an ACK from
//   publishConnectResult (early-ACK happens in cmdConnect at
//   enqueue time, host learns outcome via T_DEV_LIST piggyback).
static bool enqueueConnect(uint8_t slot, bool force, uint32_t req_seq)
{
    slots[slot].setState(VernierAdapter::ConnState::REQUESTED);
    ConnectRequest req{slot, force, req_seq};
    if (!bleWorkQueue || xQueueSend(bleWorkQueue, &req, 0) != pdTRUE)
    {
        slots[slot].setState(VernierAdapter::ConnState::IDLE);
        return false;
    }
    return true;
}

// publishConnectResult(slot, ok):
//   Side-effect block that runs after dev.connect() resolves.
//   - On success: refresh status, kick off streaming, emit the
//     5-frame burst (T_HELLO, T_STATUS, T_DEVINFO, T_DEVSTATS,
//     T_FIELDS), persist the slot's device name in NVS, and flag
//     the slot for auto-connect on the next boot.
//   - On any outcome: push T_DEV_LIST so the host's slot table
//     stays in sync (D4). The host's terminal ACK is no longer
//     emitted here — cmdConnect ACKs at enqueue time (early-ACK,
//     §Decisions Q3 piggyback); outcome correlates via T_DEV_LIST.
// Caller is responsible for the dev.connect() call itself + the
// slot-range guard. Safe to call from any task that owns access
// to the shared globals (uart's send mutex, nvsMutex via NvsScope).
static void publishConnectResult(uint8_t slot, bool ok)
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

        uart.sendHello();                                     // global — no dev
        uart.sendStatus(true, UartAdapter::CORE_STATE_READY); // global — no dev (per protocol-v2 spec)
        uart.sendDeviceInfo(dev.deviceName(), dev.orderCode(), dev.serialNumber(), slot);
        // H6: hold the per-slot status mutex across the multi-field
        // read so vernierHandler's 10 s refresh can't tear the
        // battery/charge/rssi triple mid-evaluation. Recursive mutex
        // means the getDeviceInfo() above re-entered cleanly.
        dev.lockStatus();
        uart.sendDeviceStats(dev.batteryPercent(), dev.chargeState(), dev.rssi(), dev.droppedSamples(), slot);
        dev.unlockStatus();
        sendDeviceFieldsFor(uart, dev, slot);

        // Persist last connected device name to NVS under the per-slot
        // key (deviceName0..N). Read back at boot so each slot can
        // auto-reconnect to its previously paired device.
        {
            char key[NVS_KEY_MAX_LEN];
            snprintf(key, sizeof(key), NVS_KEY_DEVICE_NAME_FMT, (unsigned)slot);
            log_d("NVS slot %u: saving %s = %s", (unsigned)slot, key, dev.deviceName());
            {
                NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false);
                if (nvs) preferences.putString(key, dev.deviceName());
            }
            // Mark this slot as eligible for future auto-connect on
            // boot — even though we won't reach autoConnectDevice
            // again until the next reboot + C_SET_PERIOD trigger.
            // Cross-task publish (see CLAUDE.md Code Quality Rules):
            // fill the per-slot flag, fence, then flip the global
            // "any saved?" flag the readers (cmdSetPeriod /
            // autoConnectDevice) poll.
            slotHasSavedDevice[slot] = true;
            __asm__ volatile("" ::: "memory");
            foundSavedDevice = true;
        }
    }

    // Per design D4: T_DEV_LIST auto-pushed after every connect attempt
    // (success OR failure — the failure case still tells the host that
    // its connect didn't take, so the slot table stays accurate).
    emitDevList();
}

// bleWorker entry — drains ConnectRequest items from bleWorkQueue
// one at a time and runs the full connect resolution on its own
// task. Producer set (G007): cmdConnect, buttonHandler,
// autoConnectDevice. In G006 the queue stays empty so the task
// just parks on the receive.
//
// Per §Decisions Q1: a request for a slot already in REQUESTED /
// CONNECTING is rejected at the enqueue site (cmdConnect's busy
// guard), so the worker is the SOLE state-machine driver from
// CONNECTING → READY / FAILED → IDLE.
//
// Per §Decisions Q3: state transitions piggyback on T_DEV_LIST.
// We emit it once before dev.connect() (CONNECTING transition) so
// the host can render a "connecting" affordance while the BLE
// handshake settles; publishConnectResult emits it again at the
// tail (READY / FAILED transition).
static void bleWorkerEntry(void *)
{
    for (;;)
    {
        ConnectRequest req{};
        if (xQueueReceive(bleWorkQueue, &req, portMAX_DELAY) != pdTRUE)
            continue;

        // Defensive — producers validate slotInRange before enqueueing.
        if (!slotInRange(req.slot))
        {
            log_e("bleWorker: dropped out-of-range slot=%u",
                  (unsigned)req.slot);
            continue;
        }

        // C_CANCEL_CONNECT (G007 §Decisions Q4) flips the slot back to
        // IDLE while the request is still queued. We dequeue but skip
        // the connect — cmdCancelConnect has already pushed the IDLE
        // transition out via T_DEV_LIST, so no further wire activity
        // needed here.
        if (slots[req.slot].state() != VernierAdapter::ConnState::REQUESTED)
        {
            log_i("bleWorker: slot %u cancelled, skipping connect",
                  (unsigned)req.slot);
            continue;
        }

        slots[req.slot].setState(VernierAdapter::ConnState::CONNECTING);
        emitDevList();

        const bool ok = slots[req.slot].connect(req.force);

        slots[req.slot].setState(ok ? VernierAdapter::ConnState::READY
                                    : VernierAdapter::ConnState::FAILED);
        // publishConnectResult no longer dispatches the host ACK —
        // cmdConnect's early-ACK ("queued") covers it and the host
        // correlates outcome via the T_DEV_LIST emit at the tail
        // (Q3 piggyback). req.req_seq survives in the ConnectRequest
        // struct for future log/trace correlation but is unused here.
        publishConnectResult(req.slot, ok);

        // FAILED is transient — drop back to IDLE so the next
        // C_CONNECT on this slot isn't blocked by the busy-guard.
        if (!ok) slots[req.slot].setState(VernierAdapter::ConnState::IDLE);
    }
}

void bleWorkerStart()
{
    // Idempotent against repeated calls AND against partial-failure
    // restart. Order: queue first, then task. If the task create
    // fails after the queue is up, free the queue so the next
    // bleWorkerStart() rebuilds from scratch.
    if (bleWorkerTaskHandle) return;
    if (!bleWorkQueue)
    {
        bleWorkQueue = xQueueCreate(BLE_WORKER_QUEUE_DEPTH,
                                    sizeof(ConnectRequest));
        if (!bleWorkQueue)
        {
            log_e("bleWorkerStart: xQueueCreate failed");
            return;
        }
    }
    const BaseType_t ok = xTaskCreate(bleWorkerEntry, "BleWorker",
                                      TASK_STACK_BLE_HEAVY, nullptr,
                                      HANDLER_TASK_PRIO,
                                      &bleWorkerTaskHandle);
    if (ok != pdPASS)
    {
        log_e("bleWorkerStart: xTaskCreate failed");
        vQueueDelete(bleWorkQueue);
        bleWorkQueue = nullptr;
        bleWorkerTaskHandle = nullptr;
    }
}

// Marshal slot table from slots[] into UartAdapter::DevListEntry[]
// then push T_DEV_LIST. Per design D4 this is auto-emitted on every
// connect / disconnect event so the host's slot table stays in sync
// without polling.
//
// "connected" reflects isReady() (= D2PIO handshake complete + channel
// mask populated) — the same gate the rest of the firmware uses for
// "this slot is producing samples".
static void emitDevList()
{
    UartAdapter::DevListEntry entries[VERNIER_MAX_SLOTS];
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        // Empty / disconnected slots return "" for name + order so the
        // host doesn't read a stale device name from a previous session
        // (VernierAdapter::deviceName/orderCode survive disconnect).
        const bool live = slots[i].isReady();
        entries[i].dev       = i;
        entries[i].name      = live ? slots[i].deviceName() : "";
        entries[i].order     = live ? slots[i].orderCode() : "";
        entries[i].connected = live;
    }
    uart.sendDevList(entries, VERNIER_MAX_SLOTS);
}

void autoConnectDevice()
{
    if (!startAutoConnect)
        return;
    startAutoConnect = false;

    // Per design D6: walk slots in order, queue a connect for each
    // slot with a saved NVS name. bleWorker drains them sequentially
    // (one in-flight at a time — BLE controller serialises connects
    // anyway). force=false so the worker scans for the slot's
    // saved name rather than the highest-RSSI nearby device.
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (slotHasSavedDevice[i])
        {
            log_i("auto-connect: queueing slot %u (saved device)",
                  (unsigned)i);
            if (!enqueueConnect(i, /*force=*/false, NO_HOST_REQUEST))
            {
                log_w("auto-connect: queue full, slot %u skipped",
                      (unsigned)i);
            }
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
                    if (isSlotOccupied(i))
                    {
                        slots[i].disconnect();
                        any = true;
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
            else if (!enqueueConnect(static_cast<uint8_t>(target_slot),
                                     /*force=*/true, NO_HOST_REQUEST))
            {
                log_w("button: queue full, slot %u",
                      (unsigned)target_slot);
            }
            else
            {
                log_i("button: connecting nearby device → slot %u",
                      (unsigned)target_slot);
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
        const uint8_t requested = static_cast<uint8_t>(requestedRaw);
        if (isSlotOccupied(requested))
        {
            log_w("C_CONNECT NACK: slot %u busy", (unsigned)requested);
            uart.sendAck(req, false, "slot busy",
                         static_cast<int16_t>(requested));
            return;
        }
        target_slot = static_cast<int8_t>(requested);
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

    // force=true: probe nearby for a NEW pairing rather than the slot's
    // saved name. Host C_CONNECT semantics = "connect to whatever's
    // around" (or the host-specified slot's nearby device).
    if (!enqueueConnect(static_cast<uint8_t>(target_slot), /*force=*/true, req))
    {
        log_w("C_CONNECT NACK: queue full, slot=%u", (unsigned)target_slot);
        uart.sendAck(req, false, "queue full",
                     static_cast<int16_t>(target_slot));
        return;
    }
    log_i("C_CONNECT: slot %u queued (%s)",
          (unsigned)target_slot,
          hasDev ? "host-specified" : "first-free");
    // Early-ACK per §Decisions Q3 / §Wire protocol additions.
    uart.sendAck(req, true, "queued", static_cast<int16_t>(target_slot));
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
    slots[dev].disconnect();
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
    if (!slotHasSavedDevice[dev] && !isSlotOccupied(dev))
    {
        // No persistent state and no live link — Forget is a no-op.
        // Reply ok so the host can still bounce the kid back to the
        // main view, but skip the NVS write entirely.
        uart.sendAck(req, true, "already empty", static_cast<int16_t>(dev));
        emitDevList();
        return;
    }
    // Disconnect first so the device's BLE link drops cleanly. Safe to
    // call when already disconnected (no-op).
    slots[dev].disconnect();
    // Clear NVS deviceName{dev} so autoConnectDevice on next boot skips
    // this slot.
    {
        char key[NVS_KEY_MAX_LEN];
        snprintf(key, sizeof(key), NVS_KEY_DEVICE_NAME_FMT, (unsigned)dev);
        NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false);
        if (nvs) preferences.remove(key);
    }
    slotHasSavedDevice[dev] = false;
    uart.sendAck(req, true, "forgotten", static_cast<int16_t>(dev));
    emitDevList();
}

static void cmdDevList(JsonVariantConst /*root*/, uint32_t req)
{
    // D4: explicit host-side query for the slot table. Always ack'd
    // BEFORE the T_DEV_LIST push so seq ordering on the wire matches
    // the host's request → ack → push expectation.
    uart.sendAck(req, true, "dev list");
    emitDevList();
}

static void cmdCancelConnect(JsonVariantConst root, uint32_t req)
{
    // C_CANCEL_CONNECT (G007 §Decisions Q4). Aborts a queued C_CONNECT
    // for `dev` — e.g. the kid pressed the wrong slot card and wants
    // to cancel before bleWorker picks the request up.
    //
    // Only effective in REQUESTED state. CONNECTING means bleWorker
    // is mid-handshake (NimBLE has no in-flight cancel; cooperative
    // cancel during scan is in §Out of scope for 2.1.0). READY /
    // IDLE / FAILED mean there's nothing to cancel — use
    // C_DISCONNECT for an established link.
    int dev_raw = root["dev"] | -1;
    if (!slotInRange(dev_raw))
    {
        uart.sendAck(req, false, "dev out of range",
                     static_cast<int16_t>(dev_raw));
        return;
    }
    const uint8_t dev = static_cast<uint8_t>(dev_raw);
    const auto st = slots[dev].state();
    if (st != VernierAdapter::ConnState::REQUESTED)
    {
        uart.sendAck(req, false, "not pending",
                     static_cast<int16_t>(dev));
        return;
    }
    // Flip to IDLE. bleWorker will eventually dequeue the queued
    // ConnectRequest, see state != REQUESTED, and skip the connect.
    // The queue slot is briefly held until that drain runs but the
    // user-visible state — and the busy-guard a follow-up C_CONNECT
    // sees — flips immediately.
    slots[dev].setState(VernierAdapter::ConnState::IDLE);
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

    // INFO: start auto-connect after gogo set sampling rate at boot.
    // Memory barrier so autoConnectDevice's reader sees a fully-
    // populated slotHasSavedDevice[] before the trigger flips.
    if (foundSavedDevice)
    {
        __asm__ volatile("" ::: "memory");
        startAutoConnect = true;
    }
}

void dispatchHostCommand(JsonVariantConst root)
{
    const uint8_t  c   = root["c"]   | 0;
    const uint32_t req = root["seq"] | 0;

    switch (c)
    {
    case UartAdapter::C_CONNECT:        cmdConnect(root, req);        break;
    case UartAdapter::C_DISCONNECT:     cmdDisconnect(root, req);     break;
    case UartAdapter::C_FORGET:         cmdForget(root, req);         break;
    case UartAdapter::C_DEV_LIST:       cmdDevList(root, req);        break;
    case UartAdapter::C_SET_PERIOD:     cmdSetPeriod(root, req);      break;
    case UartAdapter::C_CANCEL_CONNECT: cmdCancelConnect(root, req);  break;
    default:                        uart.sendAck(req, false, "unknown command"); break;
    }
}
