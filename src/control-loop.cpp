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

// bleWorker queue + task config (G006). Queue depth matches the
// per-slot natural bound — one in-flight connect per slot. The
// stack size mirrors UART_TASK_STACK in main.cpp because the worker
// inherits the same call depth (GoGoVernier::open → NimBLEScan +
// NimBLEClient discovery dives ~4 KB on its own).
constexpr UBaseType_t BLE_WORKER_QUEUE_DEPTH = VERNIER_MAX_SLOTS;
constexpr uint16_t    BLE_WORKER_STACK       = 6144;
// Same priority as uart/vernier handlers — same-priority round-robin
// keeps sample-stream throughput predictable; the plan's "priority
// between" suggestion was conditional on uart being bumped up, which
// isn't part of G006.
constexpr UBaseType_t BLE_WORKER_PRIO        = 1;

// ConnectRequest queued by enqueueConnect, drained by bleWorkerEntry.
// 12 bytes per item with natural alignment on RV32.
struct ConnectRequest {
    uint8_t  slot;
    bool     force;
    uint32_t req_seq;   // NO_HOST_REQUEST when not host-driven.
};

QueueHandle_t bleWorkQueue = nullptr;
TaskHandle_t  bleWorkerTaskHandle = nullptr;

}  // namespace

static void emitDevList();           // forward decl — defined below for grouping
static void publishConnectResult(uint8_t slot, bool ok, uint32_t req_seq);

// connectAndReport(slot, req_seq, force):
//   slot     — target slot in slots[].
//   req_seq  — host's C_CONNECT seq for the terminal T_ACK; pass
//              NO_HOST_REQUEST when not driven by a host command
//              (autoConnect, button) so we skip the ack send.
//   force    — when true, scan for the highest-RSSI nearby GDX device
//              regardless of the slot's saved _open_device. Used by
//              host C_CONNECT (probe for new pairing) and the BOOT
//              button. Auto-connect path passes false so it scans for
//              the slot's NVS-saved name.
//
// The body splits at dev.connect() — everything that runs after the
// connect call lives in publishConnectResult so a future bleWorker
// task (see .claude/plans/release-2.1.0.md §Design phase 1) can call
// it with a pre-computed result from a different task context. Today
// the call stays inline.
static bool connectAndReport(uint8_t slot,
                             uint32_t req_seq = NO_HOST_REQUEST,
                             bool force = false)
{
    if (!slotInRange(slot))
    {
        if (req_seq != NO_HOST_REQUEST)
            uart.sendAck(req_seq, false, "slot out of range");
        return false;
    }
    bool ok = slots[slot].connect(force);
    publishConnectResult(slot, ok, req_seq);
    return ok;
}

// publishConnectResult(slot, ok, req_seq):
//   Side-effect block that runs after dev.connect() resolves.
//   - On success: refresh status, kick off streaming, emit the
//     5-frame burst (T_HELLO, T_STATUS, T_DEVINFO, T_DEVSTATS,
//     T_FIELDS), persist the slot's device name in NVS, and flag
//     the slot for auto-connect on the next boot.
//   - On any outcome: emit T_ACK if req_seq is a real host
//     request, then push T_DEV_LIST so the host's slot table
//     stays in sync (D4).
// Caller is responsible for the dev.connect() call itself + the
// slot-range guard. Safe to call from any task that owns access
// to the shared globals (uart's send mutex, nvsMutex via NvsScope).
static void publishConnectResult(uint8_t slot, bool ok, uint32_t req_seq)
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

    if (req_seq != NO_HOST_REQUEST)
        uart.sendAck(req_seq, ok, ok ? nullptr : "connect failed", static_cast<int16_t>(slot));

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

        if (!slotInRange(req.slot))
        {
            if (req.req_seq != NO_HOST_REQUEST)
                uart.sendAck(req.req_seq, false, "slot out of range");
            continue;
        }

        slots[req.slot].setState(VernierAdapter::ConnState::CONNECTING);
        emitDevList();

        const bool ok = slots[req.slot].connect(req.force);

        slots[req.slot].setState(ok ? VernierAdapter::ConnState::READY
                                    : VernierAdapter::ConnState::FAILED);
        publishConnectResult(req.slot, ok, req.req_seq);

        // FAILED is a transient signal — once publishConnectResult has
        // NACKed the host, drop back to IDLE so the next C_CONNECT on
        // this slot isn't blocked by the busy-guard in cmdConnect.
        if (!ok) slots[req.slot].setState(VernierAdapter::ConnState::IDLE);
    }
}

