# D2PIO BLE bring-up — hard-won findings

Captured during the Phase 1 / Phase 2 GoGoVernier debug saga. Each entry
is a non-obvious fact that cost real time to discover. Future maintainers
should read this before tweaking BLE / log / Serial config.

## Session-end snapshot — pick up here

Phase 4 multi-device backend complete on both sides. Wire is v2,
vernier owns a 3-slot pool with first-free allocation + per-slot
NVS persistence + slot-aware command routing, host has the slot
table backend (per-slot dispatch, liveness, enumeration accessors).

What's left for full multi-device: **step 4b** (host UI slot
list + drill-down per design D8 Option C — touches gogo-display
+ gogo-firmware UI cases) and **step 5** (multi-device hardware
smoke with 2-3 GDX devices simultaneously).

Single-device behaviour preserved end-to-end at every step:
legacy callers and v1 peers all transparently land on slot 0
with identical wire bytes (modulo the optional `dev` field that
v1 ignores).

Key recent work (this session):
- Step 2: protocol v2 bump — non-breaking in either direction.
  Both firmwares now declare VERNIER_PROTOCOL_VERSION=2 and
  VERNIER_MAX_SLOTS=3.
- Step 3: vernier slot pool — 8 sub-commits, see plan doc for
  the SHA list and per-commit scope.
- Step 4a: host backend slot table — 5 sub-commits. UI still
  renders slot 0 only via the legacy accessors that route
  through `_slots[_primary_slot=0]`. cyclePrimarySlot() and
  slot-indexed accessors ready for step 4b's UI to consume.

Working-tree state: clean on both sides (all step-3 + step-4a
commits committed; vernier pushed to develop; host on
feature/co-mcu-auto-detect branch, not yet pushed).

Key wins from the corruption-debug saga (full detail below in
"UART corruption saga"):
- Per-task UartAdapter mutex closes a shared-`_doc` race that was
  silent in polling mode but produced length/body mismatches as
  soon as push mode started concurrent sends.
- `fclose(stdout)` + custom `esp_log_set_vprintf` silence
  ESP-IDF / NimBLE-host raw printf bleed onto UART0 = our
  `gogoSerial`. NimBLE init log strings were the wire garbage.
- Host-side: drain RX at boot (kills ESP32-C3 ROM bootloader
  noise), reset receiver state on implausible length (instead
  of `SKIP_PAYLOAD` for thousands of fake bytes), liveness
  watchdog in `GoGoVernier::poll()` for self-healing against
  silent peer death.

State on disk (uncommitted at cleanup time):

**Submodule `lib/GoGoVernier`** (5 files):
- `src/GoGoVernier.cpp` — Phase 4 step 1: `decodeMeasurement`
  skips `sample_ready=true` when `on_sample` is bound. Plus
  k* → SCREAMING_SNAKE rename + new constants extraction.
- `src/{GoGoVernier.h, D2PIOProtocol.h, transport/BleTransport.h,
  transport/NimBleXport.cpp}` — k* → SCREAMING_SNAKE renames
  (no logic change) + extracted MTU/scan/disconnect timing
  constants in NimBleXport.

**Parent `vernier-firmware`** (mine):
- `include/uart-adapter.h` — `_send_mutex` member + dtor.
- `src/uart-adapter.cpp` — mutex-protected sendXxx, namespace
  constants for frame layout, SendLock RAII.
- `include/vernier-adapter.h` — push queue plumbing
  (`_sample_queue`, `_push_dropped`), `waitForSample`,
  `isReady()`, queue-depth + period constants.
- `src/vernier-adapter.cpp` — ctor creates queue, connect()
  installs onSample lambda + resets push counter, disconnect()
  clears cb then resets queue, waitForSample impl.
- `src/main.cpp` — Phase 4 vernierHandler rewrite (block on
  `waitForSample`), `serial_log_vprintf` custom hook,
  `esp_log_set_vprintf` install, `fclose(stdout/stderr)`,
  magic-number cleanup constants block.

