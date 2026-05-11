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

The translation units are split by responsibility:

- `src/main.cpp` — globals (UART, NVS, `slots[]`, cross-task volatile flags), FreeRTOS task entrypoints, `setup()`, `loop()`, and `serial_log_vprintf`.
- `src/control-loop.cpp` — host-protocol control flow: `connectAndReport`, `emitDevList`, `autoConnectDevice`, `buttonHandler`, the five `cmd*` per-command handlers, and the `dispatchHostCommand` entry point.
- `src/slot-helpers.cpp` — `NvsScope` RAII, `firstFreeSlot` / `isSlotOccupied` / `slotInRange` predicates, `sendDeviceFieldsFor` marshaller.
- `src/vernier-adapter.cpp` / `src/uart-adapter.cpp` — Vernier device facade and the wire-protocol codec.

Two pinned-priority FreeRTOS tasks plus `loop()`:

- **`uartHandler` task** — owns a `FramedMsgPackReceiver` over `gogoSerial` (HardwareSerial(0), 115200). Polls frames and forwards each parsed root to `dispatchHostCommand(root)` in control-loop.cpp. Wire commands: `C_CONNECT=1`, `C_DISCONNECT=2`, `C_SET_PERIOD=3`, `C_DEV_LIST=4`, `C_FORGET=5` (single source of truth: `UartAdapter::CmdType`).
- **`vernierHandler` task** — round-robins across `slots[]`. For each ready slot it drains the push-mode sample queue via `dev.waitForSample()` (depth-2 FreeRTOS queue fed from the NimBLE notify task), emits `T_SENS_VALUES`, refreshes battery/charge/RSSI on a 10 s wall-clock and pushes a change-based `T_DEVSTATS`, and every 50 samples re-emits `T_FIELDS` so the host can recover its field cache after a reboot. Sleep cadence is adaptive — see `nextSleepMs()`.
- **`loop()`** — `autoConnectDevice()` (fires once after the host sends `C_SET_PERIOD`, walks `slotHasSavedDevice[]` and reconnects each marked slot) and `buttonHandler()` (GPIO9 boot button: short press → connect first-free slot via proximity scan, long press ≥2 s → disconnect every active slot).

A single `nvsMutex` (`SemaphoreHandle_t`) guards all `Preferences` access. Use the `NvsScope` RAII guard in `include/slot-helpers.h` — take the mutex, open the namespace, release both on scope exit. Don't call `preferences.begin/end` directly.

### Host ↔ co-processor wire protocol

Frame: `[uint16 big-endian length][MsgPack payload]`. Payloads are serialized from `ArduinoJson` `JsonDocument`s on both ends.

- **Inbound** (`FramedMsgPackReceiver` in `include/framed-msgpack-receiver.h`): payloads must fit in the caller-provided buffer (512 B in `main.cpp`); oversize frames are skipped, not truncated. Handler receives `JsonVariantConst root`; the doc is cleared after dispatch. Common fields: `c` (u8 command), `seq` (u32 request id), `dev` (u8 slot id — optional on `C_CONNECT`, required on slot-scoped commands), plus command-specific args (e.g. `period_ms`).
- **Outbound** (`UartAdapter` in `src/uart-adapter.cpp`, declared in `include/uart-adapter.h`): message types `T_STATUS=1, T_DEVINFO=2, T_DEVSTATS=3, T_FIELDS=4, T_SENS_VALUES=5, T_ACK=7, T_HELLO=8, T_DEV_LIST=9` (value 6 is retired — the old `T_DEF_VALUE` — do not reuse without a protocol bump). Every send uses the shared `_doc` guarded by `_send_mutex`; the helper writes the 2-byte BE length then the MsgPack body. Per-slot frames carry an optional `dev` field naming the slot.
- **Protocol version**: `VERNIER_PROTOCOL_VERSION` (currently 2) is reported in `T_HELLO`. Bump on any breaking change to message layout. Non-breaking additions (new optional keys, new message types) do not require a bump.

If you add a new wire command:
1. Add the enum value to `UartAdapter::CmdType` in `include/uart-adapter.h`.
2. Add a `cmd*` handler in `src/control-loop.cpp` and a case in `dispatchHostCommand`.
3. Keep numeric values stable — the host MCU's command registry mirrors them.

For a new outbound message type: add to `UartAdapter::MsgType`, add a `sendXxx()` method in `uart-adapter.h/.cpp`, document the frame layout next to the enum.

### Vernier layer

`VernierAdapter` (`include/vernier-adapter.h`, `src/vernier-adapter.cpp`) wraps `GDXLib`. It caches device identity, battery/charge/rssi, and per-channel name/unit behind its own accessors (so `main.cpp` never calls `GDXLib` directly for that metadata). `enabledChannelMask()` is a 32-bit mask; iterate bits 0..31 to walk active channels. `copySample(out, count)` is the single-producer/single-consumer handoff from the GDX poll into the UART sender — it drains `_sample_ready` once per frame.

### Persistence

NVS namespace `"vernierSetting"`. Keys are per-slot: `"deviceName0"`, `"deviceName1"`, ... — written by `connectAndReport` on every successful connect, cleared by `C_FORGET`. Default value `"proximity"` means "open the first nearby device" in GoGoVernier.

The legacy v1 schema used a single un-suffixed `"deviceName"` key; a one-shot migration in `setup()` copies it into `"deviceName0"` on first v2 boot and leaves the legacy key in place so a v1 firmware rollback still works against the same paired device.

At boot, each slot whose NVS key holds a non-default name is flagged via `slotHasSavedDevice[slot] = true` (and the global `foundSavedDevice` for the fast-path early-out). Auto-connect fires from `autoConnectDevice()` in `loop()` once the host sends its first `C_SET_PERIOD` — sequential per slot since each handshake monopolises the BLE controller.

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

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **gogo-vernier-firmware** (643 symbols, 1066 relationships, 25 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> If any GitNexus tool warns the index is stale, run `npx gitnexus analyze` in terminal first.

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `gitnexus_impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `gitnexus_detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `gitnexus_query({query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `gitnexus_context({name: "symbolName"})`.

## Never Do

- NEVER edit a function, class, or method without first running `gitnexus_impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `gitnexus_rename` which understands the call graph.
- NEVER commit changes without running `gitnexus_detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/gogo-vernier-firmware/context` | Codebase overview, check index freshness |
| `gitnexus://repo/gogo-vernier-firmware/clusters` | All functional areas |
| `gitnexus://repo/gogo-vernier-firmware/processes` | All execution flows |
| `gitnexus://repo/gogo-vernier-firmware/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
