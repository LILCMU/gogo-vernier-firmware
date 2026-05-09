# Vernier MCU internals — protocol v2 routing, GDX battery, NVS, slot serialization

Durable findings from the ESP32-C3 co-MCU side of Phase 4 multi-
device + kid-UX work (2026). Each section captures a lesson that
cost real debugging time and applies to future work on this
firmware.

Cross-repo: host-side TFT display patterns (TFT_eSPI quirks,
diff-render, sprite double buffering, layout constraints) live in
the gogo-firmware repo at
`.claude/knowledges/vernier-display-patterns.md`.

---

## Protocol v2 — `dev` field routing

Per-device frames carry `dev` (u8) so a single co-MCU can drive
multiple GDX sensors over one UART link. Categories:

| Frame | Has `dev`? | Notes |
|-------|-----------|-------|
| T_HELLO | no | Global handshake. Carries `proto_version`, `firmware_id`, `version_*`, `max_slots`. |
| **T_STATUS** | **no** | **Global** peer-health frame. `status=true` means peer is up; `status=false` is a peer-wide reset signal (cable yank / power cycle). Per-slot connectedness is owned by T_DEV_LIST, not T_STATUS. |
| T_DEVINFO  | yes | Per-slot device identity (name, order, serial). Sent once per connect. |
| T_DEVSTATS | yes | Per-slot battery / charge / RSSI / dropped sample counter. |
| T_FIELDS   | yes | Per-slot enabled-channel name+unit list. Bumps host's `fields_version`. |
| T_SENS_VALUES | yes | Per-slot sample frame. |
| T_ACK      | (echoes seq + dev where applicable) | Replies to host commands. |
| T_DEV_LIST | n/a | Carries an `entries[]` array of `{dev, name, order, connected}` for the full slot table. Auto-emitted on every connect/disconnect event per design D4. |

**Pitfall (host-side bug we hit):** the host dispatcher computed
`dev_field = root["dev"] | 0` for *every* frame, then the T_STATUS
handler used that to mutate `_slots[0].conn_state`. After a kid
pressed slot 3, vernier's connectAndReport burst included a global
T_STATUS(true,1) which the host miscredited to slot 0. Slot 0
flipped to CONNECTED-without-fields and rendered "connecting…"
on the wrong card. Fixed host-side by making T_STATUS skip the
slot mutation entirely (status=false still resets peer-wide).

**Lesson:** when a frame is global, the dispatcher's "default
dev=0" is meaningless and the handler must not act on it. Either
gate per-slot mutations on `root["dev"].isNull() == false`, or
keep global handlers segregated from per-slot ones.

---

## C_CONNECT slot targeting (added in this branch)

C_CONNECT now honors an optional `dev` field so the kid's pressed
slot is the one we allocate, not whichever slot happens to be
first-free. Wire layout:

```
{c: C_CONNECT(1), seq: u32, dev?: u8}
```

Vernier-side handler at `src/main.cpp` C_CONNECT case:

1. If `dev` present:
   - validate `dev < VERNIER_MAX_SLOTS` else NACK `"dev out of range"`.
   - validate `slots[dev].isReady() == false && isStreaming() == false`
     else NACK `"slot busy"`.
   - target_slot = dev.
2. Else (host omitted `dev` — auto-detect path / legacy / button
   shortcut): walk slots 0..N for first non-ready/non-streaming;
   NACK `"all slots full"` if none free. Per design D2.

**Presence check.** ArduinoJson 7's `is<uint8_t>()` rejects values
the MsgPack codec encoded as a narrower int width (e.g. positive
fixint → int8). Use `!root["dev"].isNull()` to detect presence and
read as `int` with an explicit range guard. Earlier logic with
`is<uint8_t>()` silently fell through to the first-free branch and
paired the wrong slot.

**Logging.** `log_i("C_CONNECT: target slot=%u (%s)", target_slot,
hasDev ? "host-specified" : "first-free")` so the wire behaviour
is visible during smoke tests.

---

## C_FORGET — disconnect + clear NVS deviceName{slot}

```
{c: C_FORGET(5), seq: u32, dev: u8}
```

Tells the co-MCU to drop the BLE link AND clear the slot's NVS
`deviceName{slot}` key so it won't auto-reconnect on next boot.
Used by the host's "Forget" action in Vernier > Settings sub-page.

**Shortcut:** if the slot has no persistent state (already
disconnected and no saved device), C_FORGET ACKs immediately
without touching BLE — avoids a no-op handshake teardown.

**Symmetry note:** the host clears its own companion per-slot
NVS keys (`vernierPrFld{slot}`, `vernierPrd{slot}`) separately —
they live on the host side, not the co-MCU.

---

## GDX CMD_GET_STATUS — battery readout

**Problem:** Vernier's published D2PIO opcode list does not
include a "get status" command, so the GDX driver
(`lib/GoGoVernier`) shipped without battery readout — `refreshStatus()`
only refreshed RSSI from the BLE link, leaving `battery_percent
== 0` forever.

