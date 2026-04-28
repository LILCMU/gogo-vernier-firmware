# GDXLib rewrite plan — GoGoVernier (working name)

## Why rewrite

Current `MomePP/GDXLib` works but has friction:

1. **7-channel ceiling.** `getMeasurement()` walks `g_firstEnabledSensor … g_seventhEnabledSensor`
   plus `g_measurement1 … g_measurement7`. Wire protocol uses a 32-bit `sensorMask` —
   API drops the rest.
2. **Globals everywhere.** `g_d2pioCommand`, `g_d2pioResponse`, `g_*EnabledSensor` are
   TU-static in `GDXLib.cpp`. Two `GDXLib` instances would corrupt each other. Multi-device
   impossible without ripping that out.
3. **Hard-coupled to ArduinoBLE.** `#include "ArduinoBLE.h"` in the public header forces
   every consumer onto ArduinoBLE. ArduinoBLE rolls its own HCI host on top of
   `esp_bt_controller_*` — the exact path that crashes on arduino-esp32 ≥ 3.3.7 / IDF 5.5.4
   (`r_lld_env_init` NULL fn ptr). Migrating off it is both a fix and a forcing function.
4. **Sync polling style.** `D2PIO_ReadBlocking` / `GDX_ReadMeasurement` block the calling
   task. No callback path means consumers can't react to live frames or status changes
   promptly.
