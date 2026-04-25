# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-C3 firmware (PlatformIO + Arduino framework) that runs as a **co-processor** for GoGo Board 7. It bridges Vernier Go Direct BLE sensors (via `GDXLib` over `ArduinoBLE`) to the host GoGo MCU through a length-prefixed MsgPack protocol on UART0.

Target board: `esp32-c3-devkitm-1`. C++17 (`-std=gnu++17`). USB CDC-on-boot is enabled.

## Common commands

Built with PlatformIO (`pio`). `default_envs = debug`.

```bash
pio run                          # build default (debug) env
pio run -e release               # no debug logs, no FIRMWARE_DEBUG_FLAG
pio run -e pre-release           # release build tagged "preview"
pio run -e debug                 # verbose logging + esp32_exception_decoder
pio run -t upload                # flash via esptool (921600 baud)
pio device monitor               # serial monitor at 921600
pio run -t clean
pio check -e check_medium_or_high_defects   # cppcheck static analysis
```

After every successful build, `platformio-scripts/combine-firmware.py` runs `esptool merge-bin` and copies both `*.bin` and `*.factory.bin` into `./dist/`. The filename is derived from `FIRMWARE_FEATURE_FLAG` + `FIRMWARE_DEBUG_FLAG` build defines, e.g. `gogo-co-firmware-vernier-debug.bin`. `compilation-db.py` runs pre-build to refresh `compile_commands.json` for clangd.

## Architecture

### Runtime layout

`src/main.cpp` owns all globals and FreeRTOS setup. Two pinned-priority tasks plus `loop()`:

- **`uartHandler` task** — owns a `FramedMsgPackReceiver` over `gogoSerial` (HardwareSerial(0), 115200). Polls frames and dispatches commands from the host MCU: `C_CONNECT=1`, `C_DISCONNECT=2`, `C_SET_PERIOD=3`. Connect flow lives in `connectAndReport()`.
- **`vernierHandler` task** — drives the GDX device: `vernier.poll()` every tick, pushes fresh samples via `uart.sendSensorValuesTs(...)`, and every 50 samples re-emits `DEVSTATS` + `FIELDS` so the host stays in sync. If connected but not streaming, it restarts reading.
- **`loop()`** — `autoConnectDevice()` (fires once after host sends `C_SET_PERIOD` if a saved device was loaded from NVS) and `buttonHandler()` (GPIO9 boot button: short press → scan & connect, long press ≥2s → disconnect).

A single `nvsMutex` (`SemaphoreHandle_t`) guards all `Preferences` access — always take it before `preferences.begin()` and give it back after `preferences.end()`.

### Host ↔ co-processor wire protocol

Frame: `[uint16 big-endian length][MsgPack payload]`. Payloads are serialized from `ArduinoJson` `JsonDocument`s on both ends.

- **Inbound** (`FramedMsgPackReceiver` in `include/framed-msgpack-receiver.h`): payloads must fit in the caller-provided buffer (512 B in `main.cpp`); oversize frames are skipped, not truncated. Handler receives `JsonVariantConst root`; the doc is cleared after dispatch. Command fields: `c` (u8 command), `seq` (u32 request id), plus command-specific args (e.g. `period_ms`).
- **Outbound** (`UartAdapter` in `src/uart-adapter.cpp`, declared in `include/uart-adapter.h`): message types `T_STATUS=1, T_DEVINFO=2, T_DEVSTATS=3, T_FIELDS=4, T_SENS_VALUES=5, T_DEF_VALUE=6, T_ACK=7`. Every send uses the shared `_doc`; the helper writes the 2-byte BE length then the MsgPack body.

If you add a new command or msg type, update both the enum in `main.cpp`'s `uartHandler` (command IDs) and the `UartAdapter::MsgType` enum, and keep the numeric values stable — the host MCU relies on them.

### Vernier layer

`VernierAdapter` (`include/vernier-adapter.h`, `src/vernier-adapter.cpp`) wraps `GDXLib`. It caches device identity, battery/charge/rssi, and per-channel name/unit behind its own accessors (so `main.cpp` never calls `GDXLib` directly for that metadata). `enabledChannelMask()` is a 32-bit mask; iterate bits 0..31 to walk active channels. `copySample(out, count)` is the single-producer/single-consumer handoff from the GDX poll into the UART sender — it drains `_sample_ready` once per frame.

### Persistence

NVS namespace `"vennierSetting"` (typo intentional — changing it orphans existing devices), key `"deviceName"`. Default value `"proximity"` means "open the first nearby device" in GDXLib. Written on every successful connect. At boot, if a non-default name is present, `foundSavedDevice = true` and auto-connect fires after the host sets the sampling period.

### Build flags that matter

- `FIRMWARE_FEATURE_FLAG` (always `"vernier"` today) and `FIRMWARE_DEBUG_FLAG` (`"debug"`, `"preview"`, or unset for release) — consumed by `combine-firmware.py:export_firmware_name()` to name dist artifacts.
- `CORE_DEBUG_LEVEL` gates `log_d/log_i/log_e`. The debug env also toggles `CHECK_LOGGING_FLAG(ENABLE_LOGGING_DEBUG)` in `include/debug-flags.h`, which enables the per-channel value dump in `loop()`.
- `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` — `Serial` is USB-CDC (debug log), `gogoSerial` (UART0 via `HardwareSerial(0)`) is the host link. Don't confuse the two.

## Dependencies

Pulled via `lib_deps` in `platformio.ini`:
- `bblanchon/ArduinoJson@^7` — MsgPack codec.
- `https://github.com/MomePP/GDXLib#develop` — forked Vernier Go Direct driver.
- `https://github.com/MomePP/ArduinoBLE` — forked BLE stack required by the GDXLib fork.

The platform is pinned to `pioarduino/platform-espressif32#develop`, not the upstream Espressif platform.
