#include "vernier-adapter.h"

bool VernierAdapter::connect(bool forceConnect)
{
    // If a session is already open (e.g. auto-connect on boot finished
    // before the host-MCU C_CONNECT arrived), treat the second request as
    // idempotent. Tearing the link down only to reopen it loses the
    // connection — and on NimBLE the BLEClient::disconnect path is async,
    // so a back-to-back close()+open() races against the controller and
    // returns BLE_HS_EALREADY (status=2) on the second connect.
    //
    // Use isReady() not isConnected(): isConnected() flips true the moment
    // the BLE link comes up, well before the D2PIO handshake fills the
    // available channel mask. If we returned success there, a follow-up
    // startReading() can race ahead of the in-flight handshake and send
    // CMD_START_MEASUREMENTS with mask=0. isReady() blocks here until the
    // handshake is genuinely complete (or fails) on whichever task got
    // the session_mutex first.
    if (_gv.isReady())
    {
        log_i("VernierAdapter::connect: already ready, returning success");
        getDeviceInfo(true);
        return true;
    }

    if (_gv.isStreaming()) _gv.stop();

    const char *target = forceConnect ? VERNIER_DEFAULT_DEVICE_NAME
                                      : _open_device.c_str();

    log_d("GoGoVernier open: %s%s", target,
          forceConnect ? " (force/proximity)" : " (saved)");

    bool ok = _gv.open(target);
    if (ok)
    {
        // Enable every channel the device exposes. Phase 1 stub returns 0
        // for availableChannelMask, so this is a no-op until the real
        // protocol layer lands; the loop is in place so the day it does
        // start returning a mask we behave correctly.
        uint32_t mask = _gv.availableChannelMask();
        for (uint8_t i = 0; i < 32; ++i)
        {
            if (mask & (1u << i)) _gv.enableSensor(i);
        }
    }
    return ok;
}

void VernierAdapter::disconnect()
{
    if (_gv.isStreaming()) _gv.stop();
    if (_gv.isConnected()) _gv.close();
    clearDeviceInfo();
}

void VernierAdapter::setSamplingRate(uint16_t period_ms)
{
    _period_ms = period_ms;
    log_i("Set sampling period to %u ms", _period_ms);
    if (_gv.isStreaming())
    {
        _gv.stop();
        _gv.start(_period_ms);
    }
}

void VernierAdapter::startReading(uint16_t period_ms)
{
    // Gate on isReady(), not isConnected(): a caller racing in between
    // BLE link-up and handshake completion would otherwise call
    // _gv.start() with available_mask==0 and waste a wire round-trip.
    // session_mutex inside GoGoVernier::start() also catches it, but
    // failing fast here keeps the contract consistent with connect().
    if (!_gv.isReady()) return;
    if (period_ms) _period_ms = period_ms;
    log_i("Start reading at %u ms period", _period_ms);
    _gv.start(_period_ms);
}

void VernierAdapter::stopReading()
{
    if (!_gv.isReady()) return;
    _gv.stop();
}

void VernierAdapter::poll()
{
    // Notification-driven now; intentionally empty. Kept so existing call
    // sites compile unchanged.
}

void VernierAdapter::getDeviceInfo(bool force)
{
    if (!_gv.isConnected() && !force) return;
    _gv.refreshStatus();
}

void VernierAdapter::clearDeviceInfo()
{
    // Cached device info lives inside GoGoVernier and is reset on open()/close().
}

uint32_t VernierAdapter::droppedSamples() const  { return _gv.droppedSamples(); }
uint8_t  VernierAdapter::channelCount() const    { return _gv.channelCount(); }
