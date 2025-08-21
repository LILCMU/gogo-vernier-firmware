/*
In the example, the sensor's name and units are used to create
a header for the data. The header and sensor data are printed to the
Serial Monitor.

For information on Go Direct sensors, the GDXLib functions, and
troubleshooting tips, see the Getting Started Guide at:
https://github.com/VernierST/GDXLib

Steps to Follow:

  1. Set the 'sensor' variable with the sensor number to enable
  2. Set the 'device' variable with the name and serial number of your device
  3. Open the Serial Monitor after the Upload is done
  4. Turn on the Go Direct device

*/

#include <Arduino.h>
#include <HardwareSerial.h>

HardwareSerial gogoSerial(0);
// #undef Serial
// #define Serial gogoSerial

#include "vernier-adapter.h"
#include "uart-adapter.h"

VernierAdapter vernier;
UartAdapter uart(gogoSerial);

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    gogoSerial.begin(115200);
    delay(1000);

    // Serial.println("Starting GDXLib..");
    // if (!GDX.open((char *)device))
    // {
    //     Serial.println("GDX.open() failed. Disconnect/Reconnect USB");
    //     while (true)
    //         ; // if open() fails, put the Arduino into a do-nothing loop
    // }
    // log_i("Successfully found: %s", GDX.getDeviceName().c_str());
    // // log_i("device: %s", GDX.deviceName()); // BUG: not working
    // log_i("order: %s", GDX.orderCode());
    // log_i("serial: %s", GDX.serialNumber());
    // log_i("battery: %d", GDX.batteryPercent());
    // log_i("charge state: %d", GDX.chargeState());
    // log_i("rssi: %d", GDX.RSSI());
    //
    // GDX.enableSensor(sensor);
    // log_i("channel name: %s", GDX.channelName());
    // log_i("channel units: %s", GDX.channelUnits());
    // log_i("channel number: %d", GDX.channelNumber());
    //
    // // print headers using the enabled sensor's name and sensor units
    // sensor_name = GDX.getSensorName(sensor);
    // sensor_unit = GDX.getUnits(sensor);
    // Serial.printf("%s %s\n", sensor_name.c_str(), sensor_unit.c_str());
    //
    // GDX.start(200); // sample period in milliseconds
}

void loop()
{
    // GDX.read();
    // float sensorValue = GDX.getMeasurement(sensor);
    // Serial.println(sensorValue);
}
