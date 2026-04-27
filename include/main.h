#pragma once

#define BOOT_BUTTON_PIN 9
#define BUTTON_LONG_PRESS_THRESHOLD 2000

enum ButtonEvent {
    BUTTON_RELEASE,
    BUTTON_PRESS,
    BUTTON_LONG_PRESS,
};

// Firmware version reported in T_HELLO. Keep in sync with the latest
// release tag (see git tag --sort=-v:refname).
#define VERNIER_FW_VERSION_MAJOR 1
#define VERNIER_FW_VERSION_MINOR 2
#define VERNIER_FW_VERSION_PATCH 0

// NOTE: preference nvs keys
const constexpr char *NVS_NAMESPACE_SETTING = "vernierSetting";
const constexpr char *NVS_KEY_DEVICE_NAME   = "deviceName";

