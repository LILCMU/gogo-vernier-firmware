#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

#include "debug-flags.h"
#include "main.h"

#include "framed-msgpack-receiver.h"
#include "uart-adapter.h"
#include "vernier-adapter.h"

namespace {
// Custom vprintf for esp_log → routes via Serial.write() directly,
// bypassing stdio entirely. Lets us close stdout/stderr (to silence
// rogue printfs that bleed onto UART0/gogoSerial) while keeping
// log_*() output visible on USB-CDC. Buffer is per-call stack; safe
// for concurrent log calls from different tasks because each task
// has its own stack.
int serial_log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0)
    {
        size_t to_write = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf);
        Serial.write(reinterpret_cast<const uint8_t *>(buf), to_write);
    }
    return n;
}
}  // namespace

// ---- main.cpp tunables ----------------------------------------------------
//
// Grouped so a future maintainer can tune cadence / buffer sizing in one
// place. None of these need to be header-exported — they're all internal
// to this translation unit.
namespace {

// UART RX command frame buffer. 512 B is enough for the largest host→co
// command we expect (C_SET_PERIOD is < 32 B). Frames larger than this
// are skipped by FramedMsgPackReceiver, not truncated.
constexpr size_t   CMD_RX_BUF_SIZE          = 512;

// Serial (USB-CDC) buffers. setTxBufferSize MUST be called before
// Serial.begin(); see comment in setup() for why default 256 B isn't
// enough during the BLE init log storm.
constexpr size_t   USB_CDC_TX_BUF_SIZE      = 4096;
constexpr size_t   USB_CDC_RX_BUF_SIZE      =  256;

// gogoSerial = HardwareSerial(0) — the host MCU UART link.
constexpr size_t   HOST_SERIAL_TX_BUF_SIZE  = 1024;
constexpr size_t   HOST_SERIAL_RX_BUF_SIZE  = 1024;
constexpr uint32_t HOST_SERIAL_BAUD         = 115200;
constexpr uint32_t USB_CDC_BAUD             = 115200;  // baud is a no-op on
                                                       // HWCDC/USBCDC; kept
                                                       // for convention only

// FreeRTOS task config.
constexpr uint16_t   UART_TASK_STACK        = 6144;    // 6 KB — call depth of
                                                       // vernier.connect() →
                                                       // NimBLEScan + NimBLEClient
                                                       // discovery can hit ~4 KB
                                                       // on its own.
constexpr uint16_t   VERNIER_TASK_STACK     = 8192;
constexpr UBaseType_t HANDLER_TASK_PRIO     = 1;       // both background
                                                       // tasks at idle prio + 1

// vernierHandler cadence.
constexpr uint32_t IDLE_SLEEP_MS            =  250;    // no device open yet
constexpr uint32_t START_RETRY_MS           =   50;    // gap between
                                                       // startReading retries
constexpr uint32_t MAX_SAMPLE_WAIT_MS       =  500;    // upper bound on
                                                       // waitForSample timeout —
                                                       // keeps disconnect
                                                       // detection snappy on
                                                       // long sample periods.
constexpr uint32_t SAMPLE_WAIT_PERIOD_MUL   =    2;    // wait up to 2× period

// uartHandler cadence — 1 ms tick is the minimum FreeRTOS resolution;
// the real bottleneck is FramedMsgPackReceiver::poll's per-frame work.
constexpr uint32_t UART_POLL_MS             =    1;

// DEVSTATS push policy.
constexpr uint32_t DEVSTATS_REFRESH_MS      = 10000;
constexpr int      RSSI_CHANGE_THRESHOLD    =     3;   // dBm
constexpr int      INITIAL_DEVSTATS_SENTINEL = -999;   // "never pushed"

// DEVFIELDS re-emit cadence — every N samples so the host can recover
// its field cache after its own reboot mid-session.
constexpr uint32_t DEVFIELDS_REPUSH_EVERY   = 50;

// Sentinel for connectAndReport's "no host request to ack" case.
constexpr uint32_t NO_HOST_REQUEST          = 0xFFFFFFFFu;

// 16-bit period overflow guard — wire protocol uses uint16_t.
constexpr uint32_t MAX_PERIOD_MS            = 0xFFFFu;

}  // namespace

// setup non-volatile storage
Preferences preferences;

HardwareSerial gogoSerial(0);
// #undef Serial
// #define Serial gogoSerial

