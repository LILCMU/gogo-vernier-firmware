#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>

#include "debug-flags.h"
#include "main.h"

#include "framed-msgpack-receiver.h"
#include "uart-adapter.h"
#include "vernier-adapter.h"

// setup non-volatile storage
Preferences preferences;

HardwareSerial gogoSerial(0);
// #undef Serial
// #define Serial gogoSerial

VernierAdapter vernier;
UartAdapter uart(gogoSerial);

TaskHandle_t uartProcessTask, vernierProcessTask;
SemaphoreHandle_t nvsMutex;

static bool startAutoConnect = true;
unsigned long startPressTime = 0;
ButtonEvent prevButtonEvent = BUTTON_RELEASE;

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

static bool connectAndReport(uint32_t req_seq = 0xFFFFFFFFu)
{
    bool ok = vernier.connect((req_seq != 0xFFFFFFFFu) ? true : false);
    if (ok)
    {
        vernier.getDeviceInfo();
        vernier.startReading(vernier.samplingPeriod());

        uart.sendStatus(true, 1);
        uart.sendDeviceInfo(vernier.deviceName(), vernier.orderCode(), vernier.serialNumber());
        uart.sendDeviceStats(vernier.batteryPercent(), vernier.chargeState(), vernier.rssi());

        uint32_t mask = vernier.enabledChannelMask();
        const uint8_t maxCh = 32;
        const char *names[maxCh];
        const char *units[maxCh];
        uint8_t count = 0;
        for (uint8_t i = 0; i < maxCh; ++i)
        {
            if (mask & (1u << i))
            {
                names[count] = vernier.sensorName(i);
                units[count] = vernier.sensorUnit(i);
                ++count;
            }
        }
        if (count > 0)
            uart.sendDeviceFields(count, names, units);

        // Persist last connected device name to NVS (writeable)
        log_d("Saving device name to NVS: %s", vernier.deviceName());
        xSemaphoreTake(nvsMutex, portMAX_DELAY);
        if (preferences.begin(NVS_NAMESPACE_SETTING, false))
        {
            preferences.putString(NVS_KEY_DEVICE_NAME, vernier.deviceName());
            preferences.end();
        }
        xSemaphoreGive(nvsMutex);
    }

    if (req_seq != 0xFFFFFFFFu)
        uart.sendAck(req_seq, ok, ok ? nullptr : "connect failed");

    return ok;
}

void vernierHandler(void *parameter)
{
    const TickType_t xWaitTime = pdMS_TO_TICKS(1);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static float sampleBuffer[32];
    size_t sampleCount = 0;
    uint32_t samplesSinceLastStats = 0;

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xWaitTime);

        if (vernier.isConnected() && vernier.isStreaming())
        {
            vernier.poll();

            if (vernier.sampleReady())
            {
                if (vernier.copySample(sampleBuffer, sampleCount))
                {
                    uart.sendSensorValuesTs(sampleBuffer, sampleCount, millis());
                    samplesSinceLastStats++;
                }
            }

            if (samplesSinceLastStats >= 50)
            {
                uart.sendDeviceStats(vernier.batteryPercent(), vernier.chargeState(), vernier.rssi());

                uint32_t mask = vernier.enabledChannelMask();
                const uint8_t maxCh = 32;
                const char *names[maxCh];
                const char *units[maxCh];
                uint8_t count = 0;
                for (uint8_t i = 0; i < maxCh; ++i)
                {
                    if (mask & (1u << i))
                    {
                        names[count] = vernier.sensorName(i);
                        units[count] = vernier.sensorUnit(i);
                        ++count;
                    }
                }
                if (count > 0)
                    uart.sendDeviceFields(count, names, units);

                samplesSinceLastStats = 0;
            }
        }
        else if (vernier.isConnected() && !vernier.isStreaming())
        {
            vernier.startReading();
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

    const TickType_t xWaitTime = pdMS_TO_TICKS(1);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static uint8_t rxBuffer[512];
    FramedMsgPackReceiver cmdRx(gogoSerial, rxBuffer, sizeof(rxBuffer));

    auto onCommand = [](JsonVariantConst root, void *)
    {
        uint8_t c = root["c"] | 0;
        uint32_t req = root["seq"] | 0;

        switch (c)
        {
        case C_CONNECT:
        {
            connectAndReport(req);
            break;
        }
        case C_DISCONNECT:
        {
            vernier.disconnect();
            uart.sendAck(req, true, "disconnected");
            break;
        }
        case C_SET_PERIOD:
        {
            uint16_t period = root["period_ms"] | vernier.samplingPeriod();
            if (period == 0)
                period = 1000;
            vernier.setSamplingRate(period);
            uart.sendAck(req, true, "rate set");

            // INFO: start auto-connect after gogo set sampling rate at boot
            if (startAutoConnect)
            {
                delay(1000);
                connectAndReport(); // no req -> no ack
                startAutoConnect = false;
            }
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

    Serial.begin(115200);
    Serial.setDebugOutput(true);

    gogoSerial.begin(115200);

    pinMode(BOOT_BUTTON_PIN, INPUT);

    log_i("gogo vernier firmware: %d.%d.%d", FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION, FIRMWARE_PATCH_VERSION);

    // INFO: get device local setting/info from nvs
    xSemaphoreTake(nvsMutex, portMAX_DELAY);
    if (!preferences.begin(NVS_NAMESPACE_SETTING, true)) // NOTE: read-only
    {
        log_e("failed to open nvs: try to enter writable nvs");

        preferences.begin(NVS_NAMESPACE_SETTING, false);
        preferences.putString(NVS_KEY_DEVICE_NAME, VERNIER_DEFAULT_DEVICE_NAME); // default to proximity scan
    }
    String deviceName;
    vernier.setOpenDevice(preferences.getString(NVS_KEY_DEVICE_NAME, VERNIER_DEFAULT_DEVICE_NAME).c_str());

    preferences.end();
    xSemaphoreGive(nvsMutex);

    xTaskCreate(
        uartHandler,
        "UartTask",
        4096,
        NULL,
        1,
        &uartProcessTask);

    xTaskCreate(
        vernierHandler,
        "VernierTask",
        8192,
        NULL,
        1,
        &vernierProcessTask);
}

void loop()
{
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
