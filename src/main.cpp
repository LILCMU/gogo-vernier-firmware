#include <Arduino.h>
#include <HardwareSerial.h>

#include "debug-flags.h"
#include "main.h"

#include "uart-adapter.h"
#include "vernier-adapter.h"

HardwareSerial gogoSerial(0);
// #undef Serial
// #define Serial gogoSerial

VernierAdapter vernier;
UartAdapter uart(gogoSerial);

TaskHandle_t vernierProcessTask;

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

            if (!vernier.connect())
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

void vernierHandler(void *parameter)
{
    const TickType_t xWaitTime = pdMS_TO_TICKS(1);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xWaitTime);

        if (vernier.isConnected() && vernier.isStreaming())
        {
            vernier.poll();
        }
        else if (vernier.isConnected() && !vernier.isStreaming())
        {
            vernier.startReading();
        }

#if CHECK_LOGGING_FLAG(ENABLE_LOGGING_DEBUG)
        static unsigned long startDebugTime = 0;

        if (vernier.isStreaming() && (millis() - startDebugTime) > 1000)
        {
            uint32_t availableMask = vernier.availableChannels();
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
};

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    gogoSerial.begin(115200);

    pinMode(BOOT_BUTTON_PIN, INPUT);

    delay(500);
    log_i("gogo vernier firmware: %d.%d.%d", FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION, FIRMWARE_PATCH_VERSION);

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
}
