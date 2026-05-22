#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

#include "control-loop.h"
#include "debug-flags.h"
#include "main.h"

#include "framed-msgpack-receiver.h"
#include "slot-helpers.h"
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
        if (n < (int)sizeof(buf))
        {
            Serial.write(reinterpret_cast<const uint8_t *>(buf), (size_t)n);
        }
        else
        {
            // Truncated. Force the last byte to '\n' so a long NimBLE
            // format string can't run into the next log line.
            buf[sizeof(buf) - 1] = '\n';
            Serial.write(reinterpret_cast<const uint8_t *>(buf), sizeof(buf));
        }
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

}  // namespace

// setup non-volatile storage
Preferences preferences;

HardwareSerial gogoSerial(0);

VernierAdapter slots[VERNIER_MAX_SLOTS];
UartAdapter uart(gogoSerial);

TaskHandle_t uartProcessTask, vernierProcessTask;
SemaphoreHandle_t nvsMutex;

// Cross-task FreeRTOS hand-off flags. cmdSetPeriod writes
// startAutoConnect (uart task); loop() reads it and dispatches
// autoConnectDevice. foundSavedDevice / slotHasSavedDevice are
// written by setup + connectAndReport (loop/uart contexts) and read
// in autoConnectDevice (loop) + cmdSetPeriod (uart).
// Per host CLAUDE.md cross-task rule: mark volatile to prevent
// compiler hoisting. ESP32-C3 is single-core RISC-V — single-byte
// writes are atomic, so a compiler reordering barrier
// (`__asm__ volatile("" ::: "memory")`) at the publish point is
// sufficient; no Xtensa `memw` is needed (and would not assemble).
// External linkage so control-loop.cpp can extern them.
volatile bool startAutoConnect = false;
volatile bool foundSavedDevice = false;
// Per-slot "has a saved device name in NVS, eligible for auto-connect"
// flag. Populated at boot from NVS keys deviceName0..N. Used by
// autoConnectDevice to know which slots to attempt on the
// startAutoConnect trigger from cmdSetPeriod.
volatile bool slotHasSavedDevice[VERNIER_MAX_SLOTS] = {false};

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

// Scale wake cadence to traffic:
// - No slot ready → long idle sleep (button or host command wakes us).
// - At least one ready but no samples this pass → quarter-period poll,
//   bounded by [START_RETRY_MS, MAX_SAMPLE_WAIT_MS] so we still notice
//   disconnects on long-period setups.
// - Samples flowed this pass → tight loop, only START_RETRY_MS yield.
static uint32_t nextSleepMs(bool anyReady, bool anySampled, uint32_t minActivePeriod)
{
    if (!anyReady)  return IDLE_SLEEP_MS;
    if (anySampled) return START_RETRY_MS;
    uint32_t s = minActivePeriod / 4u;
    if (s < START_RETRY_MS)     s = START_RETRY_MS;
    if (s > MAX_SAMPLE_WAIT_MS) s = MAX_SAMPLE_WAIT_MS;
    return s;
}