5. **No public protocol docs — but reference impl now exists.** Vernier never
   published the Go Direct OTA protocol formally, but their official Python
   library [`VernierST/godirect-py`](https://github.com/VernierST/godirect-py)
   ships the full opcode set, frame layout, and per-sensor record decoder in
   plain Python. That removes reverse-engineering from the critical path —
   the rewrite becomes a port + structural cleanup, not an RE project. Current
   `MomePP/GDXLib` is now the lower-bound spec; godirect-py is the upper-bound spec.

## Goals (priority order)

1. **Drop-in for `vernier-adapter.cpp`.** New lib must satisfy every call site:
   open/close/start/stop/getMeasurement/getAvailableChannels/getDeviceName/orderCode/
   serialNumber/batteryPercent/chargeState/RSSI/GDX_ReadMeasurement. Either same names
   or `VernierAdapter` updated in lockstep.
2. **NimBLE backend, no ArduinoBLE.** Compile + run against arduino-esp32 3.3.x
   stable NimBLE host. No platform pin.
3. **Multi-channel up to 32.** Replace globals with `_channels[32]` array.
   `getMeasurement(byte)` becomes O(1) lookup.
4. **Concurrent multi-device support.** Up to N Vernier sensors connected at
   the same time on a single ESP32-C3 (N bounded by
   `CONFIG_NIMBLE_MAX_CONNECTIONS`, default 3). Every piece of GDX state —
   characteristic handles, rolling counter, channel array, callbacks — lives
   on the instance, never in `static` / file-scope globals. The host-side
   wire protocol (`UartAdapter` in `vernier-firmware`) gains a per-frame
   device id so the GoGo MCU can distinguish samples from different
   physically-attached sensors.
5. **Async-friendly.** Notify path delivers samples via callback / ring buffer.
6. **Fake-peer testable.** Host-side mock GATT server (Python aioble) for CI.

## Non-goals

- BLE peripheral / advertising. Central only.
- Bluedroid backend. NimBLE only.
- Backwards-compatible API surface. Rename freely if `VernierAdapter` updated same PR.
- Reverse-engineering new opcodes. Stick to what current GDXLib uses.

## Protocol — confirmed against godirect-py

UUIDs (cross-checked: same in `MomePP/GDXLib::D2PIO_DiscoverService` and
`godirect-py/godirect/device_ble.py`):

| Element | Value |
|---|---|
| BLE service UUID | `d91714ef-28b9-4f91-ba16-f0d9a604f112` |
| Command (write) char | `f4bf14a6-c7d5-4b6d-8aa8-df1a7c83adcb` |
| Response (notify) char | `b41e6675-a329-40e0-aa01-44d2f444babe` |

Command opcodes (verbatim from `godirect-py/godirect/device.py`):

| Name | Opcode |
|---|---|
| `CMD_ID_START_MEASUREMENTS` | `0x18` |
| `CMD_ID_STOP_MEASUREMENTS` | `0x19` |
| `CMD_ID_INIT` | `0x1A` |
| `CMD_ID_SET_MEASUREMENT_PERIOD` | `0x1B` |
| `CMD_ID_GET_SENSOR_INFO` | `0x50` |
| `CMD_ID_GET_SENSOR_AVAILABLE_MASK` | `0x51` |
| `CMD_ID_DISCONNECT` | `0x54` |
| `CMD_ID_GET_DEVICE_INFO` | `0x55` |
| `CMD_ID_GET_DEFAULT_SENSORS_MASK` | `0x56` |

Measurement-frame TLVs (notify char payload tags, verbatim):

| Name | Tag |
|---|---|
| `MEASUREMENT_TYPE_NORMAL_REAL32` | `0x06` |
| `MEASUREMENT_TYPE_WIDE_REAL32` | `0x07` |
| `MEASUREMENT_TYPE_SINGLE_CHANNEL_REAL32` | `0x08` |
| `MEASUREMENT_TYPE_SINGLE_CHANNEL_INT32` | `0x09` |
| `MEASUREMENT_TYPE_APERIODIC_REAL32` | `0x0a` |
| `MEASUREMENT_TYPE_APERIODIC_INT32` | `0x0b` |
| `MEASUREMENT_TYPE_START_TIME` | `0x0c` |
| `MEASUREMENT_TYPE_DROPPED` | `0x0d` |
| `MEASUREMENT_TYPE_PERIOD` | `0x0e` |
| `RESPONSE_MEASUREMENT` | `0x20` |

Frame layout (see `.claude/specs/d2pio-protocol.md` for the canonical version):
- byte 0: header magic `0x58` on request, response op (e.g. `0x20`) on reply
- byte 1: total length, inclusive of header through last payload byte
- byte 2: rolling counter (decremented per send, wraps `0xFF` after `0x00`)
- byte 3: checksum — 8-bit sum of every byte except byte 3 itself
  (`_GDX_calculate_checksum`). Not a 1's complement.
- byte 4: command id on request, echoed cmd id or measurement sub-type on
  response
- bytes 5..N-1: command-specific payload

`SET_MEASUREMENT_PERIOD` payload = period in **microseconds**, little-endian
(matches existing GDXLib).

`START_MEASUREMENTS` payload = 32-bit sensor mask + reserved bytes.

Sensor info record (per channel, decoded from `GET_SENSOR_INFO` response —
fields from `godirect-py/godirect/sensor.py`):

| Field | Notes |
|---|---|
| `sensor_number` | 0..31 |
| `sensor_description` | UTF-8 string |
| `sensor_id` | u32 — Vernier's catalogue id |
| `numeric_measurement_type` | one of `MEASUREMENT_TYPE_*` |
| `sampling_mode` | periodic / aperiodic |
| `sensor_units` | UTF-8 string |
| `measurement_uncertainty` | float |
| `min_measurement`, `max_measurement` | float, hardware range |
| `typ_measurement_period`, `min_measurement_period`, `max_measurement_period` | µs |
| `measurement_period_granularity` | µs step |
| `mutual_exclusion_mask` | 32-bit — bits set = sensors that cannot coexist |
| `enabled` | runtime flag |

**Mutual exclusion** is real protocol behaviour we currently ignore: e.g.
GDX-3MG's low-range vs high-range axes, GDX-ACC ranges. New API must check
`mutual_exclusion_mask` before letting `enableSensor()` succeed, and either
auto-disable conflicts or return an error.

Charger states (from device.py):

| Name | Value |
|---|---|
| `CHARGER_STATE_IDLE` | 0 |
| `CHARGER_STATE_CHARGING` | 1 |
| `CHARGER_STATE_COMPLETE` | 2 |
| `CHARGER_STATE_ERROR` | 3 |

These get extracted into `.claude/specs/d2pio-protocol.md` so opcodes stop
being tribal knowledge buried in 1500-line .cpp.

## Supported devices (Vernier TIL/16315)

25 Go Direct devices documented at <https://www.vernier.com/til/16315>.
Channel counts span 1..9. Notable shapes:

| Device | Channels | Comment |
|---|---|---|
| GDX-3MG | 6 | low/high range mutually exclusive (3+3) |
| GDX-ACC | 9 | accel low+high (mut-ex) + gyro + altitude + tilt |
| GDX-CART | 5 | position + force + 3-axis accel |
| GDX-CCS | 1 | constant-current source |
| GDX-CO2 | 3 | CO2 + temp + RH |
| GDX-COL | 1 | colorimeter transmittance |
| GDX-CON | 3 | conductivity (TC + raw) + temp |
| GDX-DC | 1 | drop counter / volume |
| GDX-EA | 2 | electrode amp (mV + pH) |
| GDX-EKG | 4 | EKG + heart rate + EMG + EMG-rectified |
| GDX-ETOH | 1 | ethanol vapor |
| GDX-HD | 7 | force + 3-axis accel + 3-axis gyro |
| GDX-ISEA | 6 | potential + 5 ion calibrations |
| GDX-NRG | 5 | V + I + P + R + E |
| GDX-LC | 7 | lux + UV + (3 unused) + 615 + 525 + 465 nm |
| GDX-MD | 7 | (4 unused) + motion + motion-cart + motion-TC |
| GDX-O2 | 3 | O2 + O2-TC + temp |
| GDX-ODO | 3+ | DO concentration + DO saturation + temp |
| (others) | — | GDX-RB, GDX-RMS, GDX-SND, GDX-ST, GDX-TMP, GDX-WRT, GDX-WTHR |

**Max active channels per device today: 9**. The current 7-channel ceiling in
GDXLib silently drops GDX-ACC's 8th and 9th channels and any device that adds
more in the future. The new lib's 32-channel array covers all known + headroom.

## Architecture

```
+----------------------------+
|  GoGoVernier (public API)  |   <- thin facade over state machine
+----------------------------+
             |
+----------------------------+
|  D2PIOSession              |   <- protocol state machine, encoders/
|   - encodeInit()           |      decoders, frame checksum, reassembly
|   - encodeStart(mask)      |
|   - decodeStatus(buf)      |
|   - decodeMeasurement(buf) |
|   - onNotify(buf, len)     |
+----------------------------+
             |
+----------------------------+
|  BleTransport (interface)  |   <- write(buf), onNotify(cb), connect()
+----------------------------+
       |             |
+--------------+  +----------------+
| NimBLEXport  |  | FakeXport      |   <- one real, one for tests
| (NimBLE-Ard) |  | (loopback)     |
+--------------+  +----------------+
```

`BleTransport` interface = the seam. Lets us swap NimBLE-Arduino for arduino-esp32's
bundled `BLE` library without touching protocol layer. Lets host-side tests run on
loopback.

## Phase plan

### Phase 0 — bootstrap, no behaviour change
- Fork `MomePP/GDXLib` to new repo (or rename branch) `gogo-vernier`.
- Add `.claude/specs/d2pio-protocol.md` distilled from `godirect-py` opcodes
  + `MomePP/GDXLib` framing. godirect-py is the reference for any disagreement.
- Smoke test: connect → battery% → disconnect.
- Old `GDXLib` typedef preserved so `vernier-firmware` builds unchanged.

### Phase 1 — backend swap to NimBLE
- Add `NimBLEXport` against `h2zero/NimBLE-Arduino`.
- Build with arduino-esp32 stable (3.3.7 / IDF 5.5.4 / NimBLE host).
- Phase-0 smoke test still passes.
- Drop `#include "ArduinoBLE.h"` from public header.

### Phase 2 — kill globals, full 32-channel support
- Move `g_d2pioCommand`, `g_d2pioResponse`, `g_*EnabledSensor`, `g_measurement1..7`
  onto `D2PIOSession` instance state.
- Replace 7-way `if/else` in `getMeasurement()` with `_channels[32]` array of
  the per-sensor record from godirect-py's `Sensor` (description, id, units,
  measurement type, ranges, periods, mutual_exclusion_mask, enabled, value).