**Host `gogo-firmware`** (mine):
- `include/peripherals/gogo-vernier.h` — `_lastFrameMs` member.
- `include/utils/framed-msgpack-receiver.h` — `_len > _bufSize`
  → `reset()` (was `SKIP_PAYLOAD` for N bytes); rate-limited
  desync warn.
- `src/peripherals/gogo-vernier.cpp` — `poll()` drains 8 frames
  per call + liveness watchdog; `_handleFrame` updates
  `_lastFrameMs`; T_HELLO calls `_resetConnectionState()`;
  T_ACK match relaxed for C_DISCONNECT.
- `src/gogo-firmware.cpp` — `extSerial` RX drained at boot
  (50 ms after begin) to discard ESP32-C3 ROM bootloader noise.

Untouched in user's working tree (don't commit as part of this
session): `.gitignore`, `CLAUDE.md`, `platformio.ini`,
`include/display/gogo-display.h`, `src/display/gogo-display.cpp`.

Latest committed parent-branch HEAD before this session's batch:
`d126a45` (docs Phase 3 snapshot + race-window note). Submodule
pointer in that commit: `c85a1b7`.

Pinned platform: `pioarduino/...stable...` (NimBLE-Arduino backend, no
55.03.34 pin needed any more — the 55.03.34 workaround was for the old
ArduinoBLE stack).

Local-only: both submodule + parent are ahead of origin. Push order
matters — submodule first, then parent, otherwise origin parent
points at unreachable submodule SHA.

Not yet hardware-verified (code paths landed but not exercised in
real-world conditions):

- Mut-ex enforce in `enableSensor()` and `open()` default — needs
  GDX-3MG or GDX-ACC. GDX-LC has no conflicts so default-enable
  produced full mask (no `ch* skipped` lines).
- `MEAS_DROPPED` decode — needs sustained period overrun. GDX-LC
  at 1 Hz never drops.
- `isReady()` second-connect path — needs two host C_CONNECT
  commands after handshake completes. The boot-time race was
  absorbed by session_mutex in this trace.

To resume in the next session:

1. Read `.claude/plans/gdxlib-rewrite.md` Phase 4 section — items
   listed in execution order.
2. Phase 4 step 1 (notification-driven sample path) is the
   uncommitted change above; smoke + commit before moving on.
3. Next Phase 4 item: drop `g_ble_mutex` during the 500 ms
   async-disconnect wait in `NimBleXport::disconnect`. Pathological
   for multi-device throughput as-is — three instances each holding
   the mutex up to 500 ms during teardown serialises all of them.
4. Then: slot table in `vernier-adapter` + per-frame `dev` byte in
   the host UART protocol. Requires coordinating with the GoGo MCU
   side.
5. Don't re-derive protocol details from godirect-py; they're all
   here. Re-read this file's "Protocol" section if any wire-level
   question surfaces.

## Protocol

### Request and response frames are NOT symmetric

```
request  (host → device): [0x58][len][rcnt][cksum][cmd_id][payload...]
response (device → host): [op  ][len][rcnt][cksum][cmd_id|meas_type][payload...]
```

- byte 0 of a request is the magic `0x58`. byte 0 of a response is the
  response opcode (e.g. `0x20` for `RESPONSE_MEASUREMENT`, `0xB8` /
  `0x98` for ACK frames observed on GDX-LC).
- byte 1 = total length (inclusive of header through last payload byte).
- byte 2 = rolling counter on REQUESTS only. **The device does NOT echo
  the request's rolling counter back at byte 2.** Observed: send
  `rcnt=0xFE`, device replies with `rcnt=0x00`. Both godirect-py and
  GDXLib know this and don't validate rcnt on responses — match by
  echoed `cmd_id` at byte 4 instead.
- byte 3 = checksum (placeholder during compute, real value after).
- byte 4 = `cmd_id` on requests, echoed `cmd_id` on ACK responses, or
  the measurement-type tag on `RESPONSE_MEASUREMENT` frames.

