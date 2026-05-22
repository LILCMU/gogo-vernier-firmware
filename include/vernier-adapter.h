#pragma once

#include <atomic>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "GoGoVernier.h"

const constexpr char *VERNIER_DEFAULT_DEVICE_NAME = "proximity";

// Default sampling period (ms) until the host overrides via C_SET_PERIOD.
// 1 Hz is the slowest cadence the host UI updates at, so this is the
// "no surprises" landing value on a fresh boot.
constexpr uint16_t VERNIER_DEFAULT_PERIOD_MS = 1000;

// Lower bound on the sampling period the host may request via
// C_SET_PERIOD. 10 ms is well above the FreeRTOS tick floor and
// matches the fastest period any documented GDX sensor accepts;
// anything below this is treated as a host bug rather than passed
// through to GoGoVernier (which would happily spin start/stop on
// sub-ms values).
constexpr uint16_t VERNIER_MIN_PERIOD_MS = 10;

// Push-mode sample queue depth — see VernierAdapter() ctor for sizing
// rationale.
constexpr UBaseType_t VERNIER_SAMPLE_QUEUE_DEPTH = 2;

// Thin facade over gogo_vernier::GoGoVernier that preserves the call-site
// shape the firmware uses. One adapter per slot; slots[] in main.cpp
// holds VERNIER_MAX_SLOTS of them.
class VernierAdapter
{
public:
    // Per-slot connection lifecycle. Drives the bleWorker state
    // machine wired in across G004 (this enum + atomic _conn_state)
    // → G006 (bleWorker scaffolding + CAS helpers) → G007 (cutover
    // + ordering fix in da5e19c). Producer set:
    //   - cmdConnect / button / autoConnect → enqueueConnect(),
    //     which setState(REQUESTED).
    //   - bleWorker dequeue → tryAcquireConnecting() (CAS
    //     REQUESTED → CONNECTING). Cancel-race-safe.
    //   - publishConnectResult tail → setState(READY) on success,
    //     setState(IDLE) on failure. Ordering: AFTER every
    //     uart.send*, with a compiler barrier between the last
    //     send and the state publish so vernierHandler can't see
    //     READY before T_FIELDS shipped.
    //   - cmdCancelConnect → tryCancelConnect() (CAS REQUESTED →
    //     IDLE). Loses cleanly if bleWorker already CAS'd into
    //     CONNECTING.
    //   - disconnect() → setState(IDLE).
    // Numeric values are part of the wire contract (T_DEV_LIST's
    // optional `state` field, G008). Don't renumber — append at
    // the end.
    enum class ConnState : uint8_t
    {
        IDLE       = 0,
        REQUESTED  = 1,
        CONNECTING = 2,
        READY      = 3,
        // FAILED is reserved on the wire but currently unreachable
        // (no producer writes it post-da5e19c — failure goes
        // straight to IDLE). Kept so a future host-UX iteration
        // can re-expose a brief FAILED window without renumbering.
        FAILED     = 4,
    };

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
    // True only when this adapter has been put in the READY state by
    // bleWorker (i.e. dev.connect() returned true and publishConnectResult
    // ran successfully). Pre-G007 this routed through `_gv.isReady()`
    // (the GDX driver's own ready flag); the G007 cutover puts the
    // adapter in charge of the lifecycle, so vernierHandler and
    // emitDevList both consult `_conn_state` directly. The tiny lag
    // between `_gv.isReady()` flipping true and the worker writing
    // READY is intentional — it ensures all the host-protocol side-
    // effects (T_HELLO, T_DEVINFO, T_FIELDS, etc.) have shipped before
    // any reader treats the slot as live.
    bool isReady() const { return state() == ConnState::READY; }
    bool isStreaming() const { return _gv.isStreaming(); }

    // Connection-lifecycle accessors. memory_order_relaxed because
    // transitions are independent scalar publishes — readers don't
    // depend on any other write happening-before this one. Producers:
    // bleWorker (CONNECTING via tryAcquireConnecting, READY/IDLE via
    // setState from publishConnectResult), cmdCancelConnect
    // (REQUESTED→IDLE via tryCancelConnect), disconnect (IDLE).
    ConnState state() const { return _conn_state.load(std::memory_order_relaxed); }
    void setState(ConnState s) { _conn_state.store(s, std::memory_order_relaxed); }

