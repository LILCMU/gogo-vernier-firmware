#include "vernier-adapter.h"

#include <string.h>

#include "uart-adapter.h"  // VERNIER_MAX_SLOTS

// VERNIER_MAX_SLOTS sizes the slots[] array and is advertised to the
// host via T_HELLO.max_slots. The NimBLE controller can only host as
// many concurrent peripheral connections as CONFIG_BT_NIMBLE_MAX_CONNECTIONS
// allows; bumping VERNIER_MAX_SLOTS past that ceiling would silently
// fail to connect the extra slots at the BLE layer instead of failing
// loudly at compile time. The static_assert lives in this TU (rather
// than the slot-count header) because including <nimconfig.h> from
// uart-adapter.h would couple the wire-protocol layer to the BLE
// stack; vernier-adapter.cpp already pulls NimBLE in via GoGoVernier.
static_assert(VERNIER_MAX_SLOTS <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
              "VERNIER_MAX_SLOTS exceeds the NimBLE controller's "
              "max concurrent connections — bump "
              "CONFIG_BT_NIMBLE_MAX_CONNECTIONS in nimconfig first.");

VernierAdapter::VernierAdapter()
{
    // VERNIER_SAMPLE_QUEUE_DEPTH (=2) lets the NimBLE notify task push
    // frame N+1 while the consumer task is still dispatching frame N.
    // At 1 Hz this is wildly overprovisioned; at 50 ms (Vernier's
    // typical floor) it gives one slot of slack before xQueueSend
    // returns errQUEUE_FULL. sizeof(gogo_vernier::Sample) ≈ 140 B, so
    // the queue costs ≈ 280 B of static RAM — fine on an ESP32-C3 with
    // ~300 KB free DRAM.
    _sample_queue = xQueueCreate(VERNIER_SAMPLE_QUEUE_DEPTH,
                                 sizeof(gogo_vernier::Sample));
}

VernierAdapter::~VernierAdapter()
{
    if (_sample_queue) vQueueDelete(_sample_queue);
}

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
        // Enable every channel the device exposes. GoGoVernier::open()
        // already does mut-ex-aware default-enable, but redo it here
        // so a future change in the lib's default policy doesn't
        // silently propagate. enableSensor() is idempotent for
        // already-enabled bits.
        uint32_t mask = _gv.availableChannelMask();
        for (uint8_t i = 0; i < gogo_vernier::MAX_CHANNELS; ++i)
        {
            if (mask & (1u << i)) _gv.enableSensor(i);
        }

        // Reset push-side drop counter and drain any stale samples
        // left in the queue from a previous session.
        _push_dropped.store(0, std::memory_order_relaxed);
        if (_sample_queue) xQueueReset(_sample_queue);

        // Wire the push path. Lambda runs on the NimBLE notify task —
        // must be non-blocking. xQueueSend with timeout=0 drops on
        // full and bumps the counter; the host MCU sees the count
        // via the next DEVSTATS push.
        QueueHandle_t q = _sample_queue;
        std::atomic<uint32_t>* drop_counter = &_push_dropped;
        _gv.onSample([q, drop_counter](const gogo_vernier::Sample& s) {
            if (!q) return;
            if (xQueueSend(q, &s, 0) != pdTRUE) {
                drop_counter->fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    return ok;
}

void VernierAdapter::disconnect()
{
    // Clear the push consumer FIRST so the NimBLE notify task stops
    // pushing into a queue we're about to drain. The lambda copy
    // we installed in connect() captures the queue handle by value,
    // so resetting it via {} in GoGoVernier safely drops the cb.
    _gv.onSample({});
    if (_gv.isStreaming()) _gv.stop();
    if (_gv.isConnected()) _gv.close();
    if (_sample_queue) xQueueReset(_sample_queue);
}

void VernierAdapter::setSamplingRate(uint16_t period_ms)
{
    // Defence-in-depth: cmdSetPeriod also NACKs sub-min requests at the
    // wire boundary (H4), but the adapter is the single sink for
    // _period_ms, so re-asserting the floor here keeps the invariant
    // holding for future writers (bleWorker, hardware-button paths,
    // direct test calls). Floor rather than reject — by the time we're
    // here the caller has already committed to setting a rate.
    if (period_ms < VERNIER_MIN_PERIOD_MS) period_ms = VERNIER_MIN_PERIOD_MS;
    _period_ms.store(period_ms, std::memory_order_relaxed);
    log_i("Set sampling period to %u ms", (unsigned)period_ms);
    if (_gv.isStreaming())
    {
        _gv.stop();
        _gv.start(period_ms);
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
    if (period_ms)
    {
        if (period_ms < VERNIER_MIN_PERIOD_MS) period_ms = VERNIER_MIN_PERIOD_MS;
        _period_ms.store(period_ms, std::memory_order_relaxed);
    }
    const uint16_t p = _period_ms.load(std::memory_order_relaxed);
    log_i("Start reading at %u ms period", (unsigned)p);
    _gv.start(p);
}

void VernierAdapter::stopReading()
{
    if (!_gv.isReady()) return;
    _gv.stop();
}

void VernierAdapter::getDeviceInfo(bool force)
{
    if (!_gv.isConnected() && !force) return;
    _gv.refreshStatus();
}

uint32_t VernierAdapter::droppedSamples() const  { return _gv.droppedSamples() + _push_dropped.load(std::memory_order_relaxed); }
uint8_t  VernierAdapter::channelCount() const    { return _gv.channelCount(); }

bool VernierAdapter::waitForSample(float *out, size_t &count, uint32_t timeout_ms)
{
    count = 0;
    if (!_sample_queue) return false;

    gogo_vernier::Sample s;
    if (xQueueReceive(_sample_queue, &s, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    if (out && s.count > 0) {
        memcpy(out, s.values, s.count * sizeof(float));
    }
    count = s.count;
    return true;
}