### Checksum

Plain 8-bit sum of every frame byte EXCEPT byte 3 (the checksum slot).
NOT a 1's complement. Equivalent C:

```c
uint8_t checksum(const uint8_t* buf, uint8_t total_len) {
    int s = -(int)buf[3];
    for (uint8_t i = 0; i < total_len; ++i) s += buf[i];
    return (uint8_t)(s & 0xFF);
}
```

The `-buf[3]` cancellation makes it safe to call before OR after the
placeholder slot is populated.

### CMD_INIT bytes

Fixed 25-byte literal frame from `godirect-py/godirect/device.py:_GDX_init`:

```
0x58, 0x00, 0x00, 0x00, 0x1A,
0xa5, 0x4a, 0x06, 0x49,
0x07, 0x48, 0x08, 0x47,
0x09, 0x46, 0x0a, 0x45,
0x0b, 0x44, 0x0c, 0x43,
0x0d, 0x42, 0x0e, 0x41
```

Bytes 1, 2, 3 are filled in at send time (length, rcnt, checksum).
Bytes 4 (`CMD_INIT = 0x1A`) and 5..24 (the 20-byte payload) are the
literal handshake nonce. Don't try to reverse-engineer the meaning;
godirect-py treats it as opaque too.

### CMD_GET_DEVICE_INFO is slow

On GDX-LC, this single command takes 4–7 s to ack — well beyond the
3 s default we use for everything else. Bump its specific timeout to
`8000 ms`. The other queries (INIT, AVAILABLE_MASK, SENSOR_INFO,
SET_PERIOD, START_MEASUREMENTS) all reply within ~100 ms.

## BLE GATT specifics on GDX-LC (and likely all GDX devices)

### Characteristic properties

```
service:  d91714ef-28b9-4f91-ba16-f0d9a604f112
cmd_char: f4bf14a6-c7d5-4b6d-8aa8-df1a7c83adcb  WRITE_NO_RSP only
                                                (canWrite()=0, canWriteNoResponse()=1)
rsp_char: b41e6675-a329-40e0-aa01-44d2f444babe  NOTIFY only
                                                (canNotify()=1, canIndicate()=0)
```

The cmd char does NOT advertise WRITE-with-response. Calling
`writeValue(..., response=true)` on h2zero/NimBLE-Arduino returns
`false` immediately. Use `response=false`.

### MTU is mandatory before CMD_INIT

CMD_INIT is 25 bytes. Default ATT_MTU is 23, max single-write payload
is `MTU - 3 = 20` bytes. h2zero's `NimBLERemoteValueAttribute::writeValue`
falls through to a long-write path when `length > mtu - 3` AND
`response = false`, but long-write requires write-with-response which
the cmd char doesn't support, so it returns `BLE_HS_ATT_ERR_ATTR_NOT_LONG`
and silently truncates the write to `mtu - 3 = 20` bytes. The
peripheral receives an over-short, invalid frame and drops it. We see
this as a CMD_INIT timeout.

Solution:
1. `NimBLEDevice::setMTU(247)` — but call AFTER `NimBLEDevice::init`.
   `ble_att_set_preferred_mtu` requires the host stack registered;
   before init it silently fails.
2. Pass `exchangeMTU=true` to `NimBLEClient::connect` so h2zero kicks
   off the GATT MTU exchange right after BLE link-up.
3. Poll `client->getMTU()` post-connect for up to 2 s — h2zero's
   exchange is async, returns before completion. If it stays at 23,
   fire a fallback `client->exchangeMTU()` and poll again.
4. Verify final value is ≥ 28 before any handshake write.

The earlier worry that `exchangeMTU=true` would race with the peer's
own MTU REQUEST and bail on `BLE_HS_EALREADY` was an arduino-esp32
**bundled BLE library** bug, not a NimBLE host bug. h2zero's
`exchangeMTU()` explicitly treats EALREADY as success.

### Concurrent connect / startReading race

