#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Length-prefixed MsgPack framing: [uint16_be length][MsgPack payload]
//
// Protocol version — bump on any breaking change to message layout
// (field removal, type change, message-type reuse). Host compares this
// against its own compiled-in constant on every T_HELLO and warns on
// mismatch. Non-breaking additions (new optional keys, new message
// types) do NOT require a bump.
//
// v2 (Phase 4 step 2): per-device frames (T_DEVINFO, T_DEVSTATS,
// T_FIELDS, T_SENS_VALUES) gain optional `dev` (u8) field naming
// the slot the frame belongs to. Absent → treat as 0 (back-compat
// with v1 firmware on either side). T_HELLO gains `max_slots`
// reporting compile-time slot capacity. New T_DEV_LIST (msg 9)
// enumerates occupied slots. See .claude/plans/multi-device-design.md.
constexpr uint8_t VERNIER_PROTOCOL_VERSION = 2;

// Compile-time cap on concurrent BLE peers per vernier MCU. Bounded
// by CONFIG_NIMBLE_MAX_CONNECTIONS in the bundled NimBLE host (default
// 3 on arduino-esp32 3.3.x). Reported in T_HELLO so the host can
// size its slot-tracking table to match.
constexpr uint8_t VERNIER_MAX_SLOTS = 3;

// Co-MCU firmware identity values reported in T_HELLO / CMD_HELLO so the
// host can auto-detect which firmware is running. Mirror these constants
// across all three co-MCU firmwares (gogo-firmware host, this repo, and
// the GoGoBoard-Arduino library) so the host parses them uniformly.
constexpr uint8_t GOGOBOARD_FIRMWARE_ID_ARDUINO = 1;
constexpr uint8_t GOGOBOARD_FIRMWARE_ID_VERNIER = 2;
constexpr uint8_t GOGOBOARD_FIRMWARE_ID_TASMOTA = 3;

class UartAdapter
{
public:
    // Wire-protocol message types (vernier → host). Numeric values
    // are part of the protocol — keep stable; document each frame's
    // layout next to the constant.
    //
    // Value 6 retired (was T_DEF_VALUE — single-value sample variant
    // superseded by T_SENS_VALUES with count=1). Do not re-use without
    // bumping protocol.
    enum MsgType : uint8_t
    {
        T_STATUS      = 1,  // global. {t, status:bool, core_state:CoreState}. NO `dev`.
        T_DEVINFO     = 2,  // per-slot. {t, dev, device_name, order, serial}.
        T_DEVSTATS    = 3,  // per-slot. {t, dev, battery, charge_state, rssi, dropped}.
        T_FIELDS      = 4,  // per-slot. {t, dev, fields:[{name, unit}, ...]}.
        T_SENS_VALUES = 5,  // per-slot. {t, dev, ts, sensors:[float, ...], seq}.
        T_ACK         = 7,  // {t, req:u32, ok:bool, msg?:str, dev?:i16}. dev echoed when slot-scoped.
        T_HELLO       = 8,  // global. {t, proto_version, firmware_id, version_*, max_slots}.
        T_DEV_LIST    = 9,  // global. {t, slots:[{dev, name, order, connected}, ...]}.
    };

    // Wire-protocol command codes (host → vernier). Numeric values
    // are part of the protocol — keep stable; mirror the byte layout
    // in the comment so the host registry stays in sync.
    enum CmdType : uint8_t
    {
        C_CONNECT    = 1,  // {c, seq, dev?:u8}. dev opt = host-targeted slot.
        C_DISCONNECT = 2,  // {c, seq, dev:u8}.
        C_SET_PERIOD = 3,  // {c, seq, dev:u8, period_ms:u32 (capped at MAX_PERIOD_MS)}.
        C_DEV_LIST   = 4,  // {c, seq}. Triggers a T_DEV_LIST push.
        C_FORGET     = 5,  // {c, seq, dev:u8}. Disconnect + clear NVS deviceName{dev}.
    };

    // T_STATUS.core_state values — peer-health enum carried by the
    // global status frame. Currently only READY is sent on a healthy
    // boot; reserve values for future BOOTING / FAULT signals.
    enum CoreState : uint8_t
    {
        CORE_STATE_BOOTING = 0,
        CORE_STATE_READY   = 1,
    };

    explicit UartAdapter(Print &out);
    ~UartAdapter();

    void sendHello();
    void sendStatus(bool status, CoreState core_state);
    // Per-device sends: `dev` is the slot id the frame describes.
    // Defaults to 0 — single-device callers omit the arg, behaviour
    // unchanged. Multi-device callers (Phase 4 step 3+) pass the
    // slot id explicitly.
    void sendDeviceInfo(const char *device_name, const char *order, const char *serial, uint8_t dev = 0);
    void sendDeviceStats(int battery, int charge_state, int rssi, uint32_t dropped, uint8_t dev = 0);

    void sendDeviceFields(uint8_t field_count, const char *const names[], const char *const units[], uint8_t dev = 0);

    void sendSensorValues(const float *values, size_t count, uint8_t dev = 0);
    void sendSensorValuesTs(const float *values, size_t count, uint32_t ts_ms, uint8_t dev = 0);

    // T_DEV_LIST entry. One per slot — caller fills VERNIER_MAX_SLOTS
    // entries (occupied + empty) so host can render the full slot
    // table without guessing capacity. Empty slot: name="", order="",
    // connected=false. UartAdapter doesn't depend on VernierAdapter
    // — caller marshals from slots[] before calling.
    struct DevListEntry
    {
        uint8_t     dev;
        const char *name;
        const char *order;
        bool        connected;
    };
    void sendDevList(const DevListEntry *entries, uint8_t count);

    // sendAck: `dev` defaults to -1 meaning "no dev field" (legacy
    // global ack). For slot-scoped commands (C_CONNECT, C_DISCONNECT,
    // C_SET_PERIOD post-v2), pass the slot id so the host can route
    // the ack back to its per-slot pending-cmd tracker.
    void sendAck(uint32_t req_seq, bool ok, const char *msg = nullptr, int16_t dev = -1);

private:
    void send(); // serialize MsgPack with 2-byte BE length prefix

    Print &_out;
    JsonDocument _doc;
    uint32_t _seq = 0;
    // _doc is shared mutable state; every public sendXxx() builds the
    // doc then calls send() which measures + serializes. With three
    // FreeRTOS tasks (uartHandler, vernierHandler, main loop)
    // simultaneously calling sendXxx on this instance, the doc would
    // be clobbered mid-build by another task — observed in field-test
    // logs as len/body mismatches and host-side parser desync. Mutex
    // serialises the entire build+send critical section per call.
    SemaphoreHandle_t _send_mutex = nullptr;
};
