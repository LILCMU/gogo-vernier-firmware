# Multi-device support — design decisions (Phase 4 step 2)

Pre-work for Phase 4 multi-device. Captures decisions BEFORE coding so
the wire protocol bump and the slot-pool refactor land coherently
across both vernier-firmware and gogo-firmware.

## Goal

Support **N concurrent Vernier sensors** on a single ESP32-C3 vernier
MCU. N bounded by `CONFIG_NIMBLE_MAX_CONNECTIONS` (default 3 on
arduino-esp32 3.3.x — the bundled NimBLE host is built with that
cap). User-visible: host can connect to multiple GDX devices at the
same time and read live values from each, distinguished by slot id.

## Hard constraints

- NimBLE max 3 concurrent connections (compile-time on bundled stack).
- ESP32-C3 single-core, ~280 KB DRAM. Per-instance state cost matters.
- UART0 wire to host is shared. Frames from multiple slots interleave.
- Host display has limited screen real estate.

---

## Decisions to ratify

Each item: my proposed default + reasoning. Mark "ACCEPT" or override.

### D1. Slot capacity (compile-time max)

**PROPOSED**: `VERNIER_MAX_SLOTS = 3`. Matches NimBLE default. Static
array `VernierAdapter slots[VERNIER_MAX_SLOTS]`.

Rationale: NimBLE controller blob caps at 3. Asking for more would
need a custom IDF rebuild. Static array > dynamic map: predictable
memory, no allocator on a tight RAM budget.

**OPEN**: confirm `3` is enough for your use case, or do you need
more (would require sdkconfig override + larger flash)?

### D2. Slot id allocation

**PROPOSED**: vernier-side first-free assignment. When host sends
`C_CONNECT`, vernier scans free slots, picks lowest unoccupied,
returns assigned `dev` in `T_ACK`. Host treats `dev` as opaque slot
id — never assumes 0..N-1 ordering, never specifies dev on connect.

Rationale: simpler protocol. Host doesn't track slot occupancy
separately from vernier's actual state. Single source of truth.

