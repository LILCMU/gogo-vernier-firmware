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
    explicit UartAdapter(Print &out);
    ~UartAdapter();

    void sendHello();
    void sendStatus(bool status, uint8_t core_state);
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
    // Value 6 retired (was T_DEF_VALUE — single-value sample variant superseded
    // by T_SENS_VALUES with count=1). Do not re-use without bumping protocol.
    enum MsgType : uint8_t
    {
        T_STATUS      = 1,
        T_DEVINFO     = 2,
        T_DEVSTATS    = 3,
        T_FIELDS      = 4,
        T_SENS_VALUES = 5,
        T_ACK         = 7,
        T_HELLO       = 8,
        T_DEV_LIST    = 9,  // v2: slot enumeration; emit method lands in Phase 4 step 3
    };

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