void vernierHandler(void *parameter)
{
    static float          sampleBuffer[gogo_vernier::MAX_CHANNELS];
    static SlotStreamState slotState[VERNIER_MAX_SLOTS];

    for (;;)
    {
        bool anyReady   = false;
        bool anySampled = false;
        // samplingPeriod() returns uint16_t, so UINT16_MAX is the
        // unreachable sentinel for the per-pass min reduction.
        uint32_t minActivePeriod = UINT16_MAX;

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
            //
            // H6: refresh + read happen under lockStatus() so a concurrent
            // bleWorker / publishConnectResult getDeviceInfo on the same
            // slot can't tear the battery/charge/rssi triple. Recursive
            // mutex makes the nested getDeviceInfo lock safe.
            {
                uint32_t now = millis();
                if (now - slotState[i].lastRefreshMs >= DEVSTATS_REFRESH_MS)
                {
                    dev.lockStatus();
                    dev.getDeviceInfo(true);
                    slotState[i].lastRefreshMs = now;

                    int batt     = dev.batteryPercent();
                    int charge   = dev.chargeState();
                    int rssi     = dev.rssi();
                    uint32_t drop = dev.droppedSamples();
                    dev.unlockStatus();

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
                sendDeviceFieldsFor(uart, dev, i);
                slotState[i].samplesSinceFields = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(nextSleepMs(anyReady, anySampled, minActivePeriod)));
    }
};

void uartHandler(void *parameter)
{
    const TickType_t xWaitTime = pdMS_TO_TICKS(UART_POLL_MS);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static uint8_t rxBuffer[CMD_RX_BUF_SIZE];
    FramedMsgPackReceiver cmdRx(gogoSerial, rxBuffer, sizeof(rxBuffer));

    cmdRx.setHandler([](JsonVariantConst root, void *) {
        dispatchHostCommand(root);
    }, nullptr);

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

    // INFO: get device local setting/info from nvs.
    //
    // v2 schema: per-slot keys deviceName0..N. v1 schema had a single
    // deviceName key — migrated to deviceName0 on first v2 boot,
    // legacy key left in place so a rollback to v1 firmware still
    // works against the same paired device.
    {
        NvsScope nvs(nvsMutex, preferences, NVS_NAMESPACE_SETTING, false); // RW so we can migrate
        if (!nvs)
        {
            log_e("failed to open nvs (rw)");
        }
        else
        {
            // One-shot migration: if legacy "deviceName" exists but the
            // slot-0 key doesn't, copy across. Won't run again on
            // subsequent boots because the slot-0 key will then exist.
            char slot0_key[NVS_KEY_MAX_LEN];
            snprintf(slot0_key, sizeof(slot0_key), NVS_KEY_DEVICE_NAME_FMT, 0u);
            if (preferences.isKey(NVS_KEY_DEVICE_NAME) && !preferences.isKey(slot0_key))
            {
                String legacy = preferences.getString(NVS_KEY_DEVICE_NAME, "");
                if (legacy.length() > 0 && legacy != VERNIER_DEFAULT_DEVICE_NAME)
                {
                    preferences.putString(slot0_key, legacy.c_str());
                    log_i("NVS migration: %s -> %s (%s)",
                          NVS_KEY_DEVICE_NAME, slot0_key, legacy.c_str());
                }
            }

            // Per-slot load.
            for (uint8_t i = 0; i < VERNIER_MAX_SLOTS; ++i)
            {
                char key[NVS_KEY_MAX_LEN];
                snprintf(key, sizeof(key), NVS_KEY_DEVICE_NAME_FMT, (unsigned)i);
                String name = preferences.getString(key, VERNIER_DEFAULT_DEVICE_NAME);
                if (name != VERNIER_DEFAULT_DEVICE_NAME)
                {
                    log_i("Slot %u: loaded saved name from NVS: %s",
                          (unsigned)i, name.c_str());
                    slots[i].setOpenDevice(name.c_str());
                    slotHasSavedDevice[i] = true;
                    foundSavedDevice = true;
                }
            }
        }
    }

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

    // bleWorker task (G006) — drains the ConnectRequest queue and
    // runs dev.connect() off the uartHandler critical path. No
    // producer in G006; cmdConnect / buttonHandler / autoConnectDevice
    // are cut over to enqueueConnect in G007. The worker just parks
    // on its queue until then.
    bleWorkerStart();
}

void loop()
{
    autoConnectDevice();

    buttonHandler();

#if CHECK_LOGGING_FLAG(ENABLE_LOGGING_DEBUG)
    static unsigned long startDebugTime = 0;
    // slots[0] alias — the per-channel dump below is the only consumer
    // and only in debug builds, so the reference lives inline rather
    // than as a file-scope global the release build would carry as an
    // unused symbol.
    VernierAdapter &vernier = slots[0];

    // INFO: dynamic debug output of all available channels, this depends on the sampling period
    if (vernier.isStreaming() && (millis() - startDebugTime) > vernier.samplingPeriod())
    {
        uint32_t availableMask = vernier.enabledChannelMask();
        for (uint32_t i = 0; i < gogo_vernier::MAX_CHANNELS; i++)
        {
            if (availableMask & (1u << i))
            {
                log_i("%s: %f %s", vernier.sensorName(i), vernier.readMeasurement(i), vernier.sensorUnit(i));
            }
        }
        log_i("");

        startDebugTime = millis();
    }
#endif
}
