#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Length-prefixed MsgPack framing: [uint16_be length][MsgPack payload]
class UartAdapter
{
public:
    explicit UartAdapter(Print &out);

    void sendStatus(bool status, uint8_t core_state);
    void sendDeviceInfo(const char *device_name, const char *order, const char *serial);
    void sendDeviceStats(int battery, int charge_state, int rssi);

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
    };

    void send(); // serialize MsgPack with 2-byte BE length prefix

    Print &_out;
    JsonDocument _doc;
    uint32_t _seq = 0;
};
