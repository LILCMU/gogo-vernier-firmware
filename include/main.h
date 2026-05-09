#pragma once

#include <stddef.h>

#define BOOT_BUTTON_PIN 9
#define BUTTON_LONG_PRESS_THRESHOLD 2000

enum ButtonEvent {
    BUTTON_RELEASE,
    BUTTON_PRESS,
    BUTTON_LONG_PRESS,
};

// Firmware version reported in T_HELLO comes from -D FIRMWARE_*_VERSION
// build flags in platformio.ini (matches the gogo-firmware pattern).
// Keep in sync with the latest release tag.

// NOTE: preference nvs keys
const constexpr char *NVS_NAMESPACE_SETTING = "vernierSetting";

// Legacy single-slot key (v1 schema). Migrated into deviceName0 on
// first v2 boot. Kept defined so the migration code can read it; new
// writes go through NVS_KEY_DEVICE_NAME_FMT below.
const constexpr char *NVS_KEY_DEVICE_NAME   = "deviceName";

// v2 per-slot key format. snprintf(buf, sizeof(buf), NVS_KEY_DEVICE_NAME_FMT, slot)
// produces "deviceName0", "deviceName1", ... — under the 15-char NVS
// key length cap for slot ids up to 9999.
const constexpr char *NVS_KEY_DEVICE_NAME_FMT = "deviceName%u";

// Stack-buffer size for any NVS key built from the formats above.
// ESP32 Preferences caps keys at 15 chars + NUL; this matches that
// budget so a buffer overflow is impossible at compile time.
constexpr size_t NVS_KEY_MAX_LEN = 16;

