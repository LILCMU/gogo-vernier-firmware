#pragma once

#include <Arduino.h>

#include "GoGoVernier.h"

const constexpr char *VERNIER_DEFAULT_DEVICE_NAME = "proximity";

// Thin facade over gogo_vernier::GoGoVernier that preserves the call-site
// shape main.cpp already uses. Owns one device session today; the multi-
// device upgrade lives in Phase 4 (see .claude/plans/gdxlib-rewrite.md).
class VernierAdapter
{
public:
    VernierAdapter() = default;

    bool connect(bool forceConnect = false);
    void disconnect();
    void setOpenDevice(const char *device_name) { _open_device = device_name; }

    void setSamplingRate(uint16_t period_ms);
    void startReading(uint16_t period_ms = 0);
    void stopReading();
    void poll(); // no-op today; reserved for future host-side housekeeping

    void getDeviceInfo(bool force = false);
    void clearDeviceInfo();

    bool isConnected() const { return _gv.isConnected(); }
    bool isStreaming() const { return _gv.isStreaming(); }
    uint16_t samplingPeriod() const { return _period_ms; }

    // Bumped by GoGoVernier internally each time a notification overwrites
    // an unconsumed sample (latest-wins). Reset to 0 in connect().
    uint32_t droppedSamples() const;

    uint32_t enabledChannelMask() const { return _gv.enabledChannelMask(); }
    uint8_t channelCount() const;

    const char *deviceName()   { return _gv.deviceInfo().name; }
    const char *orderCode()    { return _gv.deviceInfo().order_code; }
    const char *serialNumber() { return _gv.deviceInfo().serial; }
    int batteryPercent()       { return _gv.status().battery_percent; }
    int chargeState()          { return static_cast<int>(_gv.status().charger_state); }
    int rssi()                 { return _gv.status().rssi; }

    const char *sensorName(byte selectedSensor = 255)
    {
        const auto *c = _gv.channel(selectedSensor < 32 ? selectedSensor : 0);
        return c ? c->description : "";
    }
    const char *sensorUnit(byte selectedSensor = 255)
    {
        const auto *c = _gv.channel(selectedSensor < 32 ? selectedSensor : 0);
        return c ? c->units : "";
    }
    float defaultMeasurement() { return _gv.measurement(0); }
    float readMeasurement(byte selectedSensor = 255)
    {
        return _gv.measurement(selectedSensor < 32 ? selectedSensor : 0);
    }

    // Sample buffer interface — delegates to GoGoVernier.
    bool sampleReady() const { return _gv.sampleReady(); }
    bool copySample(float *out, size_t &count)
    {
        uint8_t n = 0;
        bool ok = const_cast<gogo_vernier::GoGoVernier &>(_gv).copySample(out, n);
        count = n;
        return ok;
    }

private:
    gogo_vernier::GoGoVernier _gv;

    String _open_device = VERNIER_DEFAULT_DEVICE_NAME;
    uint16_t _period_ms = 1000;
};
