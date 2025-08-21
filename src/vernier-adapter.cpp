#include "vernier-adapter.h"

VernierAdapter::VernierAdapter()
    : _GDX() {}

bool VernierAdapter::connect()
{
    if (_connected)
    {
        _GDX.close();
        _connected = false;
        _streaming = false;
    }

    _connected = _GDX.open((char *)_open_device);
    if (_connected)
    {
        _GDX.enableSensor(_sensor_default);
        _sensor_name = _GDX.getSensorName(_sensor_default);
        _sensor_unit = _GDX.getUnits(_sensor_default);
    }
    return _connected;
}

void VernierAdapter::disconnect()
{
    _GDX.close();
    _connected = false;
    _streaming = false;
}

void VernierAdapter::startReading(uint16_t period_ms)
{
    if (!_connected)
        return;
    _period_ms = period_ms;
    _GDX.start(_period_ms);
    _streaming = true;
}

void VernierAdapter::setSamplingRate(uint16_t period_ms)
{
    if (!_connected)
        return;
    _period_ms = period_ms;
    _GDX.start(_period_ms);
    _streaming = true;
}

void VernierAdapter::poll()
{
    if (!_connected || !_streaming)
        return;
    _GDX.read();
}

void VernierAdapter::getDeviceInfo()
{
    if (!_connected)
        return;
    _sensor_name = _GDX.getSensorName(_sensor_default);
    _sensor_unit = _GDX.getUnits(_sensor_default);
}