void bleWorkerStart()
{
    if (bleWorkerTaskHandle) return;  // idempotent
    bleWorkQueue = xQueueCreate(BLE_WORKER_QUEUE_DEPTH, sizeof(ConnectRequest));
    xTaskCreate(bleWorkerEntry, "BleWorker", BLE_WORKER_STACK,
                nullptr, BLE_WORKER_PRIO, &bleWorkerTaskHandle);
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

    // Per design D6: walk slots in order, attempt connect on each one
    // that has a saved name in NVS. Sequential rather than parallel —
    // each handshake monopolises the BLE controller's scan + connect
    // path. With NimBLE max 3 conns this is at most ~3×7s = ~21s
    // total worst case to settle the slot table on boot. The host
    // can drive a faster reconnect by sending C_CONNECT explicitly
    // after boot if it doesn't want to wait.
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (slotHasSavedDevice[i])
        {
            log_i("Auto-connecting slot %u to saved device ...", (unsigned)i);
            connectAndReport(i); // no request sequence
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

            // Short-press: connect the lowest free slot via proximity
            // scan. Same first-free allocation as C_CONNECT, plus
            // force=true so we probe nearby instead of re-using the
            // slot's saved name.
            int8_t target_slot = firstFreeSlot();
            if (target_slot < 0)
            {
                log_w("button: all slots full, ignoring connect press");
            }
            else
            {
                log_i("button: connecting nearby device → slot %u",
                      (unsigned)target_slot);
                // No req_seq — button is a local trigger, no host ack.
                // connectAndReport still emits T_DEVINFO/T_FIELDS/etc.
                // and T_DEV_LIST so the host UI catches the new
                // session.
                if (!connectAndReport(static_cast<uint8_t>(target_slot),
                                      NO_HOST_REQUEST, /*force=*/true))
                {
                    log_i("button: connect failed (slot %u)",
                          (unsigned)target_slot);
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
    // Slot allocation:
    //   - If host specifies `dev` (kid pressed an empty slot card on
    //     the multi-device main view), target THAT slot so the UI's
    //     CONNECTING animation lines up with the slot the kid pressed.
    //   - Otherwise (legacy host, auto-detect path) fall back to
    //     first-free per design D2.
    // MsgPack picks any integer width for "dev" depending on the value
    // (0..127 → int8, 128..255 → uint8, etc.). Check presence via
    // !isNull() — a typed is<uint8_t>() would miss alternative widths.
    int8_t target_slot = -1;
    const bool hasDev = !root["dev"].isNull();
    if (hasDev)
    {
        // Echo `dev` on every NACK so the host can roll back the
        // per-slot CONNECTING animation against the right card instead
        // of guessing from `req` alone.
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
        // Per design D9: respond with ok=false + diagnostic msg.
        // Host UI surfaces this to the user.
        uart.sendAck(req, false, "all slots full");
        return;
    }
    // force=true: probe nearby for a NEW pairing rather than re-attempting
    // the slot's saved name. Host C_CONNECT semantics = "connect to
    // whatever's around".
    log_i("C_CONNECT: target slot=%u (%s)",
          (unsigned)target_slot,
          hasDev ? "host-specified" : "first-free");
    connectAndReport(static_cast<uint8_t>(target_slot), req, /*force=*/true);
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
    case UartAdapter::C_CONNECT:    cmdConnect(root, req);    break;
    case UartAdapter::C_DISCONNECT: cmdDisconnect(root, req); break;
    case UartAdapter::C_FORGET:     cmdForget(root, req);     break;
    case UartAdapter::C_DEV_LIST:   cmdDevList(root, req);    break;
    case UartAdapter::C_SET_PERIOD: cmdSetPeriod(root, req);  break;
    default:                        uart.sendAck(req, false, "unknown command"); break;
    }
}
