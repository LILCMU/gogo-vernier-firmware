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

**Cause:** `connectAndReport` runs on whichever task fired it —
the `uartHandler` task on a host `C_CONNECT`, or `loop()` on
auto-connect / button. Each `dev.connect()` call dives through
GoGoVernier's NimBLE-Arduino transport and blocks ~5–7 sec
while the BLE scan + GATT discovery + D2PIO handshake settles.
Whichever task issued the connect can't poll its own work
queue during that window — for `uartHandler` that means host
commands stack up in the 1 KB RX FIFO and dispatch in a burst
once the connect resolves.

**UI mitigation (host side):** `vernierSlotListView` shows
"connecting..." when `e.is_connecting` OR
(`e.connected && e.field_count == 0`). Kid sees activity
instead of a blank row.

**Architectural fix (shipped v2.1.0):** `dev.connect()` was
offloaded to a dedicated `bleWorker` task fed by a `ConnectRequest`
queue. `cmdConnect` enqueues + early-ACKs (`msg="queued"`) so the
UART task stays responsive; `bleWorker` is the single producer of
`dev.connect()`. See the next section for the state machine and the
three latent bugs the v2.1.0 hardware smoke exposed.

---

## bleWorker connect lifecycle + the v2.1.0 multi-device bug class

The non-blocking-connect refactor (v2.1.0) introduced a per-slot
`VernierAdapter::ConnState` state machine driven by `bleWorker`.
Multi-device reconnect had **never been hardware-smoked before
v2.1.0** — the smoke immediately surfaced three latent bugs (two were
present since v2.0.0). If you touch the connect path, read this
first; every bug here is a "looks fine in code review, fails on three
real sensors" trap.

### The state machine (all transitions are atomic CAS, on purpose)

`ConnState`: `IDLE(0) → REQUESTED(1) → CONNECTING(2) → READY(3)`.
The four transition helpers on `VernierAdapter` are each a
`compare_exchange_strong`, not a plain store, because every one of
them races another task:

- **`tryRequestConnect()` — IDLE→REQUESTED** (producer:
  `enqueueConnect`, called from cmdConnect / button / autoConnect).
  CAS so a duplicate enqueue for an already-claimed slot is a clean
  no-op. **Bug #1 (since v2.0.0):** the original `enqueueConnect` did
  an *unconditional* `setState(REQUESTED)` then rolled back to IDLE
  if the queue was full. The host sends one `C_SET_PERIOD` per slot
  at boot, each re-arming `startAutoConnect`, so `autoConnectDevice`
  runs several times; a later pass's rollback clobbered slots that
  already had a valid queued request → only slot 0 reconnected.
- **`tryAcquireConnecting()` — REQUESTED→CONNECTING** (bleWorker on
  dequeue). CAS so a `cmdCancelConnect` that flipped the slot to IDLE
  while it sat queued makes the worker skip it.
- **`tryFinishConnecting()` — CONNECTING→READY**
  (`publishConnectResult` success tail, AFTER all the per-slot
  sends). **Bug #3 (CRITICAL, found by a pre-tag audit):** a
  C_DISCONNECT/C_FORGET landing during the ~7 s handshake runs
  `disconnect()` on the UART task; its `_gv.stop()/close()` block on
  GoGoVernier's `session_mutex` behind the worker's `open()`, then it
  stores IDLE. Meanwhile the worker stored READY unconditionally —
  the two stores raced with no ordering, leaving the slot either
  shown-connected-but-gone or stuck-READY-never-streaming. The CAS
  abandons the connect (tears the link back down) if a disconnect
  won the race.
- **`tryCancelConnect()` — REQUESTED→IDLE** (`cmdCancelConnect`).
  CAS so it loses cleanly if the worker already moved the slot to
  CONNECTING.

`disconnect()` sets IDLE unconditionally (teardown is always
allowed); `publishConnectResult`'s READY is the only success publish
and it's gated by the CAS above.

### Bug #2 (since v2.0.0): T_HELLO per-connect is a host-reset storm

