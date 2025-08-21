#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Length-prefixed MsgPack framing: [uint16_be length][MsgPack payload]
class UartAdapter
{
public:
    explicit UartAdapter(Print &out);

    void sendStatus(bool status, uint8_t core_state);
    void sendDeviceInfo(const String &device_name, const char *order, const char *serial);
    void sendDeviceStats(int battery, int charge_state, int rssi);
    void sendDeviceFields(uint8_t available_field, const String &name, const String &unit);
    void sendSensorValues(const float *values, size_t count);
    void sendDefaultSensorValue(float value);

private:
    enum MsgType : uint8_t
    {
        T_STATUS = 1,
        T_DEVINFO = 2,
        T_DEVSTATS = 3,
        T_FIELDS = 4,
        T_SENS_VALUES = 5,
        T_DEF_VALUE = 6
    };

    void send(); // serialize MsgPack with 2-byte BE length prefix

    Print &_out;
    JsonDocument _doc;
    uint32_t _seq = 0;
};
