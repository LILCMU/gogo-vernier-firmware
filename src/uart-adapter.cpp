#include "uart-adapter.h"

UartAdapter::UartAdapter(Print &out)
    : _out(out) {}

void UartAdapter::send()
{
    const size_t len = measureMsgPack(_doc);
    if (len > 0xFFFF)
        return; // guard: too large for 16-bit framing

    uint8_t hdr[2] = {static_cast<uint8_t>((len >> 8) & 0xFF),
                      static_cast<uint8_t>(len & 0xFF)};
    _out.write(hdr, 2);
    serializeMsgPack(_doc, _out);
    _doc.clear();
}

void UartAdapter::sendStatus(bool status, uint8_t core_state)
{
    _doc["t"] = T_STATUS;
    _doc["seq"] = _seq++;
    _doc["status"] = status;
    _doc["core_state"] = core_state;
    send();
}

void UartAdapter::sendDeviceInfo(const String &device_name, const char *order, const char *serial)
{
    _doc["t"] = T_DEVINFO;
    _doc["seq"] = _seq++;
    _doc["device_name"] = device_name;
    _doc["order"] = order;
    _doc["serial"] = serial;
    send();
}

void UartAdapter::sendDeviceStats(int battery, int charge_state, int rssi)
{
    _doc["t"] = T_DEVSTATS;
    _doc["seq"] = _seq++;
    _doc["battery"] = battery;
    _doc["charge_state"] = charge_state;
    _doc["rssi"] = rssi;
    send();
}

void UartAdapter::sendDeviceFields(uint8_t available_field, const String &name, const String &unit)
{
    _doc["t"] = T_FIELDS;
    _doc["seq"] = _seq++;
    _doc["field_count"] = available_field;

    JsonArray fields = _doc["fields"].to<JsonArray>();
    JsonObject f0 = fields.add<JsonObject>();
    f0["name"] = name;
    f0["unit"] = unit;

    send();
}

void UartAdapter::sendSensorValues(const float *values, size_t count)
{
    _doc["t"] = T_SENS_VALUES;
    _doc["seq"] = _seq++;

    JsonArray arr = _doc["sensors"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i)
        arr.add(values[i]);

    send();
}

void UartAdapter::sendDefaultSensorValue(float value)
{
    _doc["t"] = T_DEF_VALUE;
    _doc["seq"] = _seq++;
    _doc["sensor"] = value;
    send();
}
