#include "slot-helpers.h"

NvsScope::NvsScope(SemaphoreHandle_t mutex, Preferences &prefs,
                   const char *ns, bool read_only)
    : _mutex(mutex), _prefs(prefs), _opened(false)
{
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _opened = _prefs.begin(ns, read_only);
}

NvsScope::~NvsScope()
{
    if (_opened) _prefs.end();
    if (_mutex)  xSemaphoreGive(_mutex);
}

int8_t firstFreeSlot()
{
    // Prefer an IDLE slot that is not waiting for its lost sensor, so a
    // new pairing does not take the slot of a sensor that may come back;
    // fall back to a waiting one when every other slot is busy.
    int8_t waiting = -1;
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (isSlotOccupied(i)) continue;
        if (slots[i].reconnectState() == VernierAdapter::ReconnectState::OFF)
            return static_cast<int8_t>(i);
        if (waiting < 0) waiting = static_cast<int8_t>(i);
    }
    return waiting;
}

void sendDeviceFieldsFor(UartAdapter &uart, VernierAdapter &dev, uint8_t slot)
{
    uint32_t mask = dev.enabledChannelMask();
    const char *names[gogo_vernier::MAX_CHANNELS];
    const char *units[gogo_vernier::MAX_CHANNELS];
    uint8_t count = 0;
    for (uint8_t i = 0; i < gogo_vernier::MAX_CHANNELS; ++i)
    {
        if (mask & (1u << i))
        {
            names[count] = dev.sensorName(i);
            units[count] = dev.sensorUnit(i);
            ++count;
        }
    }
    if (count > 0)
        uart.sendDeviceFields(count, names, units, slot);
}
