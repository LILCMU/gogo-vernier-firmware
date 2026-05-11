#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "uart-adapter.h"
#include "vernier-adapter.h"

// RAII guard for the cross-task NVS critical section. Takes the mutex
// on construction, opens the namespace, releases the mutex and closes
// the namespace on destruction. Conversion to bool is `true` iff
// preferences.begin() succeeded — caller must check before reading or
// writing keys.
//
// Usage:
//   {
//       NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false);
//       if (nvs) {
//           preferences.putString(key, value);
//       }
//   }
class NvsScope
{
public:
    NvsScope(SemaphoreHandle_t mutex, Preferences &prefs,
             const char *ns, bool read_only);
    ~NvsScope();
    explicit operator bool() const { return _opened; }

    NvsScope(const NvsScope &)            = delete;
    NvsScope &operator=(const NvsScope &) = delete;

private:
    SemaphoreHandle_t _mutex;
    Preferences      &_prefs;
    bool              _opened;
};

// slots[] lives in main.cpp — the helpers below act on it directly so
// call sites don't have to plumb the array through every layer.
extern VernierAdapter slots[VERNIER_MAX_SLOTS];

// "Occupied" = handshake complete (isReady) OR mid-handshake
// transition (isStreaming before isReady flips). Matches the gate
// used everywhere in the firmware to decide whether a slot can
// accept a new C_CONNECT.
inline bool isSlotOccupied(uint8_t slot)
{
    return slots[slot].isReady() || slots[slot].isStreaming();
}

inline bool slotInRange(int slot)
{
    return slot >= 0 && slot < VERNIER_MAX_SLOTS;
}

// Lowest-index free slot, or -1 if every slot is busy. Used by
// C_CONNECT's first-free fallback and the BOOT button short-press.
int8_t firstFreeSlot();

// Build the names[]/units[] arrays from `dev`'s enabled-channel mask
// and push T_FIELDS. Skips the send when the slot exposes zero
// channels (host doesn't need an empty fields frame). Replaces the
// inline marshal loop that was duplicated in connectAndReport and
// the re-emit branch of vernierHandler.
void sendDeviceFieldsFor(UartAdapter &uart, VernierAdapter &dev, uint8_t slot);