The firmware has two tasks that can both initiate a vernier connect:
- main task `loop()` running `autoConnectDevice` after boot
- `uartHandler` task processing a host-MCU `C_CONNECT` command

When the host MCU also fires `C_SET_PERIOD` then `C_CONNECT` close
together, `connectAndReport` runs concurrently with the auto-connect's
in-flight `_gv.open()` handshake. The idempotent-connect path in
`VernierAdapter::connect` returns success as soon as `isConnected()`
goes true, but `isConnected` is set BEFORE the D2PIO handshake
completes. Result: a concurrent `startReading()` ran with `available==0`
mask, sent CMD_START_MEASUREMENTS with mask=0, and timed out.

Fix lives in two places:
- `GoGoVernier::start` refuses if `_impl->available == 0` (handshake
  incomplete).
- `sendRequest` stamps `pending_rcnt`/`pending_cmd` INSIDE the request
  mutex (not in `encode()`) so two tasks racing into encode + send
  can't clobber each other's pending state.

A future Phase 4 refactor should add a single `_ready` flag set at the
end of `open()` success, and have the adapter's idempotent path use
`isReady()` instead of `isConnected()`.

## Logging on ESP32-C3 with USB-CDC

### Serial baud is a no-op on USB-CDC

`Serial.begin(baud)` and `monitor_speed` in platformio.ini both
**ignore** the baud value. ESP32-C3 with `ARDUINO_USB_CDC_ON_BOOT=1`
exposes Serial as USB-CDC (HWCDC for the built-in USB-Serial-JTAG, or
USBCDC for TinyUSB). Both `HWCDC::begin(baud)` and `USBCDC::begin(baud)`
declare the parameter but never read it (verified in arduino-esp32
`cores/esp32/USBCDC.cpp` and `HWCDC.cpp`). USB-FS bulk transfer
governs throughput. The "baudrate" exists only because the CDC class
spec includes a LineCoding descriptor for legacy compatibility.

### What actually controls CDC throughput

- `Serial.setTxBufferSize(N)` BEFORE `Serial.begin()`. Default is 256
  bytes which overruns during the BLE init storm and causes mid-line
  truncation. 4096 bytes is a comfortable working size.
- Less log volume in hot paths (drop debug spam, keep info / error).
- `vTaskDelay(1)` after dense bursts lets the host drain.
- Move logs off USB-CDC (use a real UART on free pins) for
  guaranteed throughput control.

### `USE_ESP_IDF_LOG` flag matters for tag filtering

Without `-D USE_ESP_IDF_LOG`, arduino-esp32's `log_*()` macros expand
to plain `log_printf(...)` which writes directly to Serial without
going through `esp_log_write`. `esp_log_level_set(...)` is then a
no-op. With `USE_ESP_IDF_LOG` on, log_*() expand to
`ESP_LOG_LEVEL_LOCAL` which respects per-tag level. Default tag is
`ARDUHAL_ESP_LOG_TAG` = `"ARDUINO"`.

When the project enables `USE_ESP_IDF_LOG`, `initBleOnce()` must call:

```cpp
esp_log_level_set("*", ESP_LOG_WARN);
esp_log_level_set("ARDUINO", ESP_LOG_DEBUG);  // our log_i / log_d
esp_log_level_set("NimBLEDevice", ESP_LOG_INFO);
esp_log_level_set("NimBLEClient", ESP_LOG_INFO);
esp_log_level_set("NimBLEScan",   ESP_LOG_INFO);
```

…or our own `log_i` / `log_d` calls vanish (default tag level on
arduino-esp32 is WARN).

### ANSI color codes interleave badly

`-D CONFIG_ARDUHAL_LOG_COLORS` and `-D CONFIG_LOG_COLORS` emit ANSI
color escape sequences in every log line. With multiple tasks writing
to Serial concurrently, these escapes get split mid-sequence and
produce garbage in the host monitor. Prefer leaving them off unless
you have a colorising terminal AND the log volume is light enough
that interleaving doesn't matter.

