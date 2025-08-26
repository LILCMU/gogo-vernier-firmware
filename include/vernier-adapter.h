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

    bool enableAvailableChannels(bool force = false);
    void getDeviceInfo(bool force = false);
    void clearDeviceInfo();

    bool isConnected() const { return _connected; }
    bool isStreaming() const { return _streaming; }
    uint16_t samplingPeriod() const { return _period_ms; }
    uint32_t availableChannels() { return _GDX.getAvailableChannels(); }

    const char *deviceName() { return _device_name.c_str(); }
    const char *orderCode() { return _device_order.c_str(); }
    const char *serialNumber() { return _device_serial.c_str(); }
    int batteryPercent() { return _device_battery; }
    int chargeState() { return _device_charge_state; }
    int rssi() { return _device_rssi; }

    const char *sensorName(const byte selectedSensor = 255) { return _GDX.getSensorName(selectedSensor); }
    const char *sensorUnit(const byte selectedSensor = 255) { return _GDX.getUnits(selectedSensor); }
    float defaultMeasurement() { return _GDX.getMeasurement(_sensor_default); }
    float readMeasurement(const byte selectedSensor = 255) { return _GDX.getMeasurement(selectedSensor); }

private:
    GDXLib _GDX;

    bool _connected = false;
    bool _streaming = false;

    const char *_open_device = "proximity";
    unsigned long _period_start_time = 0;
    uint16_t _period_ms = 200;
    uint16_t _read_timeout = 5000;

    String _device_name;
    String _device_order;
    String _device_serial;
    uint8_t _device_battery;
    uint8_t _device_charge_state;
    int _device_rssi;

    const byte _sensor_default = 255;
};