- `GDX_ReadMeasurement` decodes live frame straight into `_channels[]` —
  switch on `MEASUREMENT_TYPE_*` tag to handle real32 / int32 / aperiodic /
  dropped / period sub-frames as godirect-py's `_GDX_handle_measurement` does.
- Honour `mutual_exclusion_mask` in `enableSensor()` — reject or auto-disable
  the conflict and report which mask bit conflicted.

### Phase 3 — async sample path
- `onSample(std::function<void(const Sample&)>)` callback.
- Internal ring buffer for `vernierHandler`-style polling consumers.
- `GDX_ReadMeasurement` becomes thin wrapper preserving Phase-0 callers.

### Phase 4 — vernier-firmware integration
- Update `vernier-adapter.cpp` to consume new API. Replace single
  `VernierAdapter` global with a slot table (`VernierAdapter[3]`) keyed by
  device id; `connectAndReport` accepts a slot index; UART msgs gain a
  `dev` field.
- Extend host wire protocol (`include/uart-adapter.h`) with `dev` (u8) on
  every per-device frame: `T_DEVINFO`, `T_DEVSTATS`, `T_FIELDS`,
  `T_SENS_VALUES`, `T_HELLO` stays device-agnostic. Add `T_DEV_LIST` so the
  host can enumerate occupied slots.
