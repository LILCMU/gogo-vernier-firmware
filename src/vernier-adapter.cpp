#include "vernier-adapter.h"

static byte gblReadBuffer[128];

VernierAdapter::VernierAdapter() : _GDX()
{
    _enabled_mask = 0;
    _channel_count = 0;
    _sample_ready = false;
}

bool VernierAdapter::connect(bool forceConnect)
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

    bool connected = false;
    if (forceConnect)
    {
        // abort any ongoing scan in the library so we can force a new connection
        _GDX.abortScan();

        unsigned long waitStart = millis();
        const unsigned long waitTimeout = 3000;
        while (_GDX.isScanning() && (millis() - waitStart) < waitTimeout)
        {
            delay(10);
        }

        log_d("Force connect enabled: scanning for device ...");
        connected = _GDX.open((char *)VERNIER_DEFAULT_DEVICE_NAME);
    }
    else
    {
        log_d("Use saved device: %s", _open_device.c_str());
        connected = _GDX.open((char *)_open_device.c_str());
    }

    if (connected)
    {
        _dropped_samples = 0; // fresh session starts clean
        this->getDeviceInfo(connected);
        this->_enableAvailableChannels(connected);
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

    log_i("Set sampling period to %u ms", _period_ms);

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

    log_i("Start reading at %u ms period", _period_ms);

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

    if ((millis() - _period_start_time) < _period_ms)
        return;

    // Attempt read (library-specific low-level call or generic)
    if (_GDX.GDX_ReadMeasurement(gblReadBuffer, _read_timeout) == 0)
    {
        if (_GDX.GDX_ReadMeasurement(gblReadBuffer, _read_timeout) == 0)
            return;
    }

    // Latest-wins: if the previous sample wasn't consumed yet, count it
    // as dropped before the overwrite so drops surface in T_DEVSTATS.
    if (_sample_ready)
        _dropped_samples++;

    // Collect all enabled channel values
    for (uint8_t i = 0; i < 32; ++i)
    {
        if (_enabled_mask & (1u << i))
        {
            _last_values[i] = _GDX.getMeasurement(i);
        }
    }
    _sample_ready = true;
    _period_start_time = millis();
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

bool VernierAdapter::_enableAvailableChannels(bool force)
{
    if (!_connected && !force)
        return false;

    uint32_t availableMask = _GDX.getAvailableChannels();

    if (!availableMask)
        return false;

    _enabled_mask = 0;
    _channel_count = 0;

    for (uint8_t i = 0; i < 32; i++)
    {
        if (availableMask & (1 << i))
        {
            _GDX.enableSensor(i);
            _enabled_mask |= (1 << i);
            _channel_count++;

            log_i("enable channel: %u (%s %s)", i, _GDX.getSensorName(i), _GDX.getUnits(i));
        }
    }

    return _channel_count > 0;
}