T_HELLO is the GLOBAL boot handshake; the host's T_HELLO handler
calls `_resetConnectionState()` (it reads T_HELLO as "peer
rebooted"). The old `connectAndReport` emitted T_HELLO on **every**
connect, so during a sequential 3-slot reconnect each new connect
wiped the previously-connected slots back to "connecting" — only the
last slot kept its fields, the earlier two recovered ~30 s later via
the periodic T_FIELDS re-emit. **Rule: T_HELLO + T_STATUS are sent
exactly once per session**, lazily on the first host command
(`dispatchHostCommand`), NEVER per connect. `publishConnectResult`
emits only per-slot frames (T_DEVINFO / T_DEVSTATS / T_FIELDS) +
T_DEV_LIST. If you add a frame to the connect path, do NOT add a
global/reset-bearing one.

### Ordering invariant (T_FIELDS before T_SENS_VALUES)

`publishConnectResult` sends T_DEVINFO/T_DEVSTATS/T_FIELDS, then a
compiler barrier, then the CONNECTING→READY CAS. `vernierHandler`
gates streaming on `isReady() == (state()==READY)`, so it cannot emit
T_SENS_VALUES before T_FIELDS shipped. Don't move the state publish
above the sends (that was a real bug, fixed in `da5e19c`).

### Known gap (deferred): stale queued request + same-slot re-connect

If a slot is REQUESTED (queued, not yet drained) and a
forget/disconnect flips it to IDLE, the stale queue entry remains. A
new C_CONNECT for the same slot within the ~7–14 s drain window
re-sets REQUESTED and queues a second request; the worker dequeues
the STALE entry first, its REQUESTED→CONNECTING CAS now succeeds, and
it connects with the stale request's `force` flag. Low probability;
the clean fix is a per-slot generation counter packed with
`_conn_state` into one atomic word so the claim CAS validates both.
Documented in `.claude/plans/release-2.1.0.md` §Still deferred.

### Meta-lesson

Three concurrency bugs in one path, none caught by code review,
all caught by running three real sensors through a power-cycle.
**Hardware-smoke the multi-device path on every release that touches
connect/disconnect** — single-sensor testing exercises none of the
cross-slot races.

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

The vernier-firmware namespace `"vernierSetting"` (defined as
`NVS_NAMESPACE_SETTING` in `include/main.h`) holds only
`deviceName{slot}` keys today. All other Vernier-related NVS
keys live on the host side. Renaming this constant would orphan
existing paired devices on installed boards, so keep it stable
unless you ship a migration step.

---

## Boot auto-connect choreography

Vernier MCU side:
1. Boot reads NVS `deviceName0..N`. Sets `slotHasSavedDevice[i]`
   per slot and the global `foundSavedDevice` if any are saved.
2. Host's first command triggers the one-shot boot handshake in
   `dispatchHostCommand`: T_HELLO (peer identity + proto version)
   then T_STATUS(true) (peer up). Sent **once per co-MCU session**,
   never per connect.
3. Host sends `C_SET_PERIOD` per slot at boot. The first one with
   `foundSavedDevice == true` flips `startAutoConnect = true`.
4. Next loop tick, `autoConnectDevice` walks slots; for each with
   `slotHasSavedDevice[i]` it `enqueueConnect(i)` (CAS IDLE→REQUESTED;
   a duplicate pass after another `C_SET_PERIOD` re-arm is a no-op).
   bleWorker drains the queue, connecting one slot at a time.
5. For each successful connect, `publishConnectResult(slot)` emits
   only the per-slot frames — T_DEVINFO / T_DEVSTATS / T_FIELDS —
   then T_DEV_LIST. **No T_HELLO/T_STATUS here**: emitting T_HELLO
   per connect made the host's T_HELLO handler
   (`_resetConnectionState`) wipe every already-connected slot back
   to "connecting" on each new slot's connect, so only the last slot
   of a multi-slot reconnect kept its fields. The handshake moved to
   step 2 (once) to fix this.

Host side (post-fix):
- The auto-detect-co-MCU path on the host **no longer** issues an
  unsolicited `C_CONNECT` (it used to). Instead it sends
  `C_DEV_LIST` so the host's slot table populates immediately.
  Vernier-firmware's saved-device auto-reconnect handles the
  pairing per-slot, and the host learns about it via the resulting
  T_DEV_LIST + per-slot frames. Avoids the host-vs-vernier slot
  race that lit up the wrong slot's CONNECTING animation.

---

## Cross-task discipline (FreeRTOS task hand-off)

Single-core RISC-V (ESP32-C3) means aligned word loads/stores are
atomic and no Xtensa `memw` instruction exists (it would not
assemble). Two patterns in use, depending on the shape of the data:

- **`std::atomic<T>` with `memory_order_relaxed`** for standalone
  scalars with no ordering dependency on other writes. After H2
  (v2.0.0 → v2.1.0), this covers `VernierAdapter::_period_ms` and
  `VernierAdapter::_push_dropped`. Preferred for new cross-task
  scalars — the type itself documents the contract and the codegen
  is identical to `volatile uint32_t` on RV32.
- **`std::atomic<T>` release/acquire** for the fill-then-flag publish
  pattern, where a writer fills a buffer / array then flips a flag the
  reader polls. The boot-time hand-off triple `startAutoConnect`,
  `foundSavedDevice`, `slotHasSavedDevice[]` uses this (promoted off
  `volatile` + `__asm__` barrier when the 2.2.0 backlog folded into
  2.1.0). The array is relaxed; the two gate flags carry the ordering:
  `setup()` / `publishConnectResult` fill `slotHasSavedDevice[]`
  (relaxed) then store `foundSavedDevice` release; `C_SET_PERIOD`
  acquire-loads `foundSavedDevice`, then release-stores
  `startAutoConnect`; `autoConnectDevice` acquire-loads
  `startAutoConnect` and reads the array. Each release→acquire edge
  makes the fills visible without an explicit barrier. The legacy
  `volatile` + `__asm__ volatile("" ::: "memory")` form is equivalent
  on single-core ESP32-C3 but release/acquire documents the ordering
  portably and is preferred for new code.

Tasks at play:
- `setup()` → fills NVS + per-slot saved-device flags.
- `uartHandler` task → reads C_SET_PERIOD, flips startAutoConnect.
- `loop()` → reads startAutoConnect, dispatches autoConnectDevice.
- `vernierHandler` task → polls slots[i] state.

`nvsMutex` (FreeRTOS semaphore) gates every Preferences begin/end
pair so concurrent NVS reads/writes from setup, uartHandler and
loop never collide.

---

## NACK acks always echo `dev` for slot-scoped commands

Every C_CONNECT / C_DISCONNECT / C_FORGET / C_SET_PERIOD NACK
includes the requested `dev` in the ack so the host can roll back
its per-slot UI tracker (the `is_connecting` animation) against the
right card. Without this, a "slot busy" or "dev out of range" reply
forces the host to guess the slot from `req`, which only works if
exactly one outstanding command is mapped 1:1 to a slot.

Wire layout: `T_ACK = {t, req:u32, ok:bool, msg?:str, dev?:i16}`.
`dev = -1` (the default in `sendAck`) means "no slot context",
typical for global commands like C_DEV_LIST.

The C_CONNECT ok-path echoes `dev` on its early-ACK
(`msg="queued"`, sent by `cmdConnect` at enqueue time — v2.1.0;
there is no trailing completion ACK, the outcome rides out on the
auto-pushed T_DEV_LIST). On the failure paths the host gets enough
info to clear the CONNECTING animation immediately instead of
waiting on the slot's CONNECTING handshake timeout.

---

## Wire-protocol enums — single source of truth

`UartAdapter::MsgType` (T_*) and `UartAdapter::CmdType` (C_*) are
the canonical wire-protocol numeric IDs. Both are public enums
inside `UartAdapter` with byte-layout comments next to each
constant. `UartAdapter::CoreState` enumerates the values carried
by the global `T_STATUS.core_state` field.

Older code kept a duplicate `enum CommandType` inside `uartHandler`'s
function body. That duplicate was dropped — the dispatcher pulls
`constexpr auto C_CONNECT = UartAdapter::C_CONNECT;` etc. into
local scope so case labels stay terse without a second source of
truth to drift.

When adding a new wire frame:
1. Bump `VERNIER_PROTOCOL_VERSION` if existing field semantics
   change (additive fields are optional and don't require a bump).
2. Add the constant + byte-layout comment in `uart-adapter.h`.
3. Add the handler in `_handleFrame` (host) or `onCommand`
   (vernier) branching on the enum value.
4. Mirror in the host's command/message enum in `gogo-vernier.h`.

---

## Deferred / known issues (do before architecture lands)

- **Mid-handshake C_DISCONNECT race — RESOLVED in v2.1.0.** Connect
  no longer blocks uartHandler (it's on `bleWorker`), and a
  C_DISCONNECT/C_FORGET arriving while the slot is CONNECTING is
  handled by the `tryFinishConnecting()` CAS in `publishConnectResult`:
  if the disconnect won, the worker abandons the connect and tears the
  link down instead of leaving the slot READY. See the "bleWorker
  connect lifecycle" section above. (Residual low-probability gap —
  stale queued request + same-slot re-connect — is the only piece
  still deferred; see that section.)
- **Per-slot auto-connect failure cap.** A slot whose saved device
  vanished burns ~7 sec at every boot retrying. Add a per-slot
  `_autoConnectFailures` counter; clear `slotHasSavedDevice[slot]`
  + the NVS key after K consecutive failures.

---

## Cross-references

- Plans: `.claude/plans/multi-device-design.md` (D-decisions),
  `.claude/plans/step-5-hardware-smoke.md` (Phase 4 verification
  baseline), `.claude/plans/release-2.1.0.md` +
  `.claude/plans/2-1-0-hardware-smoke.md` (v2.1.0 cycle). The
  host-side `multi-device-ui-step-4b4.md` UI plan was removed —
  it belonged in `gogo-firmware`, not here, and shipped long ago.
- Earlier knowledge:
  - `.claude/knowledges/d2pio-debug-findings.md` — protocol-level
    debugging session notes (preserved as-is, captures pre-Phase-4
    findings).
- Submodule: `lib/GoGoVernier@main` (pinned at v1.0.0) carries the GDX driver
  including `refreshStatus()` battery support.
- Host-side display patterns:
  `gogo-firmware/.claude/knowledges/vernier-display-patterns.md`.
