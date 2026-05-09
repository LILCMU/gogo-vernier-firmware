#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "GoGoVernier.h"

const constexpr char *VERNIER_DEFAULT_DEVICE_NAME = "proximity";

// Default sampling period (ms) until the host overrides via C_SET_PERIOD.
// 1 Hz is the slowest cadence the host UI updates at, so this is the
// "no surprises" landing value on a fresh boot.
constexpr uint16_t VERNIER_DEFAULT_PERIOD_MS = 1000;

// Push-mode sample queue depth — see VernierAdapter() ctor for sizing
// rationale.
constexpr UBaseType_t VERNIER_SAMPLE_QUEUE_DEPTH = 2;

// Thin facade over gogo_vernier::GoGoVernier that preserves the call-site
// shape main.cpp already uses. Owns one device session today; the multi-
// device upgrade lives in Phase 4 (see .claude/plans/gdxlib-rewrite.md).
class VernierAdapter
{
public:
    VernierAdapter();
    ~VernierAdapter();

    bool connect(bool forceConnect = false);
    void disconnect();
    void setOpenDevice(const char *device_name) { _open_device = device_name; }

    void setSamplingRate(uint16_t period_ms);
    void startReading(uint16_t period_ms = 0);
    void stopReading();

    void getDeviceInfo(bool force = false);

    bool isConnected() const { return _gv.isConnected(); }
    // True only after open() finished the full D2PIO handshake. Use this
    // — not isConnected() — anywhere a caller needs to know "the device
    // is ready to start streaming". isConnected() flips at BLE link-up,
    // before the channel mask is populated.
    bool isReady() const { return _gv.isReady(); }
    bool isStreaming() const { return _gv.isStreaming(); }
    uint16_t samplingPeriod() const { return _period_ms; }

    // Total dropped sample count: GoGoVernier's internal "previous
    // sample wasn't drained" + device-reported MEAS_DROPPED frames
    // + this adapter's queue-full drops on the push path. Reset to
    // 0 in connect().
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

    // Sample buffer interface — delegates to GoGoVernier. Polling
    // path is retained for the legacy `sampleReady()` /
    // `copySample()` shape; new callers should prefer
    // `waitForSample()` which blocks on the push-mode queue and
    // doesn't burn CPU on adaptive ticks.
    bool sampleReady() const { return _gv.sampleReady(); }
    bool copySample(float *out, size_t &count)
    {
        uint8_t n = 0;
        bool ok = const_cast<gogo_vernier::GoGoVernier &>(_gv).copySample(out, n);
        count = n;
        return ok;
    }

    // Block up to `timeout_ms` waiting for the next sample. On hit,
    // copies up to `count` floats into `out`, sets `count` to the
    // number copied, returns true. On timeout: count=0, returns
    // false. Backed by a depth-2 FreeRTOS queue fed from the
    // GoGoVernier::onSample callback running on the NimBLE notify
    // task. Queue overflow (consumer slower than producer) bumps
    // _push_dropped, exposed via droppedSamples().
    bool waitForSample(float *out, size_t &count, uint32_t timeout_ms);

private:
    gogo_vernier::GoGoVernier _gv;

    String _open_device = VERNIER_DEFAULT_DEVICE_NAME;
    uint16_t _period_ms = VERNIER_DEFAULT_PERIOD_MS;

    // Push-mode plumbing. Created in the ctor (process-lifetime),
    // drained by waitForSample(), filled by an onSample lambda
    // installed on connect() success and cleared on disconnect().
    QueueHandle_t _sample_queue = nullptr;
    uint32_t      _push_dropped = 0;  // queue-full drops
};