    // CAS REQUESTED → CONNECTING for the bleWorker dequeue path. Closes
    // the race window between xQueueReceive and a concurrent
    // cmdCancelConnect: if cancel landed first the state is IDLE, the
    // CAS fails, and the worker skips the connect. Atomic on RV32 by
    // construction.
    bool tryAcquireConnecting()
    {
        ConnState expected = ConnState::REQUESTED;
        return _conn_state.compare_exchange_strong(
            expected, ConnState::CONNECTING, std::memory_order_relaxed);
    }

    // CAS REQUESTED → IDLE for cmdCancelConnect. Loses cleanly if
    // bleWorker already moved the slot to CONNECTING (worker won the
    // race; cancel is "too late"). Without the CAS, cmdCancelConnect's
    // read-state-then-store-IDLE could clobber a worker-set CONNECTING
    // and leave the worker dispatching dev.connect() on a slot the
    // host believes is idle.
    bool tryCancelConnect()
    {
        ConnState expected = ConnState::REQUESTED;
        return _conn_state.compare_exchange_strong(
            expected, ConnState::IDLE, std::memory_order_relaxed);
    }

    // H6: per-slot mutex protecting the cached status struct (battery,
    // charge, rssi, RSSI etc. served by the *Percent / *State / rssi()
    // accessors) against torn multi-field reads. getDeviceInfo() —
    // which calls _gv.refreshStatus() and writes those fields —
    // takes the mutex internally. Callers that need an atomic snapshot
    // across the three status fields (publishConnectResult's
    // sendDeviceStats, vernierHandler's 10 s refresh push) wrap their
    // read block in lockStatus()/unlockStatus(). timeout_ms == 0 means
    // try-lock (non-blocking). Returns false on contention.
    bool lockStatus(uint32_t timeout_ms = portMAX_DELAY);
    void unlockStatus();
    uint16_t samplingPeriod() const { return _period_ms.load(std::memory_order_relaxed); }

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
        const auto *c = _gv.channel(selectedSensor < gogo_vernier::MAX_CHANNELS ? selectedSensor : 0);
        return c ? c->description : "";
    }
    const char *sensorUnit(byte selectedSensor = 255)
    {
        const auto *c = _gv.channel(selectedSensor < gogo_vernier::MAX_CHANNELS ? selectedSensor : 0);
        return c ? c->units : "";
    }
    float defaultMeasurement() { return _gv.measurement(0); }
    float readMeasurement(byte selectedSensor = 255)
    {
        return _gv.measurement(selectedSensor < gogo_vernier::MAX_CHANNELS ? selectedSensor : 0);
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
    // Cross-task: written by cmdSetPeriod / startReading (UART task),
    // read by vernierHandler (vernier task) via samplingPeriod() and
    // again by startReading() itself. std::atomic<uint16_t> documents
    // the contract; on RV32 the loads/stores are single-instruction
    // and free, so memory_order_relaxed is the right ordering.
    std::atomic<uint16_t> _period_ms{VERNIER_DEFAULT_PERIOD_MS};

    // Push-mode plumbing. Created in the ctor (process-lifetime),
    // drained by waitForSample(), filled by an onSample lambda
    // installed on connect() success and cleared on disconnect().
    QueueHandle_t _sample_queue = nullptr;
    // queue-full drops. Written by the NimBLE notify task (push lambda
    // installed in connect()), read by vernierHandler via droppedSamples().
    // std::atomic<uint32_t> replaces the prior volatile+manual-load-store
    // pattern that side-stepped C++20's -Wdeprecated-volatile on the
    // compound ++. Same single-core RV32 free-load characteristics.
    std::atomic<uint32_t> _push_dropped{0};

    // Cross-task connection lifecycle. Writer set will be cmdConnect /
    // bleWorker / disconnect() once the bleWorker cutover (G006/G007)
    // lands. Reader set will be the host-protocol layer that emits
    // T_DEV_LIST entries (G008) and any cancel / busy guards in
    // cmdConnect. memory_order_relaxed because transitions don't carry
    // ordering w.r.t. other adapter state.
    std::atomic<ConnState> _conn_state{ConnState::IDLE};

    // H6 per-slot status mutex. Created in ctor (process-lifetime),
    // taken inside getDeviceInfo() and by external callers via
    // lockStatus() / unlockStatus(). Guards the (refreshStatus + read)
    // window against vernierHandler's 10 s wall-clock refresh racing
    // bleWorker's end-of-publishConnectResult getDeviceInfo on the
    // same slot. Per-slot rather than global so concurrent connects on
    // different slots don't serialise on each other's status reads.
    SemaphoreHandle_t _status_mutex = nullptr;
};
