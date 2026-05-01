# D2PIO BLE bring-up — hard-won findings

Captured during the Phase 1 / Phase 2 GoGoVernier debug saga. Each entry
is a non-obvious fact that cost real time to discover. Future maintainers
should read this before tweaking BLE / log / Serial config.

## Session-end snapshot — pick up here

Last session ended with **Phase 2 fully working end-to-end on real
GDX-LC**: clean handshake (INIT → DEVICE_INFO → AVAILABLE_MASK → 5×
SENSOR_INFO → SET_PERIOD → START_MEASUREMENTS), all five channels
named (Light, UV, 615/525/465 nm), live samples flowing at 1 Hz, no
duplicate writes, no failed commands.

State on disk (latest commits):

- Submodule `lib/GoGoVernier` HEAD: `afcb584` — session_mutex serialises
  open/close/start/stop; pending_cmd stamped under req_mutex; cmd_id-only
  ACK matching; `start()` refuses pre-handshake or no-ops if already
  streaming.
- Parent `vernier-firmware` branch `develop` HEAD: `ceac306` — submodule
  bump + Serial TX buffer 4 KB + monitor_speed=115200 + USE_ESP_IDF_LOG
  on + ANSI colors off + `.claude/knowledges/d2pio-debug-findings.md`
  (this file) + plan in `.claude/plans/gdxlib-rewrite.md`.

Pinned platform: `pioarduino/...stable...` (NimBLE-Arduino backend, no
55.03.34 pin needed any more — the 55.03.34 workaround was for the old
ArduinoBLE stack).

To resume in the next session:

1. Read `.claude/plans/gdxlib-rewrite.md` Phase 3 / Phase 4 sections —
   they list the deferred follow-ups in order.
2. Highest-leverage next item is the `_ready` flag in
   `lib/GoGoVernier/src/GoGoVernier.cpp` so `VernierAdapter::connect`
   can switch its idempotent guard from `isConnected()` to `isReady()`.
   Today the session_mutex absorbs the bug; making the contract explicit
   is cheap and removes a foot-gun.
3. Phase 4 multi-device requires replacing `g_active_impl` in
   `lib/GoGoVernier/src/transport/NimBleXport.cpp` with a per-instance
   subscribe lambda (the global is the only thing blocking real
   multi-conn). See "Multi-device readiness (Phase 4 prep)" section
   below.
4. Don't re-derive any of the protocol details from godirect-py;
   they're all here. Re-read this file's "Protocol" section if any
   wire-level question surfaces.

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

## Multi-device readiness (Phase 4 prep)

Currently single-instance only:
- `g_active_impl` global pointer in `NimBleXport.cpp` is a single-conn
  routing slot for the notify trampoline. Multi-device requires a
  `conn_handle → Impl*` map.
- `~NimBleXport` deletes `_impl` while the trampoline may still hold
  a pointer — UAF risk if a notify is in flight at destruction. Fix
  in Phase 4 by switching to per-instance subscribe lambdas instead
  of the global trampoline.
- `disconnect()` holds `g_ble_mutex` for up to 500 ms during the
  async-disconnect wait. Multi-device throughput suffers; drop the
  mutex during the wait and retake before `deleteClient`.

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
