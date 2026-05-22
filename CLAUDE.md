# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-C3 firmware (PlatformIO + Arduino framework) that runs as a **co-processor** for GoGo Board 7. It bridges Vernier Go Direct (D2PIO) BLE sensors — through the in-tree `lib/GoGoVernier` driver over `h2zero/NimBLE-Arduino` — to the host GoGo MCU via a length-prefixed MsgPack protocol on UART0.

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
- **`T_DEV_LIST.slots[].state`** (added 2.1.0, G008): optional `u8` per entry carrying `VernierAdapter::ConnState`'s numeric value — `0=IDLE, 1=REQUESTED, 2=CONNECTING, 3=READY, 4=FAILED`. Pre-2.1.0 hosts ignore the key. Hosts that read it can render a CONNECTING spinner / FAILED toast without inferring state from the absence of T_DEVINFO. `connected:bool` (= state==READY) stays in the frame so the contract is purely additive.
- **Commands**: `C_CONNECT=1, C_DISCONNECT=2, C_SET_PERIOD=3, C_DEV_LIST=4, C_FORGET=5, C_CANCEL_CONNECT=6`. `C_CANCEL_CONNECT` added 2.1.0 (G007) — aborts a still-queued connect via a `REQUESTED→IDLE` CAS on `VernierAdapter::_conn_state`; NACK if the slot has already moved past REQUESTED (bleWorker won the race) or was never pending.
- **`C_CONNECT` ACK timing** (2.1.0, G007): the terminal T_ACK for a successful enqueue arrives early with `ok=true, msg="queued"` instead of waiting for the BLE handshake. Outcome (success or failure) reaches the host via the next auto-pushed `T_DEV_LIST` (Q3 piggyback, see `.claude/plans/release-2.1.0.md` §Decisions). No second ACK on completion.
- **Protocol version**: `VERNIER_PROTOCOL_VERSION` (currently 2) is reported in `T_HELLO`. Bump on any breaking change to message layout. Non-breaking additions (new optional keys, new message types) do not require a bump — the 2.1.0 `state` key and `C_CANCEL_CONNECT` are both additive.

If you add a new wire command:
1. Add the enum value to `UartAdapter::CmdType` in `include/uart-adapter.h`.
2. Add a `cmd*` handler in `src/control-loop.cpp` and a case in `dispatchHostCommand`.
3. Keep numeric values stable — the host MCU's command registry mirrors them.

For a new outbound message type: add to `UartAdapter::MsgType`, add a `sendXxx()` method in `uart-adapter.h/.cpp`, document the frame layout next to the enum.

### Vernier layer

`VernierAdapter` (`include/vernier-adapter.h`, `src/vernier-adapter.cpp`) wraps `gogo_vernier::GoGoVernier` (in-tree at `lib/GoGoVernier/`). It caches device identity, battery/charge/rssi, and per-channel name/unit behind its own accessors (so `main.cpp` never calls into the driver directly for that metadata). `enabledChannelMask()` is a 32-bit mask; iterate bits 0..31 to walk active channels. The sample handoff is push-style — `_gv.onSample(cb)` fires on the NimBLE notify task and the callback enqueues into a depth-2 FreeRTOS queue that the `vernierHandler` task drains via `dev.waitForSample()`.

The driver itself lives in `lib/GoGoVernier/` (BSD-3, ported from `VernierST/godirect-py` + `Vernier-Science-Education/GDXLib`). See `lib/GoGoVernier/README.md` for the public API and `lib/GoGoVernier/src/D2PIOProtocol.h` for the wire-protocol constants. Protocol details (opcodes, frame envelope, GET_STATUS layout) are in `.claude/specs/d2pio-protocol.md` and `.claude/knowledges/vernier-mcu-internals.md`.

### Persistence

NVS namespace `"vernierSetting"`. Keys are per-slot: `"deviceName0"`, `"deviceName1"`, ... — written by `connectAndReport` on every successful connect, cleared by `C_FORGET`. Default value `"proximity"` means "open the first nearby device" in GoGoVernier.

The legacy v1 schema used a single un-suffixed `"deviceName"` key; a one-shot migration in `setup()` copies it into `"deviceName0"` on first v2 boot and leaves the legacy key in place so a v1 firmware rollback still works against the same paired device.

At boot, each slot whose NVS key holds a non-default name is flagged via `slotHasSavedDevice[slot] = true` (and the global `foundSavedDevice` for the fast-path early-out). Auto-connect fires from `autoConnectDevice()` in `loop()` once the host sends its first `C_SET_PERIOD` — sequential per slot since each handshake monopolises the BLE controller.

### Build flags that matter

