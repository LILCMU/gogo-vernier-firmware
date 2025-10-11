#pragma once

#include <Arduino.h>
#include <GDXLib.h>

class VernierAdapter
{
public:
    VernierAdapter();

    bool connect();
    void disconnect();

    void setSamplingRate(uint16_t period_ms);
    void startReading(uint16_t period_ms = 0);
    void stopReading();
    void poll();

    void getDeviceInfo(bool force = false);
    void clearDeviceInfo();

    // Sampling / status
    bool isConnected() const { return _connected; }
    bool isStreaming() const { return _streaming; }
    uint16_t samplingPeriod() const { return _period_ms; }

    // Channels
    uint32_t enabledChannelMask() const { return _enabled_mask; }
    uint8_t channelCount() const { return _channel_count; }

    // Device cached info
    const char *deviceName() { return _device_name.c_str(); }
    const char *orderCode() { return _device_order.c_str(); }
    const char *serialNumber() { return _device_serial.c_str(); }
    int batteryPercent() { return _device_battery; }
    int chargeState() { return _device_charge_state; }
    int rssi() { return _device_rssi; }

    // Per-channel quick access
    const char *sensorName(const byte selectedSensor = 255) { return _GDX.getSensorName(selectedSensor); }
    const char *sensorUnit(const byte selectedSensor = 255) { return _GDX.getUnits(selectedSensor); }
    float defaultMeasurement() { return _GDX.getMeasurement(_sensor_default); }
    float readMeasurement(const byte selectedSensor = 255) { return _GDX.getMeasurement(selectedSensor); }

    // Sample buffer interface
    bool sampleReady() const { return _sample_ready; }
    bool copySample(float *out, size_t &count)
    {
        if (!_sample_ready)
            return false;
        count = _channel_count;
        uint8_t idx = 0;
        for (uint8_t i = 0; i < 32; ++i)
        {
            if (_enabled_mask & (1u << i))
            {
                out[idx++] = _last_values[i];
            }
        }
        _sample_ready = false;
        return true;
    }

private:
    GDXLib _GDX;

    bool _enableAvailableChannels(bool force = false);

    bool _connected = false;
    bool _streaming = false;

    const char *_open_device = "proximity";
    unsigned long _period_start_time = 0;
    uint16_t _period_ms = 1000;
    uint16_t _read_timeout = 5000;

    // Cached device info
    String _device_name;
    String _device_order;
    String _device_serial;
    uint8_t _device_battery = 0;
    uint8_t _device_charge_state = 0;
    int _device_rssi = 0;

    const byte _sensor_default = 255;

    // Channel/sample state
    uint32_t _enabled_mask = 0;
    uint8_t _channel_count = 0;
    bool _sample_ready = false;
    float _last_values[32] = {0.0f};
};