## Build / platform

### Pin to a tested platform release

`pioarduino/platform-espressif32` ships precompiled BT controller libs
(`libbtdm_app.a`, `libbtbb.a`) bundled with each release. Library API
revisions can break the host-side stack between minor releases (saw
this with arduino-esp32-libs 5.5.0 → 5.5.4 — `r_lld_env_init`
NULL-fn-ptr crash inside `esp_bt_controller_init`).

Working pin during this saga: `55.03.34` (Arduino 3.3.4, IDF 5.5.1).
Later releases work too once we moved off the `MomePP/ArduinoBLE` fork
to `h2zero/NimBLE-Arduino`. The lesson: when a "stable" release
suddenly breaks BLE init, suspect a libs version drift before
suspecting your own code.

### h2zero/NimBLE-Arduino on arduino-esp32 3.3.x

h2zero compiles against the bundled NimBLE host (no vendored stack on
this platform) — same blob version as the BT controller, no
version-skew failure mode. That's why we picked it over the
arduino-esp32 bundled `BLE` library: the bundled BLE auto-MTU bug
(unconditional `ble_gattc_exchange_mtu` after connect, with no
EALREADY tolerance) was the GDX-LC connect-failure root cause, and
h2zero exposes `exchangeMTU` as an opt-in flag that handles EALREADY
gracefully.

## UART corruption saga (Phase-3.5, this codebase ↔ host MCU wire)

After Phase-3 push-mode landed, the GoGo host display refused to
flip to "connected" even though vernier was streaming samples
correctly. Took several rounds to peel back; recording the layers
so future debug doesn't repeat the dance.

### Layer 1 — UartAdapter `_doc` race