- `FIRMWARE_FEATURE_FLAG` (always `"vernier"` today) and `FIRMWARE_DEBUG_FLAG` (`"debug"`, `"preview"`, or unset for release) — consumed by `combine-firmware.py:export_firmware_name()` to name dist artifacts.
- `CORE_DEBUG_LEVEL` gates `log_d/log_i/log_e`. The debug env also toggles `CHECK_LOGGING_FLAG(ENABLE_LOGGING_DEBUG)` in `include/debug-flags.h`, which enables the per-channel value dump in `loop()`.
- `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` — `Serial` is USB-CDC (debug log), `gogoSerial` (UART0 via `HardwareSerial(0)`) is the host link. Don't confuse the two.

## Dependencies

Declared in `platformio.ini` `lib_deps`:
- `bblanchon/ArduinoJson@^7` — MsgPack codec.

The Vernier driver lives in-tree at `lib/GoGoVernier/`. Its own `library.json` pulls in `h2zero/NimBLE-Arduino@^2.0.0` transitively, so PlatformIO sees it on every build without an explicit entry. The previous external `MomePP/GDXLib` + `MomePP/ArduinoBLE` pair has been removed — `gogo_vernier::GoGoVernier` replaces both.

The platform is pinned to `pioarduino/platform-espressif32#develop`, not the upstream Espressif platform.

## Code Quality Rules

These rules apply to **all** code changes in this repo, including the in-tree `lib/GoGoVernier/` driver. Follow them without exception:

- **No magic numbers.** Every literal used for sizing, offsets, thresholds, timeouts, or configuration must be a named constant (`constexpr` or `#define`) in the appropriate header. If a value appears in logic, give it a name. Local-only file-scope `constexpr` in an anonymous namespace is acceptable for values that genuinely have no consumer outside the TU — but layout/protocol/wire constants always go in a header.
- **Document packet/protocol formats.** When adding or modifying a wire frame, update the registry comment block beside the relevant enum and describe the byte layout next to the constant:
  - Host ↔ co-processor frames: `UartAdapter::MsgType` / `UartAdapter::CmdType` in `include/uart-adapter.h`.
  - D2PIO frames over BLE: `CmdId` / `MeasurementType` / `RESPONSE_*` in `lib/GoGoVernier/src/D2PIOProtocol.h`.
  - Numeric values are part of the wire contract — never renumber. Add new entries at the end of the enum, and bump `VERNIER_PROTOCOL_VERSION` (or the D2PIO equivalent) only on breaking changes.
- **Keep headers as the single source of truth.** Buffer sizes, frame offsets, sub-command selector bytes, timing values, and threshold constants live in `.h` files — never hard-coded inline in `.cpp` files. Each constant should have a one-line comment explaining the *unit* and *why* the value was chosen.
- **Cross-task shared state.** When a variable is written by one FreeRTOS task and read by another, pick the lightest primitive that documents the contract:
  - **Single scalar with no ordering dependency on other writes** → `std::atomic<T>` with `memory_order_relaxed`. Preferred for new code (e.g. `VernierAdapter::_period_ms`, `_push_dropped`). The type itself documents the cross-task contract; `relaxed` is free on RV32 because aligned word loads/stores are single-instruction atomic.
  - **Fill-then-flag publish pattern** (writer fills a buffer / array, then flips a flag the reader polls) → keep the flag `volatile` and insert a compiler barrier (`__asm__ volatile("" ::: "memory")`) between the buffer writes and the flag store. ESP32-C3 is single-core RISC-V — the Xtensa `memw` fence is not needed (and would not assemble); the compiler barrier alone is sufficient. Used today for the boot hand-off triple `startAutoConnect` / `foundSavedDevice` / `slotHasSavedDevice[]`; sweeping these to `std::atomic` is tracked in `.claude/plans/release-2.1.0.md` under "Out of scope for 2.1.0".
  - **Higher-level handoff** (FreeRTOS queues, semaphores, mutexes) → use the kernel primitive directly. No extra barrier required; the primitive's own ordering covers it.
- **Clean up before committing.** Remove unused constants, dead code, and stale comments (especially phase / refactor commentary that refers to code that no longer exists). Rename identifiers when their purpose changes. `pio check -e check_medium_or_high_defects` should stay clean.
- **Update knowledge before committing.** When adding or changing features, update the relevant `.claude/knowledges/` file BEFORE creating the commit. If no knowledge file exists for the topic, create one. Include: what changed, why, protocol details, opcode numbers, and any backward-compatibility notes. Also verify that existing knowledge files touched by the change are still accurate — fix stale descriptions, outdated constants, wrong file paths, or incorrect API names. Knowledge must always reflect the committed code.
