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

// #include <HardwareSerial.h>
// HardwareSerial gogoSerial(0);
// #undef Serial
// #define Serial gogoSerial


// #define DEBUG

#include "GDXLib.h"
GDXLib GDX;

// byte sensor = 1;                                   // select the device's sensor to read. In most Go Direct devices, sensor 1 is the default
// const constexpr char *device = "GDX-TMP 0F10H632"; // put your Go Direct name and serial number here, between the quotes
// const constexpr char *device = "GDX-TMP 0F5002FT"; // put your Go Direct name and serial number here, between the quotes
const constexpr char *device = "proximity"; // INFO: scan the nearest device: stronger than -60
byte sensor = 255;                          // INFO: 255 is selected default sensor

String sensor_name = "";
String sensor_unit = "";

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(1000);

    Serial.println("Starting GDXLib..");
    if (!GDX.open((char *)device))
    {
        Serial.println("GDX.open() failed. Disconnect/Reconnect USB");
        while (true)
            ; // if open() fails, put the Arduino into a do-nothing loop
    }
    Serial.print("Successfully found: ");
    Serial.println(GDX.getDeviceName());
    Serial.println();

    GDX.enableSensor(sensor);

    // print headers using the enabled sensor's name and sensor units
    sensor_name = GDX.getSensorName(sensor);
    sensor_unit = GDX.getUnits(sensor);
    Serial.printf("%s %s\n", sensor_name.c_str(), sensor_unit.c_str());

    GDX.start(200); // sample period in milliseconds
}

void loop()
{
    GDX.read();
    float sensorValue = GDX.getMeasurement(sensor);
    Serial.println(sensorValue);
}
