#include "uart-adapter.h"

namespace {
// Length-prefix is uint16_t big-endian. Cap on payload size + size of
// the prefix itself so callers can read either constant by name.
constexpr size_t  MAX_FRAME_PAYLOAD = 0xFFFF;
constexpr uint8_t FRAME_PREFIX_SIZE = 2;
}  // namespace

UartAdapter::UartAdapter(Print &out)
    : _out(out)
{
    _send_mutex = xSemaphoreCreateMutex();
}

UartAdapter::~UartAdapter()
{
    if (_send_mutex) vSemaphoreDelete(_send_mutex);
}

namespace {
struct SendLock {
    SemaphoreHandle_t m;
    SendLock(SemaphoreHandle_t mu) : m(mu) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
    ~SendLock()                            { if (m) xSemaphoreGive(m); }
};
}  // namespace

void UartAdapter::send()
{
    const size_t len = measureMsgPack(_doc);
    if (len > MAX_FRAME_PAYLOAD)
    {
        log_e("UART send skip — payload too large len=%u", (unsigned)len);
        return; // guard: too large for 16-bit framing
    }

    uint8_t hdr[FRAME_PREFIX_SIZE] = {static_cast<uint8_t>((len >> 8) & 0xFF),
                                      static_cast<uint8_t>(len & 0xFF)};
    _out.write(hdr, FRAME_PREFIX_SIZE);
    serializeMsgPack(_doc, _out);
    _doc.clear();
}

void UartAdapter::sendStatus(bool status, uint8_t core_state)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_STATUS;
    _doc["seq"] = _seq++;
    _doc["status"] = status;
    _doc["core_state"] = core_state;
    send();
}

void UartAdapter::sendDeviceInfo(const char *device_name, const char *order, const char *serial, uint8_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_DEVINFO;
    _doc["seq"] = _seq++;
    _doc["dev"] = dev;
    _doc["device_name"] = device_name;
    _doc["order"] = order;
    _doc["serial"] = serial;
    send();
}

void UartAdapter::sendHello()
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_HELLO;
    _doc["seq"] = _seq++;
    _doc["proto_version"] = VERNIER_PROTOCOL_VERSION;
    _doc["firmware_id"]   = GOGOBOARD_FIRMWARE_ID_VERNIER;
    _doc["version_major"] = FIRMWARE_MAJOR_VERSION;
    _doc["version_minor"] = FIRMWARE_MINOR_VERSION;
    _doc["version_patch"] = FIRMWARE_PATCH_VERSION;
    _doc["max_slots"]     = VERNIER_MAX_SLOTS;
    send();
}

void UartAdapter::sendDeviceStats(int battery, int charge_state, int rssi, uint32_t dropped, uint8_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_DEVSTATS;
    _doc["seq"] = _seq++;
    _doc["dev"] = dev;
    _doc["battery"] = battery;
    _doc["charge_state"] = charge_state;
    _doc["rssi"] = rssi;
    _doc["dropped"] = dropped;
    send();
}

void UartAdapter::sendDeviceFields(uint8_t field_count, const char *const names[], const char *const units[], uint8_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_FIELDS;
    _doc["seq"] = _seq++;
    _doc["dev"] = dev;
    _doc["field_count"] = field_count;

    JsonArray arr = _doc["fields"].to<JsonArray>();
    for (uint8_t i = 0; i < field_count; ++i)
    {
        JsonObject f = arr.add<JsonObject>();
        f["name"] = names[i] ? names[i] : "";
        f["unit"] = units[i] ? units[i] : "";
    }
    send();
}

void UartAdapter::sendSensorValues(const float *values, size_t count, uint8_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_SENS_VALUES;
    _doc["seq"] = _seq++;
    _doc["dev"] = dev;

    JsonArray arr = _doc["sensors"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i)
        arr.add(values[i]);

    send();
}

void UartAdapter::sendSensorValuesTs(const float *values, size_t count, uint32_t ts_ms, uint8_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_SENS_VALUES;
    _doc["seq"] = _seq++;
    _doc["dev"] = dev;
    _doc["ts"] = ts_ms;

    JsonArray arr = _doc["sensors"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i)
        arr.add(values[i]);

    send();
}

void UartAdapter::sendDevList(const DevListEntry *entries, uint8_t count)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_DEV_LIST;
    _doc["seq"] = _seq++;

    JsonArray arr = _doc["slots"].to<JsonArray>();
    for (uint8_t i = 0; i < count; ++i)
    {
        JsonObject s = arr.add<JsonObject>();
        s["dev"]       = entries[i].dev;
        s["name"]      = entries[i].name  ? entries[i].name  : "";
        s["order"]     = entries[i].order ? entries[i].order : "";
        s["connected"] = entries[i].connected;
    }
    send();
}

void UartAdapter::sendAck(uint32_t req_seq, bool ok, const char *msg, int16_t dev)
{
    SendLock _lk(_send_mutex);
    _doc["t"] = T_ACK;
    _doc["seq"] = _seq++;
    _doc["req"] = req_seq;
    _doc["ok"] = ok;
    if (msg && *msg)
        _doc["msg"] = msg;
    if (dev >= 0)
        _doc["dev"] = static_cast<uint8_t>(dev);
    send();
}
