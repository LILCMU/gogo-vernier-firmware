#include "vernier-adapter.h"

static byte gblReadBuffer[128];

VernierAdapter::VernierAdapter()
    : _GDX() {}

bool VernierAdapter::connect()
{
    if (_streaming)
    {
        _GDX.stop();
        _streaming = false;
    }
    if (_connected)
    {
        _GDX.close();
        _connected = false;
    }

    bool connected = _GDX.open((char *)_open_device);

    if (connected)
    {
        this->getDeviceInfo(connected);
        this->enableAvailableChannels(connected);
    }
    _connected = connected;

    return connected;
}

void VernierAdapter::disconnect()
{
    if (_streaming)
        _GDX.stop();

    if (_connected)
        _GDX.close();

    _connected = false;
    _streaming = false;

    this->clearDeviceInfo();
}

void VernierAdapter::setSamplingRate(uint16_t period_ms)
{
    _period_ms = period_ms;

    if (_streaming)
    {
        _GDX.stop();
        _GDX.start(_period_ms);
    }
}

void VernierAdapter::startReading(uint16_t period_ms)
{
    if (!_connected)
        return;

    if (period_ms)
        _period_ms = period_ms;

    _GDX.start(_period_ms);
    _streaming = true;
}

void VernierAdapter::stopReading(void)
{
    if (!_connected)
        return;

    _GDX.stop();
    _streaming = false;
}

void VernierAdapter::poll()
{
    if (!_connected || !_streaming)
        return;

    if ((millis() - _period_start_time) > _period_ms)
    {
        if (_GDX.GDX_ReadMeasurement(gblReadBuffer, _read_timeout) == 0)
            _GDX.GDX_ReadMeasurement(gblReadBuffer, _read_timeout);

        _period_start_time = millis();
    }
}

bool VernierAdapter::enableAvailableChannels(bool force)
{
    if (!_connected && !force)
        return false;

    uint32_t availableMask = _GDX.getAvailableChannels();

    if (!availableMask)
        return false;

    for (uint8_t i = 0; i < 32; i++)
    {
        if (availableMask & (1 << i))
        {
            _GDX.enableSensor(i);
            log_i("enable channel: %d", i);
            log_i("%s -> %s\n", _GDX.getSensorName(i), _GDX.getUnits(i));
        }
    }

    return true;
}

void VernierAdapter::getDeviceInfo(bool force)
{
    if (!_connected && !force)
        return;

    _device_name = _GDX.getDeviceName();
    _device_order = _GDX.orderCode();
    _device_serial = _GDX.serialNumber();
    _device_battery = _GDX.batteryPercent();
    _device_charge_state = _GDX.chargeState();
    _device_rssi = _GDX.RSSI();

    log_i("Device Name: %s", _device_name.c_str());
    log_i("Order Code: %s", _device_order.c_str());
    log_i("Serial Number: %s", _device_serial.c_str());
    log_i("Battery Percent: %d", _device_battery);
    log_i("Charge State: %d", _device_charge_state);
    log_i("RSSI: %d", _device_rssi);
}

void VernierAdapter::clearDeviceInfo()
{
    _device_name = "";
    _device_order = "";
    _device_serial = "";
    _device_battery = 0;
    _device_charge_state = 0;
    _device_rssi = 0;
}
