# D2PIO — Vernier Go Direct over BLE

This is a working spec for the over-the-air protocol used by Vernier
Go Direct sensors. Vernier has not published a formal protocol document.
The fields below are extracted from:

- `VernierST/godirect-py` (BSD-3, © 2024 Vernier Science Education) — the
  official Python implementation. Authoritative for opcodes, frame layout
  and per-channel record shape.
- `Vernier-Science-Education/GDXLib` (BSD-3, © 2024 Vernier Science Education),
  via the `MomePP/GDXLib` fork — used for empirically observed BLE behaviour
  on ESP32 (MTU, chunking, characteristic discovery order).

If the two sources disagree, godirect-py wins.

## BLE GATT layout

The device exposes a single primary service plus a battery service.
Application traffic uses one writable characteristic and one notify
characteristic on the GDX service:

| Role | UUID | Properties |
|---|---|---|
| GDX service | `d91714ef-28b9-4f91-ba16-f0d9a604f112` | primary |
| Command (host → device) | `f4bf14a6-c7d5-4b6d-8aa8-df1a7c83adcb` | write |
| Response (device → host) | `b41e6675-a329-40e0-aa01-44d2f444babe` | notify |

Connect → enable notifications on the response char → write a command frame
on the command char → read response frames from the notify queue.

## Frame layout

Every command and every response is a single byte stream of the same shape.
**Asymmetry note:** byte 0 is the literal magic `0x58` on the request path
(host → device) and the **response op** (e.g. `0x20` for measurement frames)
on the reply path. The other header bytes share offsets between request and
response.

```
request (host → device):
+--------+----------+-------+----------+--------+----------+
| 0x58   | length   | rcnt  | checksum | cmd_id | payload  |
+--------+----------+-------+----------+--------+----------+
  byte 0    byte 1   byte 2   byte 3    byte 4   bytes 5..N-1

response (device → host):
+--------+----------+-------+----------+----------------+----------+
|  op    | length   | rcnt  | checksum | cmd_id|meas_ty | payload  |
+--------+----------+-------+----------+----------------+----------+
  byte 0    byte 1   byte 2   byte 3        byte 4       bytes 5..N-1
```

| Byte | Field | Notes |
|---|---|---|
| 0 | header / response op | requests: constant `0x58`. Responses: `0x20` for `RESPONSE_MEASUREMENT`, otherwise tied to the command being acknowledged. |
| 1 | length | total bytes in this frame, header through last payload byte, inclusive |
| 2 | rolling counter | starts at `0xFF`, decrements on every host write, wraps to `0xFF` after `0x00`. Device echoes the same value in the corresponding response. |
| 3 | checksum | see below |
| 4 | command id (request) / sub-type (response) | request: one of the `CMD_*` opcodes. Response: the original `cmd_id` echoed back, or for `RESPONSE_MEASUREMENT` the measurement-type tag (`MEAS_*`). |
| 5..N-1 | payload | command-specific |

A response can span multiple BLE notification packets if `length` exceeds
the negotiated MTU minus the ATT header. Re-assemble by counting bytes
until `length` is satisfied. godirect-py reference: `Device._GDX_read_blocking`.

### Checksum

Plain 8-bit sum of every frame byte EXCEPT the checksum byte itself
(byte 3). Not a 1's complement — earlier drafts of this doc were wrong
about that.

Reference: `Device._GDX_calculate_checksum` in godirect-py:

```python
def _GDX_calculate_checksum(self, buff):
    length = int(buff[1])
    checksum = -1 * int(buff[3])     # cancels the placeholder
    for i in range(0, length):
        checksum += int(buff[i])
        checksum = checksum & 0xFF
    return checksum
```

Equivalent C:

```c
uint8_t checksum(const uint8_t* buf, uint8_t total_len) {
    int s = -(int)buf[3];
    for (uint8_t i = 0; i < total_len; ++i) s += buf[i];
    return (uint8_t)(s & 0xFF);
}
```

The `-buf[3]` cancellation makes the function safe to call before OR
after the placeholder slot is populated. GDXLib's
`D2PIO_CalculateChecksum` is identical modulo style.

## Command opcodes

From `godirect-py/godirect/device.py`:

| Name | Op | Purpose |
|---|---|---|
| `CMD_START_MEASUREMENTS` | `0x18` | begin streaming. Payload = u32 sensor mask. |
| `CMD_STOP_MEASUREMENTS` | `0x19` | stop streaming. Payload empty. |
| `CMD_INIT` | `0x1A` | initial handshake; resets rolling counter. |
| `CMD_SET_MEASUREMENT_PERIOD` | `0x1B` | set sample period. Payload = u32 period in **microseconds**, little-endian. |
| `CMD_GET_SENSOR_INFO` | `0x50` | per-channel descriptor. Payload = u8 sensor number. |
| `CMD_GET_SENSOR_AVAILABLE_MASK` | `0x51` | u32 mask of channels the device exposes. |
| `CMD_DISCONNECT` | `0x54` | clean shutdown of the link. |
| `CMD_GET_DEVICE_INFO` | `0x55` | name, order code, serial, FW versions, etc. |
| `CMD_GET_DEFAULT_SENSORS_MASK` | `0x56` | u32 mask of channels the device enables when the user picks "default". |

## Sensor info record (`CMD_GET_SENSOR_INFO` response)

Fields from `godirect-py/godirect/sensor.py:Sensor`. Lengths and byte
ordering inside the response are derived empirically from `_GDX_get_sensor_info`;
when in doubt, mirror that decoder byte-for-byte.

| Field | Type | Notes |
|---|---|---|
| `sensor_number` | u8 | 0..31 |
| `sensor_id` | u32 | Vernier catalogue id |
| `numeric_measurement_type` | u8 | one of the `MEAS_*` tags below |
| `sampling_mode` | u8 | periodic vs aperiodic |
| `sensor_description` | utf-8 string, padded | "Force (N)" etc. |
| `sensor_units` | utf-8 string, padded | "N", "lux", etc. |
| `measurement_uncertainty` | f32 | |
| `min_measurement` | f32 | hardware range floor |
| `max_measurement` | f32 | hardware range ceil |
| `typ_measurement_period` | u32 µs | recommended period |
| `min_measurement_period` | u32 µs | fastest period |
| `max_measurement_period` | u32 µs | slowest period |
| `measurement_period_granularity` | u32 µs | period must be a multiple |
| `mutual_exclusion_mask` | u32 | bits set = sensors this one excludes |

`mutual_exclusion_mask` is the single most-overlooked field of the protocol.
GDX-3MG, GDX-ACC and others have low/high-range channel pairs that can not
be enabled simultaneously. A correct implementation refuses to set a bit in
`CMD_START_MEASUREMENTS`'s mask if it conflicts with another set bit
according to any enabled channel's `mutual_exclusion_mask`.

## Live measurement frames

Once `CMD_START_MEASUREMENTS` has been ACKed, the device emits frames with
op = `RESPONSE_MEASUREMENT` (`0x20`) at the requested cadence. Each frame's
payload is a sequence of TLV-style sub-records. Tag values:

| Tag | Name | Decode |
|---|---|---|
| `0x06` | `MEAS_NORMAL_REAL32` | u8 sensor#, f32 value |
| `0x07` | `MEAS_WIDE_REAL32` | u8 sensor#, u32 timestamp µs, f32 value |
| `0x08` | `MEAS_SINGLE_CHANNEL_REAL32` | f32 value (channel implied) |
| `0x09` | `MEAS_SINGLE_CHANNEL_INT32` | i32 value (channel implied) |
| `0x0a` | `MEAS_APERIODIC_REAL32` | u8 sensor#, u32 ts µs, f32 value |
| `0x0b` | `MEAS_APERIODIC_INT32` | u8 sensor#, u32 ts µs, i32 value |
| `0x0c` | `MEAS_START_TIME` | u32 epoch ms — start-of-stream marker |
| `0x0d` | `MEAS_DROPPED` | u32 count — samples dropped by the device |
| `0x0e` | `MEAS_PERIOD` | u32 actual period µs — may differ from requested |

Reference decoder: `Device._GDX_handle_measurement` in godirect-py.

## Charger states

Returned in the device-info / status response.

| Value | Meaning |
|---|---|
| 0 | idle |
| 1 | charging |
| 2 | complete |
| 3 | error |

## Open items

- Exact byte offsets within the `CMD_GET_SENSOR_INFO` response — derive
  from `_GDX_get_sensor_info` in godirect-py. Document here once ported.
- Exact byte layout of `CMD_GET_DEVICE_INFO` response — same.
- MTU negotiation: godirect-py relies on the OS BLE stack to set a useful
  MTU; ArduinoBLE / NimBLE on ESP32 default to 23, which forces multi-frame
  reassembly on every long response. Investigate raising it to 247.
