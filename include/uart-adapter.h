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
constexpr uint8_t VERNIER_PROTOCOL_VERSION = 1;

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
    void sendDeviceInfo(const char *device_name, const char *order, const char *serial);
    void sendDeviceStats(int battery, int charge_state, int rssi, uint32_t dropped);

    void sendDeviceFields(uint8_t field_count, const char *const names[], const char *const units[]);

    void sendSensorValues(const float *values, size_t count);
    void sendSensorValuesTs(const float *values, size_t count, uint32_t ts_ms);

    void sendAck(uint32_t req_seq, bool ok, const char *msg = nullptr);

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