**godirect-py reference:** the official Python driver hardcodes
`CMD_ID_GET_STATUS = 0x10` (`godirect/device.py`). Source:
[godirect-py device.py — _GDX_get_status](https://github.com/VernierST/godirect-py/blob/master/godirect/device.py).

**Wire request:** `[0x58, len, rolling_counter, checksum, 0x10]`.
Same envelope as every other GDX command.

**Wire response:** Python struct format `'<xxxxxxBBBBHBBHBB'`
(unpacked after the 6-byte frame header):

| Offset (from frame start) | Type | Field |
|---------------------------|------|-------|
| 0..5                      | header (skipped) | — |
| 6                         | u8   | status |
| 7                         | u8   | spare |
| 8                         | u8   | primaryCpuMajor |
| 9                         | u8   | primaryCpuMinor |
| 10..11                    | u16 LE | primaryCpuBuild |
| 12                        | u8   | secondaryCpuMajor |
| 13                        | u8   | secondaryCpuMinor |
| 14..15                    | u16 LE | secondaryCpuBuild |
| 16                        | u8   | **batteryLevelPercent** |
| 17                        | u8   | **chargerState** |

**Implementation:** `lib/GoGoVernier/src/GoGoVernier.cpp::refreshStatus()`
sends CMD_GET_STATUS via the existing `encode + sendRequest`
pattern, validates `resp_len >= 18`, then reads
`resp_buf[16]` and `resp_buf[17]`. Charger state clamped to
the `ChargerState` enum range.

**Caveat:** observed timeout on slow GDX devices isn't catastrophic
— prior cached values persist, periodic `refreshStatus()` retries.

---

## Multi-slot connect serialization

**Symptom:** in a 3-sensor auto-connect, slot 0's first sample
arrives ~7 seconds after slot 0 itself reports "streaming
started" — the host UI sees `connected=true, has_sample=false`
for ~7 sec.

**Cause:** `vernierHandler` is a single FreeRTOS task that
round-robins `vernier.poll()` across slots and also drives
`autoConnectDevice` sequentially. While slot N is in BLE
handshake (synchronous calls into ArduinoBLE that block ~3 sec
each), already-connected slots can't be polled. After all 3
slots finish handshake, polling resumes for all of them and
samples flow.

**UI mitigation (host side):** `vernierSlotListView` shows
"connecting..." when `e.is_connecting` OR
(`e.connected && e.field_count == 0`). Kid sees activity
instead of a blank row.

**Architectural fix (deferred):** parallelize connect off the
poll task. ArduinoBLE/NimBLE supports async scan callbacks; one
approach is to launch handshakes from a background `ble_task`
and let `vernierHandler` keep polling whichever slots are in
the streaming state. Out of scope for kid-UX redesign.

---

## NVS key budget on ESP32 Preferences

Preferences (NVS) keys are limited to **15 chars** + null
terminator. Format strings used in this codebase:

| Format | Max expanded | Fits |
|--------|--------------|------|
| `deviceName%u`   | `deviceName9` (11 chars) | ✅ |
| `vernierPrFld%u` (host side) | `vernierPrFld9` (13 chars) | ✅ |
| `vernierPrd%u` (host side)   | `vernierPrd9` (11 chars) | ✅ |
| `vernierPeriodPreset%u` (rejected) | 21 chars | ❌ |

If a key formatter overruns 15 chars Preferences silently
truncates and you get key collisions (every slot writes to the
same truncated key). **Always test with the maximum slot index.**

The vernier-firmware namespace `"vennierSetting"` (typo
intentional — changing it orphans existing devices) holds only
`deviceName{slot}` keys today. All other Vernier-related NVS
keys live on the host side.

---

## Boot auto-connect choreography

Vernier MCU side:
1. Boot reads NVS `deviceName0..N`. Sets `slotHasSavedDevice[i]`
   per slot and the global `foundSavedDevice` if any are saved.
2. Host sends `C_SET_PERIOD` per slot at boot. The first one with
   `foundSavedDevice == true` flips `startAutoConnect = true`.
3. Next loop tick, `autoConnectDevice` walks slots; for each
   with `slotHasSavedDevice[i]` it calls `connectAndReport(i)`.
4. Each `connectAndReport(slot)` emits T_HELLO, T_STATUS(true,1)
   (global), T_DEVINFO/T_DEVSTATS/T_FIELDS (per-slot), then
   T_DEV_LIST. Host learns the slot's identity.

Host side (post-fix):
- The auto-detect-co-MCU path on the host **no longer** issues an
  unsolicited `C_CONNECT` (it used to). Instead it sends
  `C_DEV_LIST` so the host's slot table populates immediately.
  Vernier-firmware's saved-device auto-reconnect handles the
  pairing per-slot, and the host learns about it via the resulting
  T_DEV_LIST + per-slot frames. Avoids the host-vs-vernier slot
  race that lit up the wrong slot's CONNECTING animation.

---

## Cross-references

- Plans: `.claude/plans/multi-device-ui-step-4b4.md`,
  `.claude/plans/multi-device-design.md` (D-decisions),
  `.claude/plans/step-5-hardware-smoke.md` (verification plan).
- Earlier knowledge:
  - `.claude/knowledges/d2pio-debug-findings.md` — protocol-level
    debugging session notes (preserved as-is, captures pre-Phase-4
    findings).
- Submodule: `lib/GoGoVernier@main` carries the GDX driver
  including `refreshStatus()` battery support.
- Host-side display patterns:
  `gogo-firmware/.claude/knowledges/vernier-display-patterns.md`.
