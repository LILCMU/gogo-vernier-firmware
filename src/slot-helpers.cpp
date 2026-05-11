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
    for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
    {
        if (!isSlotOccupied(i)) return static_cast<int8_t>(i);
    }
    return -1;
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
