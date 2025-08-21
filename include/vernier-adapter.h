#pragma once

#include <Arduino.h>
#include <GDXLib.h>

class VernierAdapter
{
public:
    VernierAdapter();

    bool connect();
    void disconnect();

    void startReading(uint16_t period_ms = 200);
    void setSamplingRate(uint16_t period_ms);
    void poll();

    bool isConnected() const { return _connected; }
    bool isStreaming() const { return _streaming; }
    uint16_t samplePeriod() const { return _period_ms; }

    String deviceName() { return _GDX.getDeviceName(); }
    const char *orderCode() { return _GDX.orderCode(); }
    const char *serialNumber() { return _GDX.serialNumber(); }
    int batteryPercent() { return _GDX.batteryPercent(); }
    int chargeState() { return _GDX.chargeState(); }
    int rssi() { return _GDX.RSSI(); }

    const String &sensorName() const { return _sensor_name; }
    const String &sensorUnit() const { return _sensor_unit; }
    float defaultMeasurement() { return _GDX.getMeasurement(_sensor_default); }

    uint8_t availableField() const { return 1; }
    void getDeviceInfo();

private:
    GDXLib _GDX;

    bool _connected = false;
    bool _streaming = false;

    const char *_open_device = "proximity"; // scan nearest device
    uint16_t _period_ms = 200;

    const byte _sensor_default = 255;
    String _sensor_name;
    String _sensor_unit;
};
