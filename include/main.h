#pragma once

#define BOOT_BUTTON_PIN 9
#define BUTTON_LONG_PRESS_THRESHOLD 2000

enum ButtonEvent {
    BUTTON_RELEASE,
    BUTTON_PRESS,
    BUTTON_LONG_PRESS,
};

// NOTE: preference nvs keys
const constexpr char *NVS_NAMESPACE_SETTING = "vernierSetting";
const constexpr char *NVS_KEY_DEVICE_NAME   = "deviceName";