// Phase 4 step 3.1: introduce per-slot adapters. Keeping `vernier` as
// a reference to slots[0] preserves the existing single-slot code
// paths verbatim while we migrate uartHandler / connectAndReport /
// vernierHandler to be slot-aware in the following sub-commits
// (3.2 .. 3.5). Slots 1..N exist but stay idle until 3.4 lands the
// first-free slot allocator.
VernierAdapter slots[VERNIER_MAX_SLOTS];
VernierAdapter &vernier = slots[0];
UartAdapter uart(gogoSerial);

TaskHandle_t uartProcessTask, vernierProcessTask;
SemaphoreHandle_t nvsMutex;

static bool startAutoConnect = false;
static bool foundSavedDevice = false;
unsigned long startPressTime = 0;
ButtonEvent prevButtonEvent = BUTTON_RELEASE;

static bool connectAndReport(uint8_t slot, uint32_t req_seq = NO_HOST_REQUEST)
{
    if (slot >= VERNIER_MAX_SLOTS)
    {
        if (req_seq != NO_HOST_REQUEST)
            uart.sendAck(req_seq, false, "slot out of range");
        return false;
    }
    VernierAdapter &dev = slots[slot];
    bool ok = dev.connect((req_seq != NO_HOST_REQUEST) ? true : false);
    if (ok)
    {
        dev.getDeviceInfo();
        // If a previous connect (auto-connect on boot) already started
        // streaming, calling startReading() again restarts the GATT
        // subscription and resets the dropped-sample counter mid-experiment.
        // Only kick off the stream when the link isn't already producing.
        if (!dev.isStreaming())
            dev.startReading(dev.samplingPeriod());

        uart.sendHello();        // global — no dev
        uart.sendStatus(true, 1); // global — no dev (per protocol-v2 spec)
        uart.sendDeviceInfo(dev.deviceName(), dev.orderCode(), dev.serialNumber(), slot);
        uart.sendDeviceStats(dev.batteryPercent(), dev.chargeState(), dev.rssi(), dev.droppedSamples(), slot);

        uint32_t mask = dev.enabledChannelMask();
        const char *names[gogo_vernier::MAX_CHANNELS];
        const char *units[gogo_vernier::MAX_CHANNELS];
        uint8_t count = 0;
        for (uint8_t i = 0; i < gogo_vernier::MAX_CHANNELS; ++i)
        {
            if (mask & (1u << i))
            {
                names[count] = dev.sensorName(i);
                units[count] = dev.sensorUnit(i);
                ++count;
            }
        }
        if (count > 0)
            uart.sendDeviceFields(count, names, units, slot);

        // Persist last connected device name to NVS (writeable). Per-slot
        // NVS keys land in step 3.7; until then, slot 0's name still
        // shadows the legacy single-key schema and other slots are
        // ephemeral (lost on reboot).
        if (slot == 0)
        {
            log_d("Saving device name to NVS: %s", dev.deviceName());
            xSemaphoreTake(nvsMutex, portMAX_DELAY);
            if (preferences.begin(NVS_NAMESPACE_SETTING, false))
            {
                preferences.putString(NVS_KEY_DEVICE_NAME, dev.deviceName());
                preferences.end();
            }
            xSemaphoreGive(nvsMutex);
        }
    }

    if (req_seq != NO_HOST_REQUEST)
        uart.sendAck(req_seq, ok, ok ? nullptr : "connect failed", static_cast<int16_t>(slot));

    return ok;
}

auto autoConnectDevice = []
{
    if (startAutoConnect)
    {
        startAutoConnect = false;

        log_i("Auto-connecting to saved device ...");
        connectAndReport(0); // no request sequence; slot 0 default until step 3.7 adds per-slot NVS
    }
};

auto buttonHandler = []
{
    if (digitalRead(BOOT_BUTTON_PIN) == LOW)
    {
        if (prevButtonEvent == BUTTON_PRESS)
        {
            if ((millis() - startPressTime) > BUTTON_LONG_PRESS_THRESHOLD)
            {
                prevButtonEvent = BUTTON_LONG_PRESS;

                // INFO: long press event
                log_i("disconnect the device");
                vernier.disconnect();
            }
        }
        else if (prevButtonEvent == BUTTON_RELEASE)
        {
            prevButtonEvent = BUTTON_PRESS;
            startPressTime = millis();

            // INFO: press event
            log_i("start connecting nearby device ...");

            if (!vernier.connect(true))
            {
                log_i("GDX.open() failed. Disconnect/Reconnect USB");
            }
        }
    }
    else
    {
        if (prevButtonEvent != BUTTON_RELEASE)
        {
            prevButtonEvent = BUTTON_RELEASE;
        }
    }
};