**Alternative considered**: host-assigned slot id (host says "use
slot 2"). Rejected — splits state across two MCUs, requires sync.

### D3. Wire protocol v2 — `dev` field semantics

**PROPOSED**: add `dev` (u8) to:
- `T_DEVINFO` — which slot's device info
- `T_DEVSTATS` — which slot's battery/rssi/dropped
- `T_FIELDS` — which slot's channel definitions
- `T_SENS_VALUES` — which slot's sample frame
- `T_ACK` — which slot the ack relates to (only for slot-scoped commands; absent for global)

Inbound commands gain `dev` field where slot-specific:
- `C_CONNECT` — NO dev field (vernier picks). May carry optional `name` (target advertised name; absent = "proximity" first-found).
- `C_DISCONNECT` — `dev` field (which slot to disconnect). Required.
- `C_SET_PERIOD` — `dev` field (which slot's rate). Required. See D5 for shared-vs-per-slot debate.

Wire format: each `dev` is u8 in MsgPack; absent = treat as `0` (back-compat with v1 firmware that doesn't send the field).

**Bump**: `VERNIER_PROTOCOL_VERSION = 2`. Host warns on mismatch but
keeps parsing — both sides default `dev=0` when field absent, so v1
talking to v2 (or v2 talking to v1) "works" for slot 0 only.

### D4. New message type — `T_DEV_LIST`

**PROPOSED**: `T_DEV_LIST = 9`. Pushed by vernier on:
- After connect handshake completes for any slot.
- After disconnect for any slot.
- On boot, after auto-connect attempts settle.
- On request: new command `C_DEV_LIST = 4` from host.

Payload:
```
{
  "t": 9, "seq": N,
  "slots": [
    {"dev": 0, "name": "GDX-LC 091001F5", "order": "GDX-LC", "connected": true},
    {"dev": 1, "name": "GDX-TMP 0123ABCD", "order": "GDX-TMP", "connected": true},
    {"dev": 2, "name": "", "order": "", "connected": false}
  ]
}
```

Always sends ALL slots (occupied + free), so host can render full
slot table without guessing capacity.

### D5. Sampling period — per-slot or shared?

**ACCEPTED: Option A (per-slot).** Each slot has its own sampling
rate. `C_SET_PERIOD` carries `dev` field; vernier applies period to
that slot only. Host UI exposes period control on the per-slot
drill-down screen (per D8 C). Fits multi-sensor workflows where
slots probe unrelated phenomena at different cadences (e.g. GDX-ACC
@ 100 Hz + GDX-TMP @ 1 Hz on the same MCU).

Implementation in vernier:
- `VernierAdapter::_period_ms` becomes per-instance (already is).
- C_SET_PERIOD handler: `vernier[dev].setSamplingRate(period_ms)`.
- Each slot's vernierHandler iteration uses its own `samplingPeriod()`.

Implementation in host:
- `_devices[dev].period_ms` tracked separately.
- DEVSTATS for slot N reflects slot N's period.
- Period UI control is per-slot.

### D6. NVS persistence

**PROPOSED**: store list of saved device names, indexed by slot.
- Key `"deviceName0"`, `"deviceName1"`, `"deviceName2"` in
  namespace `"vennierSetting"` (typo intentional — preserves
  upgrade path from v1 NVS schema).
- Default value `"proximity"` for unset slots.
- v1's single key `"deviceName"` migrated automatically to
  `"deviceName0"` on first v2 boot.

**Auto-connect on boot**: vernier scans NVS, attempts auto-connect for
each non-default slot in order. Each handshake is sequential
(scan-then-connect occupies BLE controller); could parallelise if
needed but sequential is simpler for v2.

### D7. Vernier task topology

**PROPOSED**: keep `vernierHandler` as ONE task. Inside, iterate
slots round-robin: for each occupied slot, `waitForSample(slot,
buf, count, short_timeout)`. Multiplex onto single UART send.

- Pro: single task = single mutex contention point on UartAdapter.
  Simpler.
- Con: total throughput capped by single-task scheduling. At 3 slots
  × 100 Hz = 300 frames/sec, each frame ~50 B, that's 15 KB/sec.
  Well under 115200 baud (11.5 KB/s)... wait, that's OVER. Need to
  re-examine if 100 Hz × 3 is achievable.

**Sanity check**: Vernier's typical period floor is 50 ms (20 Hz).
3 × 20 = 60 frames/sec × 50 B = 3 KB/sec. Comfortable. 100 Hz isn't
actually a typical use case — Vernier sensors max around 1 kHz on
specific high-rate channels but most are 1–50 Hz. So single-task
multiplex is fine in practice.

**Alternative**: per-slot `vernierHandler` task. Each blocks on its
own queue. UartAdapter's mutex serialises sends. Cost: N × stack
(~8 KB each = 24 KB for N=3). Defer unless throughput shows it's
needed.

### D8. Host UI

**ACCEPTED: Option C (slot list + drill-down).**

Main vernier screen ("slot list view"):
- Vertical list of N rows (one per slot, including free slots).
- Each row shows: slot id, device name (or "Empty"), connection
  state badge, and the first sensor value + unit (e.g. "Light: 56.2 lux").
- Cursor / selector indicates the current row. Up/down navigates.
- "Press to select" enters the drill-down view for the highlighted slot.

Drill-down view ("slot detail"):
- All fields of the selected slot rendered in the existing
  single-slot layout.
- Period setting control (per D5: per-slot).
- Disconnect button (operates on this slot only).
- Back button returns to slot list view.

Empty-slot row in list view: "Empty — press to connect". Selecting
issues C_CONNECT (vernier picks first-free; user can't pre-pick
slot id per D2).

Implementation cost: bigger than Option A, but matches multi-slot
mental model. Drill-down reuses most of today's single-slot draw
code with `_devices[selected_slot]` instead of bare scalars.

This is the biggest individual host-side work item in Phase 4 step
4. Will likely scope it as its own sub-step:
- Step 4a: backend slot table + per-slot dispatch in `_handleFrame`
  (no UI change yet, primary-slot-0 default behaviour).
- Step 4b: slot list view + drill-down navigation in display layer.

### D9. Connect button behaviour

**PROPOSED**: when host's "Connect nearby device" button pressed:
- If any slot is free → vernier scans + assigns to first free slot.
- If all slots occupied → vernier returns `T_ACK` with `ok=false`,
  msg=`"all slots full"`.

Disconnect button (per D8 Option A primary slot): disconnects the
currently-displayed slot. UI cycles to next occupied slot or shows
"Disconnected" if none remain.

### D10. Liveness / per-slot self-healing

**PROPOSED**: extend host's liveness watchdog to per-slot. Each
`_devices[i]` tracks its own `_lastFrameMs`. Liveness fires per-slot
independently. Reset only that slot's state, don't disturb others.

---

## Wire protocol v2 — full spec

### Outbound (vernier → host)

| Type | Value | Fields |
|---|---|---|
| `T_STATUS` | 1 | `status` (bool), `core_state` (u8), `dev` (u8, optional) |
| `T_DEVINFO` | 2 | `dev` (u8), `device_name`, `order`, `serial` |
| `T_DEVSTATS` | 3 | `dev` (u8), `battery`, `charge_state`, `rssi`, `dropped` |
| `T_FIELDS` | 4 | `dev` (u8), `field_count`, `fields[]` |
| `T_SENS_VALUES` | 5 | `dev` (u8), `sensors[]`, `ts` (u32) |
| `T_ACK` | 7 | `req` (u32), `ok` (bool), `msg`, `dev` (u8, optional — for slot-scoped acks) |
| `T_HELLO` | 8 | `proto_version=2`, `firmware_id`, `version_*`, `max_slots` (NEW: u8 = VERNIER_MAX_SLOTS) |
| `T_DEV_LIST` | 9 | `slots[]` array of `{dev, name, order, connected}` |

### Inbound (host → vernier)

| Cmd | Value | Fields |
|---|---|---|
| `C_CONNECT` | 1 | `name` (str, optional — target name; absent = proximity) |
| `C_DISCONNECT` | 2 | `dev` (u8, required) |
| `C_SET_PERIOD` | 3 | `period_ms` (u32), `dev` (u8, required per D5 A — per-slot period) |
| `C_DEV_LIST` | 4 | (no fields — request enumeration) |

### Back-compat

- v2 vernier ↔ v1 host: vernier sends `dev` field; v1 host's
  ArduinoJson decoder ignores unknown keys; effectively shows slot 0
  data (which has `dev=0`). v1 host sends commands without `dev`; v2
  vernier defaults `dev=0`. Slot 0 fully functional.
- v1 vernier ↔ v2 host: v1 doesn't send `dev`; v2 host's `root["dev"]
  | 0` defaults to 0. Slot 0 fully functional.

→ Wire bump is **non-breaking** in either direction for slot 0
operation. Only multi-slot features need v2 on both sides.

---

## Architecture sketch

### vernier-firmware

```
main.cpp:
  VernierAdapter slots[VERNIER_MAX_SLOTS];
  // slot 0 keeps the existing "vernier" name as alias for back-compat
  VernierAdapter& vernier = slots[0];

  uartHandler: dispatches commands, routes by `dev` field.
  vernierHandler: round-robins waitForSample across occupied slots,
                  sends T_SENS_VALUES with dev=slot_id.
  buttonHandler: long-press disconnects slot 0; short-press
                 connects to first free slot via proximity scan.
```

### gogo-firmware

```
peripherals/gogo-vernier.h:
  struct VernierSlot {
    connection_state_t conn_state;
    char device_name[32], device_order[16], device_serial[16];
    int battery, charge_state, rssi;
    uint32_t dropped, last_frame_ms;
    // ... per-slot sample buffer, fields, etc.
  };
  VernierSlot _slots[VERNIER_MAX_SLOTS];
  uint8_t _primary_slot = 0;  // currently-displayed slot
```

`_handleFrame` reads `dev` field from incoming frames, dispatches to
`_slots[dev]`. UI accessors (`deviceName()`, `batteryPercent()`, etc.)
read from `_slots[_primary_slot]`.

`gogoVernier.cycleSlot()` — rotates `_primary_slot` to next occupied.

---

## Phase plan (under multi-device umbrella)

### Phase 4 step 2 — protocol v2 + this doc ✅ DONE (this commit)
- Document settled.
- Code: bump `VERNIER_PROTOCOL_VERSION = 2`, add `dev=0` to
  outbound frames on vernier side, host parses `dev` field
  (defaulting 0). Slot 0 still single-device. Compatible with
  v1 firmware on either end.

### Phase 4 step 3 — vernier slot pool ✅ DONE
8 sub-commits (3.1–3.8) on vernier-firmware develop:
- 3.1 `8abc856` refactor: wrap singleton in slots[VERNIER_MAX_SLOTS]
- 3.2 `e4675c5` feat(uart): route C_DISCONNECT/C_SET_PERIOD by dev
- 3.3 `d03b7a9` feat(main): connectAndReport(slot, …); per-slot frames
- 3.4 `37d124d` feat(main): C_CONNECT first-free allocator; T_ACK echoes dev
- 3.5 `52269ee` feat(main): vernierHandler iterates slots, per-slot DEVSTATS
- 3.6 `be2006e` feat(uart): T_DEV_LIST emit + C_DEV_LIST handler
- 3.7 `add2926` feat(nvs): per-slot deviceName0..N + legacy migration
- 3.8 `2ef0364` feat(main): buttonHandler multi-slot + force flag

Single-device behaviour preserved end-to-end on the wire (smoke
trace from boot 3.7 shows: scan saved name → handshake → save
deviceName0 → re-load on next boot → auto-connect identically).

### Phase 4 step 4a — host backend slot table ✅ DONE
5 sub-commits on gogo-firmware feature/co-mcu-auto-detect:
- 4a.1 `bed45a5` refactor: VernierSlot[] + primary slot routing
- 4a.2 `6969b32` feat: _handleFrame dispatches by dev field
- 4a.3 `d90c9b2` feat: per-slot liveness watchdog
- 4a.4 `5266358` feat: T_DEV_LIST handler + slot enumeration accessors
- 4a.5 `9cd04b8` feat: outbound commands carry dev; add requestDevList

Backend ready. UI still single-slot (legacy accessors all read
_slots[_primary_slot=0], so display behaves identically). Multi-
slot machinery available for step 4b's UI work.

### Phase 4 step 4b — host UI multi-slot
Split into two batches:

**4b mini-batch ✅ DONE** (3 commits on host
feature/co-mcu-auto-detect):
- 4b.1 `f044e46` feat: request slot enumeration at host boot.
- 4b.2 `eaa582f` feat(vernier-display): VERNIER_CYCLE menu item +
  Slot N/M indicator.
- 4b.3 (no code change): per-slot disconnect already works via
  4a.5's disconnect(dev=primary) + the cycle button picking the
  primary slot.

This gives multi-slot UX (see which slots occupied via cycle,
disconnect a specific slot by cycling first) without redesigning
the display state machine.

**4b.4** — full slot list view + drill-down per design D8
(Option C). Shipped on the host side (`gogo-firmware`); its
detailed plan lived in the vernier repo by mistake and was
removed during the v2.1.0 cleanup.

### Phase 4 step 5 — multi-device smoke
- Hardware test: 1× GDX-LC + 1× GDX-TMP + 1× GDX-ACC simultaneously.
- Verify per-slot liveness, disconnect, reconnect, period change.
- Verify NimBLE 3-connection cap behaviour (4th C_CONNECT returns
  "all slots full").

---

## Decisions ratified

All ACCEPTED with these specifics:

- [x] D1: max slots = 3 (NimBLE default cap)
- [x] D2: vernier-side first-free slot assignment
- [x] D3: wire format `dev` field on per-device frames; absent = 0
- [x] D4: `T_DEV_LIST` (msg 9) auto-pushed on connect/disconnect events
- [x] **D5: per-slot period (Option A) — `C_SET_PERIOD` carries `dev`**
- [x] D6: NVS list `deviceName0..N`, migration from `deviceName`
- [x] D7: single `vernierHandler` task multiplexing slots
- [x] **D8: host UI slot list + drill-down (Option C)**
- [x] D9: connect = first-free; full = `T_ACK ok=false msg="all slots full"`
- [x] D10: per-slot liveness watchdog (extend existing scalar to per-slot)

---

## After ratification

- Implement step 2 (wire protocol bump only — no slot pool yet).
- Smoke single-device against current host firmware to confirm
  back-compat.
- Smoke single-device against host with `dev` field handling to
  confirm forward-compat.