- Drop `MomePP/ArduinoBLE` from `lib_deps`.
- Unpin platform → back to `pioarduino/...stable...`.
- Smoke test on real GoGo + 1× GDX-LC + 1× GDX-TMP simultaneously, then
  push to 3× simultaneously (NimBLE max).

### Phase 5 — tests, CI
- Host-side fake GATT server: extend `godirect-py` to act as the device side
  of the protocol (server) instead of just the client; replays canned frames
  for each GDX device family. Saves writing a new sim from scratch.
- GitHub Actions: build-only on each PR; manual workflow for HIL on lab
  board against a real GDX-LC + GDX-ACC (covers single-channel + 9-channel +
  mutual-exclusion paths).
- Conformance suite: for each opcode + measurement type, an encoder/decoder
  round-trip test cross-checks our C++ output byte-for-byte against
  godirect-py's bytes for the same input.

## Deliverables

- New library repo with layered design.
- `.claude/specs/d2pio-protocol.md` — opcode table, frame format, examples.
- `examples/Basic.ino`, `examples/MultiChannel.ino`.
- Migration notes for old `GDXLib` consumers.
- This plan file (`.claude/plans/gdxlib-rewrite.md`) updated as decisions land.

## References

- `VernierST/godirect-py` — official Python implementation. Authoritative for
  opcode / frame / sensor-record layouts. Track upstream commits for new
  measurement types or device-info additions.
- `MomePP/GDXLib` — current C++ implementation. Authoritative for
  Arduino-flavoured BLE characteristic discovery, MTU / chunking behaviour
  observed empirically on ESP32, and the rolling-counter quirk.
- <https://www.vernier.com/til/16315> — supported devices + channel lists.
  Refresh per release; channel lists are how end users will phrase support
  requests ("does it support GDX-RB?").

## Open questions

- **Multi-instance throughput.** Multi-device is now a goal, not a stretch.
  Open variables: per-link RAM cost (NimBLE allocates per-conn buffers),
  aggregate sample bandwidth at 9 ch × 3 devs × 100 ms = 270 floats/s plus
  UART back-pressure on `gogoSerial` at 115200. Budget on hardware in Phase 4
  before declaring 3-device support GA.
- **Chunking on response char.** Long channel-info payloads may arrive across
  multiple notify frames. godirect-py's `_GDX_read_blocking` accumulates by
  declared length until the frame closes — port that exact logic.
- **Power model.** Should `stop()` deinit NimBLE to save power, or keep host
  stack hot? Current GDXLib keeps it hot. godirect-py keeps the BLE backend
  alive too; defer optimisation.
- **Vendor stability.** D2PIO opcode set could change if Vernier ships new
  sensor families. godirect-py has been stable on these opcodes since 2018.
  Version-check on `INIT` reply still recommended.