// Per-slot housekeeping state used by vernierHandler. Lives static so
// state persists across loop iterations without polluting global
// scope.
struct SlotStreamState
{
    int      lastPushedBatt    = INITIAL_DEVSTATS_SENTINEL;
    int      lastPushedCharge  = INITIAL_DEVSTATS_SENTINEL;
    int      lastPushedRssi    = INITIAL_DEVSTATS_SENTINEL;
    uint32_t lastPushedDrop    = 0;
    uint32_t lastRefreshMs     = 0;
    uint32_t samplesSinceFields = 0;
};

void vernierHandler(void *parameter)
{
    static float          sampleBuffer[gogo_vernier::MAX_CHANNELS];
    static SlotStreamState slotState[VERNIER_MAX_SLOTS];

    for (;;)
    {
        bool anyReady   = false;
        bool anySampled = false;
        uint32_t minActivePeriod = MAX_PERIOD_MS;

        for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
        {
            VernierAdapter &dev = slots[i];
            if (!dev.isReady())
                continue;
            anyReady = true;

            if (!dev.isStreaming())
            {
                dev.startReading();
                continue;
            }

            if (dev.samplingPeriod() < minActivePeriod)
                minActivePeriod = dev.samplingPeriod();

            // Non-blocking sample drain. Round-robin across slots so a
            // single slow slot can't block the others. Per-pass cost is
            // one xQueueReceive(timeout=0) per ready slot — cheap.
            size_t sampleCount = 0;
            if (dev.waitForSample(sampleBuffer, sampleCount, 0))
            {
                uart.sendSensorValuesTs(sampleBuffer, sampleCount, millis(), i);
                slotState[i].samplesSinceFields++;
                anySampled = true;
            }

            // DEVSTATS push — change-based, decoupled from sample cadence.
            // Battery/charge/RSSI are cached on connect in VernierAdapter and
            // only refresh when getDeviceInfo() is called, so we do that here
            // on a fixed wall-clock interval and push to the host only when
            // something actually changed (RSSI needs a small threshold since
            // it naturally jitters by 1 dBm on a quiet link).
            {
                uint32_t now = millis();
                if (now - slotState[i].lastRefreshMs >= DEVSTATS_REFRESH_MS)
                {
                    dev.getDeviceInfo(true);
                    slotState[i].lastRefreshMs = now;

                    int batt     = dev.batteryPercent();
                    int charge   = dev.chargeState();
                    int rssi     = dev.rssi();
                    uint32_t drop = dev.droppedSamples();
                    bool changed = (batt != slotState[i].lastPushedBatt)
                                || (charge != slotState[i].lastPushedCharge)
                                || (abs(rssi - slotState[i].lastPushedRssi) >= RSSI_CHANGE_THRESHOLD)
                                || (drop != slotState[i].lastPushedDrop);
                    if (changed)
                    {
                        uart.sendDeviceStats(batt, charge, rssi, drop, i);
                        slotState[i].lastPushedBatt   = batt;
                        slotState[i].lastPushedCharge = charge;
                        slotState[i].lastPushedRssi   = rssi;
                        slotState[i].lastPushedDrop   = drop;
                    }
                }
            }

            // DEVFIELDS re-emit every DEVFIELDS_REPUSH_EVERY samples —
            // helps the host recover its field cache if it rebooted
            // mid-session.
            if (slotState[i].samplesSinceFields >= DEVFIELDS_REPUSH_EVERY)
            {
                uint32_t mask = dev.enabledChannelMask();
                const char *names[gogo_vernier::MAX_CHANNELS];
                const char *units[gogo_vernier::MAX_CHANNELS];
                uint8_t count = 0;
                for (uint8_t k = 0; k < gogo_vernier::MAX_CHANNELS; ++k)
                {
                    if (mask & (1u << k))
                    {
                        names[count] = dev.sensorName(k);
                        units[count] = dev.sensorUnit(k);
                        ++count;
                    }
                }
                if (count > 0)
                    uart.sendDeviceFields(count, names, units, i);

                slotState[i].samplesSinceFields = 0;
            }
        }

        // Sleep policy: scale wake cadence to traffic.
        // - No slot ready → long idle sleep (button or host command will wake us).
        // - At least one ready but no samples this pass → quarter-period
        //   poll (capped MAX_SAMPLE_WAIT_MS so we still respond to
        //   disconnects on long-period setups).
        // - Samples flowed → tight loop, only START_RETRY_MS yield.
        if (!anyReady)
        {
            vTaskDelay(pdMS_TO_TICKS(IDLE_SLEEP_MS));
        }
        else if (!anySampled)
        {
            uint32_t sleep_ms = minActivePeriod / 4u;
            if (sleep_ms < START_RETRY_MS) sleep_ms = START_RETRY_MS;
            if (sleep_ms > MAX_SAMPLE_WAIT_MS) sleep_ms = MAX_SAMPLE_WAIT_MS;
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(START_RETRY_MS));
        }
    }
};