Vernier's `UartAdapter` had ONE `JsonDocument _doc` member. Three
FreeRTOS tasks called its public `sendXxx()` methods without
synchronisation: `uartHandler` (sendAck on host commands), main
`loop()` task (`connectAndReport`'s 5-frame burst on connect),
`vernierHandler` task (sendSensorValuesTs on every BLE notify).

Race window: between `measureMsgPack(_doc)` and
`serializeMsgPack(_doc, _out)` inside `send()`, another task could
mutate `_doc`. Field-test logs showed:
```
TX t=4 seq=6 len=123 (hdr=2 body=149)   ← _doc clobbered
TX t=5 seq=7 len=149 (hdr=2 body=18)
```
Wire bytes had a 2-byte length-prefix saying N, then a payload of
M ≠ N. Host parser read N bytes, got misaligned, treated next
frames as garbage.

Fix: per-instance `SemaphoreHandle_t _send_mutex` (created in
ctor). Every public `sendXxx()` takes it for the entire
build+measure+serialize critical section. Exposed via a stack-
allocated `SendLock` RAII guard.

### Layer 2 — ESP-IDF stdio bleed onto UART0

After fixing the race, host STILL got nothing past T_ACK during
NimBLE init. Captured raw bytes on host RX showed they were
**Arduino HAL log strings** ("D NimBLEDevice: Starting NimBLE-
Arduino 2.5.0", "(3624) ARDUINO:", etc.) — vernier's own debug log
output appearing on the protocol wire.

Cause: `ARDUINO_USB_CDC_ON_BOOT=1` + `Serial.setDebugOutput(true)`
only redirects `esp_log` (path 1). It does NOT touch stdio. The
bundled IDF NimBLE host stack (separate from h2zero's Arduino
wrapper) uses `MODLOG_DFLT` which expands to raw `printf`. `printf`
writes to `stdout` whose default fd is UART0 = same hardware as
our `gogoSerial`.

Two log paths confirmed:
1. arduino-esp32 `log_*()` with `USE_ESP_IDF_LOG` → `esp_log` →
   `setDebugOutput`-installed vprintf → USB-CDC ✓
2. raw `printf` / `puts` / `fwrite` from IDF / NimBLE-host →
   stdio → UART0 ✗

Fix: `fclose(stdout); fclose(stderr);` early in `setup()` to kill
path 2. `esp_log_set_vprintf(serial_log_vprintf)` installed
explicitly to guarantee path 1 keeps writing via `Serial.write()`
directly (bypassing stdio entirely) so closing stdout doesn't
break visible logs.

### Layer 3 — host-side wire-noise handling

Even with vernier writing only valid frames, the ESP32-C3 ROM
bootloader writes diagnostic strings to UART0 at the very start
of every boot, BEFORE our app's `setup()` runs and BEFORE we
close stdout. Those bytes pre-fill the host's `extSerial` RX
buffer. `FramedMsgPackReceiver` reads two bytes, gets bogus
length like `0x4420 = 17440`, enters `SKIP_PAYLOAD` to drain
17440 bytes that don't actually exist — every legitimate frame
arriving in the next ~3 minutes (at 100 B/s) gets eaten in the
skip and lost.

Two host-side fixes:
1. After `extSerial.begin()`, `delay(50)` then drain RX buffer.
   Discards the boot noise window. Logs `Vernier: drained N
   boot-noise bytes` if any.
2. In `FramedMsgPackReceiver::processStep` READ_LEN1, when
   `_len > _bufSize`: `reset()` to READ_LEN0 instead of entering
   `SKIP_PAYLOAD`. Resyncs in 1 byte instead of N. Throttled
   warning logs every 500 ms with a reset-count.

### Layer 4 — host self-heal against silent peer death

Vernier reset (reflash, brownout, crash) leaves host's
`_connState` stuck at `CONN_CONNECTED`. The disconnect button
sends C_DISCONNECT but if the bytes/T_ACK don't make it through
the wire-state confusion of vernier reboot, host UI stays stuck
forever waiting for an ACK that never comes. Three host fixes:

1. **Liveness watchdog** in `GoGoVernier::poll()`: if
   `_connState == CONN_CONNECTED` and `millis() - _lastFrameMs >
   max(period × 3, 5s, capped at 30s)`, force
   `_resetConnectionState()`. Display flips to "Disconnected"
   automatically without user action.
2. **T_HELLO state reset**: `T_HELLO` handler now calls
   `_resetConnectionState()` (was: only `_resetSeqStats`). Peer
   reboot announces itself; host re-enters IDLE. The handshake's
   immediately-following `T_STATUS=true` re-sets to CONNECTED for
   normal connect flow. Brief IDLE→CONNECTED flicker is fine.
3. **T_ACK match relaxed** for C_DISCONNECT: was `_lastAckReq ==
   _lastCmdSeqSent && _lastCmdCodeSent == C_DISCONNECT && ok`.
   Now `_lastCmdCodeSent == C_DISCONNECT && ok` only. Late ACKs
   after multi-press now flip state. Safe because host UI
   serialises connect/disconnect — only one outstanding command
   at a time.

### Lessons

- **Stdio is not idle**. Any C library on ESP-IDF can `printf` and
  hit UART0. If UART0 is not free for the application, close
  stdio at startup.
- **Shared mutable state across tasks needs a mutex**. The
  UartAdapter race had been silent for the entire pre-Phase-3
  history because polling-mode timing made overlap rare. Push-
  mode changed the timing and the bug surfaced immediately.
- **Don't trust a length-prefix on a noisy wire without sync
  bytes**. Either add a sync preamble (heavy protocol change) or
  recover fast on implausible lengths (light: reset state, retry
  next byte).
- **Build defensive timeouts at every layer that depends on a
  remote signal**. Anything that can stall the peer can stall
  the host, and the host should self-heal.

## Known race windows (intentionally not fixed)

These came up during the Phase-3 review. Not introduced by Phase 3 —
inherited from the Phase-1 single-slot polling design — and the
real-world impact is small enough at current sample rates that
fixing them isn't worth the lock contention on the notify task.
Document so a future debugger doesn't chase them as "new bugs":

- **Cross-channel snapshot tear in `copySample()`.** Notify task
  writes `channels[i].value` for each enabled channel, then sets
  `sample_ready=true`. If notify N+1 fires while the consumer is
  mid-`copySample`, the consumer sees a mix of frame N and frame
  N+1 values. RV32 atomic 32-bit loads guarantee no torn floats
  per channel; only the cross-channel snapshot is unsynchronised.
  The `sample_ready` flag itself doesn't help — the race is
  between "ready=true was set" and the iteration over channels.
  At 1 Hz the window is microseconds and indistinguishable from
  the device's own intra-frame jitter; revisit if Phase-4
  multi-device aggregate sample rates close the gap.

- **`enabled` mask read during `decodeMeasurement`.** When the
  push-mode `onSample` callback fires, `enabled` is read to build
  the dense `Sample` layout. `enableSensor()` /
  `disableSensor()` from a caller task can write it concurrently.
  The contract is that callers don't toggle channels during
  streaming (only between `stop()` and `start()`), but it isn't
  enforced. Add an assertion if a future bug points here.

- **`_impl->dropped` increment.** Single-writer (notify task) +
  aligned u32 read from caller via `droppedSamples()`. Not
  actually a race on RV32 — atomic load + atomic increment from
  the only writer. Reviewer flagged it as one; documented here
  so the same flag doesn't reappear next review.

## Multi-device readiness (Phase 4 prep)

Done:
- ✅ `g_active_impl` removed. `NimBleXport::subscribe` passes
  h2zero a `std::function` lambda capturing `Impl*` directly.
  Per-instance routing — two GoGoVernier instances no longer
  steal each other's notifications. Lambda indirects through
  `Impl::on_notify` so `unsubscribe()` nulling it makes the
  callback no-op cleanly during teardown.
- ✅ dtor UAF surface reduced. `NimBLEDevice::deleteClient` inside
  `disconnect()` clears the subscription before `~NimBleXport`
  deletes `_impl`. A late notify dispatched from the host task
  hits a nulled `on_notify` and returns.

Still single-instance because of:
- `disconnect()` holds `g_ble_mutex` for up to 500 ms during the
  async-disconnect wait. Multi-device throughput would suffer;
  drop the mutex during the wait and retake before `deleteClient`.
- `vernier-adapter` is a single global, not a slot table. Needs
  `VernierAdapter[CONFIG_NIMBLE_MAX_CONNECTIONS]` keyed by a
  device id.
- Host UART protocol (`include/uart-adapter.h`) has no per-device
  field. Frames need a `dev` (u8) byte on `T_DEVINFO` /
  `T_DEVSTATS` / `T_FIELDS` / `T_SENS_VALUES`. Add `T_DEV_LIST`
  for the host to enumerate occupied slots. `T_HELLO` stays
  device-agnostic.

## Reference implementations cross-checked

| | godirect-py | GDXLib (MomePP fork) | GoGoVernier (this repo) |
|---|---|---|---|
| Frame layout | spec-of-record | identical | matches |
| Checksum | spec-of-record | identical | matches |
| CMD_INIT bytes | spec-of-record | identical | matches |
| Rolling counter | pre-decrement, first=0xFE | post-decrement, first=0xFF | pre-decrement (godirect-py style) |
| Subscribe | Bleak `start_notify` | `BLECharacteristic::subscribe()` | `NimBLERemoteCharacteristic::subscribe(true, cb)` |
| Write mode | Bleak default (with-response) | ArduinoBLE auto-detect | `canWrite()` ? with-response : without |
| MTU | Bleak ~244 default | ArduinoBLE auto on connect | h2zero `setMTU(247) + exchangeMTU` + 2 s settle poll |
| Response validation | byte 4 echoed cmd_id only | length-only | byte 4 echoed cmd_id only (rcnt NOT validated) |

When in doubt about wire behaviour, read `godirect-py/godirect/device.py`
first. It's the authoritative reference and the most recent of the
three.