void uartHandler(void *parameter)
{
    // Command IDs from host MCU
    enum CommandType : uint8_t
    {
        C_CONNECT = 1,
        C_DISCONNECT = 2,
        C_SET_PERIOD = 3
    };

    const TickType_t xWaitTime = pdMS_TO_TICKS(UART_POLL_MS);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static uint8_t rxBuffer[CMD_RX_BUF_SIZE];
    FramedMsgPackReceiver cmdRx(gogoSerial, rxBuffer, sizeof(rxBuffer));

    auto onCommand = [](JsonVariantConst root, void *)
    {
        uint8_t c = root["c"] | 0;
        uint32_t req = root["seq"] | 0;

        switch (c)
        {
        case C_CONNECT:
        {
            // First-free slot allocation per design D2: vernier picks
            // the lowest unoccupied slot. Host treats slot id as
            // opaque — never specifies dev on connect, learns it from
            // T_ACK.dev. "Occupied" = ready (handshake done) OR
            // streaming (in transition). isReady() catches the steady
            // state; isStreaming() guards the brief mid-handshake
            // window where ready hasn't flipped yet.
            int8_t target_slot = -1;
            for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
            {
                if (!slots[i].isReady() && !slots[i].isStreaming())
                {
                    target_slot = static_cast<int8_t>(i);
                    break;
                }
            }
            if (target_slot < 0)
            {
                // Per design D9: respond with ok=false + diagnostic
                // msg. Host UI surfaces this to the user.
                uart.sendAck(req, false, "all slots full");
                break;
            }
            connectAndReport(static_cast<uint8_t>(target_slot), req);
            break;
        }
        case C_DISCONNECT:
        {
            // v2 wire: `dev` selects which slot to disconnect. Absent
            // (v1 host) → default 0, identical to legacy behaviour.
            uint8_t dev = root["dev"] | (uint8_t)0;
            if (dev >= VERNIER_MAX_SLOTS)
            {
                uart.sendAck(req, false, "dev out of range");
                break;
            }
            slots[dev].disconnect();
            uart.sendAck(req, true, "disconnected", static_cast<int16_t>(dev));
            break;
        }
        case C_SET_PERIOD:
        {
            // v2 wire (per D5 A): `dev` selects which slot's period
            // to set — each slot keeps its own rate. Absent (v1 host)
            // → default 0.
            uint8_t dev = root["dev"] | (uint8_t)0;
            if (dev >= VERNIER_MAX_SLOTS)
            {
                uart.sendAck(req, false, "dev out of range");
                break;
            }
            // Accept the host's value in u32 first so a 90s/120s setting
            // (mentioned as a use case in CLAUDE.md's adaptive-tick comment)
            // doesn't get silently mod-2^16-truncated at the JSON cast.
            uint32_t period32 = root["period_ms"] | (uint32_t)slots[dev].samplingPeriod();
            if (period32 == 0)
                period32 = VERNIER_DEFAULT_PERIOD_MS;
            if (period32 > MAX_PERIOD_MS)
            {
                uart.sendAck(req, false, "period exceeds 16-bit range");
                break;
            }
            uint16_t period = static_cast<uint16_t>(period32);
            slots[dev].setSamplingRate(period);
            uart.sendAck(req, true, "rate set", static_cast<int16_t>(dev));

            // INFO: start auto-connect after gogo set sampling rate at boot
            if (foundSavedDevice)
                startAutoConnect = true;

            break;
        }
        default:
            uart.sendAck(req, false, "unknown command");
            break;
        }
    };

    cmdRx.setHandler(onCommand, nullptr);

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xWaitTime);

        cmdRx.poll();
    }
};

void setup()
{
    nvsMutex = xSemaphoreCreateMutex();

    // Serial = USB-CDC (ARDUINO_USB_CDC_ON_BOOT=1). The baud argument is
    // a no-op on HWCDC/USBCDC — they ignore it (verified in
    // arduino-esp32 cores/esp32/USBCDC.cpp / HWCDC.cpp; `baud` is
    // declared but never read). Throughput is governed by USB-FS bulk
    // transfers and the on-chip TX ring buffer; the only knob that
    // matters is setTxBufferSize, applied BEFORE begin(). Default is
    // 256 bytes which overruns during the BLE bring-up phase and chops
    // log lines mid-character. 4 KB gives enough slack for the
    // NimBLEDevice / NimBLEScan / NimBLEClient init storm without
    // dropping bytes.
    Serial.setTxBufferSize(USB_CDC_TX_BUF_SIZE);
    Serial.setRxBufferSize(USB_CDC_RX_BUF_SIZE);
    Serial.begin(USB_CDC_BAUD);       // baud value retained for convention only
    Serial.setDebugOutput(true);

    // Override esp_log's vprintf with our Serial-direct hook AFTER
    // setDebugOutput so we beat arduino-esp32's HWCDC handler. Then
    // close stdout/stderr to silence raw printf / puts / fwrite from
    // the underlying ESP-IDF (BT controller, NimBLE host, vendored
    // libs) — those go through stdio whose default fd is UART0,
    // which is the same hardware peripheral we own as gogoSerial for
    // the host-MCU protocol wire. Bleed of NimBLE init log strings
    // onto our wire was the observed corruption that made the host
    // parser see length-prefix garbage. Our hook keeps log_*() calls
    // visible on USB-CDC because it bypasses stdio entirely.
    esp_log_set_vprintf(serial_log_vprintf);
    fclose(stdout);
    fclose(stderr);

    // gogoSerial = HardwareSerial(0) → real UART0 to the host MCU. Baud
    // here IS honoured. Keep at HOST_SERIAL_BAUD — the host firmware
    // speaks the same rate (see CLAUDE.md: "uartHandler ... gogoSerial
    // ... 115200").
    gogoSerial.setTxBufferSize(HOST_SERIAL_TX_BUF_SIZE);
    gogoSerial.setRxBufferSize(HOST_SERIAL_RX_BUF_SIZE);
    gogoSerial.begin(HOST_SERIAL_BAUD);

    pinMode(BOOT_BUTTON_PIN, INPUT);

    // INFO: get device local setting/info from nvs
    xSemaphoreTake(nvsMutex, portMAX_DELAY);
    if (!preferences.begin(NVS_NAMESPACE_SETTING, true)) // NOTE: read-only
    {
        log_e("failed to open nvs: try to enter writable nvs");

        preferences.begin(NVS_NAMESPACE_SETTING, false);
        preferences.putString(NVS_KEY_DEVICE_NAME, VERNIER_DEFAULT_DEVICE_NAME); // default to proximity scan
    }
    String deviceName = preferences.getString(NVS_KEY_DEVICE_NAME, VERNIER_DEFAULT_DEVICE_NAME);
    if (deviceName != VERNIER_DEFAULT_DEVICE_NAME)
    {
        log_i("Loaded saved device name from NVS: %s", deviceName.c_str());
        vernier.setOpenDevice(deviceName.c_str());
        foundSavedDevice = true;
    }

    preferences.end();
    xSemaphoreGive(nvsMutex);

    xTaskCreate(
        uartHandler,
        "UartTask",
        // UART_TASK_STACK absorbs the call depth of vernier.connect(),
        // which dives through GoGoVernier::open → NimBLEScan +
        // NimBLEClient discovery and can come close to 4 KB on its
        // own. Until connect is offloaded to a worker (TODO:
        // pendingConnect flag pattern), keep the headroom.
        UART_TASK_STACK,
        NULL,
        HANDLER_TASK_PRIO,
        &uartProcessTask);

    xTaskCreate(
        vernierHandler,
        "VernierTask",
        VERNIER_TASK_STACK,
        NULL,
        HANDLER_TASK_PRIO,
        &vernierProcessTask);
}

void loop()
{
    autoConnectDevice();

    buttonHandler();

#if CHECK_LOGGING_FLAG(ENABLE_LOGGING_DEBUG)
    static unsigned long startDebugTime = 0;

    // INFO: dynamic debug output of all available channels, this depends on the sampling period
    if (vernier.isStreaming() && (millis() - startDebugTime) > vernier.samplingPeriod())
    {
        uint32_t availableMask = vernier.enabledChannelMask();
        for (uint32_t i = 0; i < 32; i++)
        {
            if (availableMask & (1 << i))
            {
                log_i("%s: %f %s", vernier.sensorName(i), vernier.readMeasurement(i), vernier.sensorUnit(i));

                // const char *unit = vernier.sensorUnit().c_str();
                // log_i("%s: %f %s", name, vernier.readMeasurement(i), unit);
            }
        }
        log_i("");

        startDebugTime = millis();
    }
#endif
}
