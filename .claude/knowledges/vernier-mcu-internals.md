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
| T_DEV_LIST | n/a | Carries an `entries[]` array of `{dev, name, order, connected}` for the full slot table. Auto-emitted on every connect/disconnect event per design D4. Emitted from uartHandler, loop and bleWorker; `emitDevList()` holds `devListMutex` (control-loop.cpp) across the slot-state snapshot and the send, so frames leave in snapshot order and the last T_DEV_LIST always carries the newest table. Before that a snapshot taken mid-connect could be sent after bleWorker's newer READY one, and a host that frees slots on `connected=false` would drop a live slot. |

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

Vernier-side handler: `cmdConnect` in `src/control-loop.cpp`, which
allocates the slot and hands it to `requestUserConnect()`:

1. If `dev` present:
   - validate `dev < VERNIER_MAX_SLOTS` else NACK `"dev out of range"`.
   - target_slot = dev.
2. Else (host omitted `dev` — auto-detect path / legacy / button
   shortcut): `firstFreeSlot()`, the lowest IDLE slot, preferring one
   that is not waiting for a lost sensor (v2.3.0); NACK
   `"all slots full"` if none free. Per design D2.
3. `requestUserConnect()`: claim the slot's background reconnect if it
   has one (ARMED → ADOPTED), then queue a proximity connect
   (`force=true`) → ACK `"queued"`. If the slot is busy: ACK ok
   `"reconnecting"` when it was a background attempt for the saved
   sensor (that attempt now answers the host, see "Background
   reconnect after a link loss or a failed boot connect"), else NACK `"slot busy"` (READY, or
   connecting for another producer).

**Presence check.** ArduinoJson 7's `is<uint8_t>()` rejects values
the MsgPack codec encoded as a narrower int width (e.g. positive
fixint → int8). Use `!root["dev"].isNull()` to detect presence and
read as `int` with an explicit range guard. Earlier logic with
`is<uint8_t>()` silently fell through to the first-free branch and
paired the wrong slot.

**Logging** (debug builds): `C_CONNECT: slot N queued (host-specified
| first-free)`, `C_CONNECT: slot N takes over its reconnect attempt`,
`C_CONNECT NACK: slot N busy`, so the wire behaviour is visible during
smoke tests.

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
without touching BLE — avoids a no-op handshake teardown. It still
stores `ReconnectState` OFF (v2.3.0).

**Background reconnect (v2.3.0):** C_FORGET, C_DISCONNECT and the BOOT
long-press call `disconnect(/*disarm=*/true)`, which stores
`ReconnectState` OFF under the adapter's `_lifecycle_mutex`, before the
teardown, so a sensor whose link had dropped is not brought back in
the background, even when the link-loss release is running at the same
moment. C_DISCONNECT keeps the NVS name (the next boot still
auto-connects it); C_FORGET clears it. A connect that succeeds during
a Forget does not write the name back (see "Races closed").

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
  won the race. Since v2.3.0 it runs under the adapter's
  `_lifecycle_mutex` together with the OFF store of the background
  reconnect and a hook (`onReady`, `onSensorReady` in control-loop)
  that saves the sensor's name to NVS (see "Races closed").
- **`tryCancelConnect()` — REQUESTED→IDLE** (`cmdCancelConnect`).
  CAS so it loses cleanly if the worker already moved the slot to
  CONNECTING.
- **`tryArmFailedConnect()` — CONNECTING→IDLE** (v2.3.0,
  `publishConnectResult` failure tail of the boot connect), then
  OFF→ARMED, both under `_lifecycle_mutex`. Every other failure stores
  IDLE plainly.

`disconnect(disarm)` sets IDLE unconditionally (teardown is always
allowed); `publishConnectResult`'s READY is the only success publish
and it's gated by the CAS above.

### Link loss frees the slot (READY → IDLE without a host command)

Before this fix a sensor that dropped its BLE link (switched off, out
of range, battery dead) left its slot READY until the co-MCU was
power-cycled. `GoGoVernier::isConnected()` is the *session* flag (true
until `close()`), and `NimBleXport`'s `onDisconnect` only cleared the
transport's own flag, so nothing in the firmware noticed. The host's
liveness watchdog flipped its card to idle after 5-30 s, but the next
`C_CONNECT` for that slot was NACKed `"slot busy"` and a GoGo reboot's
auto-connect failed the IDLE→REQUESTED CAS. Only the BOOT long-press or
a power cycle recovered it.

Now:
- `lib/GoGoVernier` exposes `GoGoVernier::isLinkUp()`, a lock-free read
  of the transport flag that `onDisconnect` clears
  (`NimBleXport::Impl::connected`, now `std::atomic<bool>`).
- `loop()` calls `releaseLostLinks()` (control-loop.cpp), which asks
  every slot `VernierAdapter::releaseIfLinkLost()` on each pass. When the link is down it runs the same
  teardown as `disconnect()` (clear `onSample`, `_gv.close()` to free
  the NimBLE client, state IDLE), logs `slot N: BLE link lost, slot
  freed`, and pushes T_DEV_LIST (`state=0`, `connected=false`).
- Detection latency is the BLE supervision timeout. The GDX-ACC asks
  for 5.00 s (interval 6/6, latency 0) right after connect and NimBLE
  applies it, so the C3 learned of a switch-off 5.0 s later, after the
  GoGo's own 5 s liveness timer had reset the slot (measured
  2026-10-20: T_DEV_LIST arrived ~150 ms late).
  `NimBleXport` now connects with 7.5 ms / latency 0 / 2.0 s
  (`SUPERVISION_TIMEOUT_MAX_UNITS = 200`, 10 ms units) and rejects any
  peer request above that cap. The supervision timer starts at the last
  link-layer packet, i.e. when the sensor goes silent, so the loss is
  expected about 2 s after that: up to period + 2 s after the last
  sample, and only when no other slot is connecting (a connect holds
  `g_ble_mutex` for 5-9 s). Expected value, not yet measured with the
  cap on a real switch-off. Fallback if a peer drops the link on
  rejection: accept, then `updateConnParams()` after the handshake.
- `disconnect(disarm)` and `releaseIfLinkLost(arm)` share
  `VernierAdapter::_lifecycle_mutex`. Teardown runs on three tasks
  (UART, loop, bleWorker), and two concurrent teardowns would reset the
  same `std::function` at once. The mutex also orders the
  `ReconnectState` stores that go with a teardown (see "Races closed").
- Why `loop()` and not `vernierHandler`: `_gv.close()` waits on the
  lib-wide `g_ble_mutex`, which `NimBleXport::connect()` holds for a
  whole scan + connect (about 5 to 9 s). Run on vernierHandler, one
  dropped sensor during another slot's connect would stall samples for
  every healthy slot past the GoGo's 5 s liveness timeout, and the host
  would reset them too. vernierHandler only skips slots that are no
  longer READY, and `startReading()` returns at once on a slot whose
  link is down: `_gv.start()` would otherwise block on the session
  mutex that the teardown's `close()` holds and stall every slot. The loop task's button polling
  waits instead, as it already does for a long-press disconnect.
- The saved NVS name is kept, and since v2.3.0 the slot is retried in
  the background (next section). Up to v2.2.0 it was not (owner
  question Q-C4, default "free only"): it came back only on a host
  `C_CONNECT`, the next auto-connect pass, or the next boot. The owner
  changed that decision on 2026-10-30.
- Host side (gogo-firmware `gogo-vernier.cpp`): T_DEV_LIST
  `connected=false` on a CONNECTED slot resets it to IDLE at once (log
  `Vernier slot N reported disconnected by T_DEV_LIST`), 58-70 ms after
  the C3's `slot N: BLE link lost` on board B (2026-10-30). The host
  never downgrades a CONNECTING slot through T_DEV_LIST and ignores the
  `state` key.

### Background reconnect after a link loss or a failed boot connect (v2.3.0)

A sensor the user connected, and did not disconnect or forget, comes
back on its own after it was switched off or left range, or when it is
switched on after the C3 booted. Before v2.3.0 it stayed free until
someone pressed Connect (board B, 2026-10-30: no rescan for over a
minute; Connect paired it in ~7 s), and a saved sensor that was off
at boot was tried once only.

**Policy.** Per-slot `VernierAdapter::ReconnectState` (not on the
wire), written from several tasks, so a relaxed `std::atomic`:

| State | Host sees (T_DEV_LIST) | Entered by | Left by |
|---|---|---|---|
| OFF | the real `ConnState` | boot; a successful connect; C_DISCONNECT, C_FORGET, BOOT long-press; the give-up time; the same sensor connected to another slot | link loss of a READY slot with a saved name → ARMED; failed boot connect of a saved name → ARMED |
| ARMED | `connected=false`, `state=0`, `name=""`, even while an attempt has the slot REQUESTED / CONNECTING | `releaseIfLinkLost(true)`; `tryArmFailedConnect()`; a failed or cancelled ADOPTED connect | the sensor is READY → OFF; a user connect → ADOPTED; C_DISCONNECT / C_FORGET / long-press → OFF; `RECONNECT_GIVE_UP_MS` after the arming → OFF; its sensor connected to another slot → OFF |
| ADOPTED | the real `ConnState` | a host C_CONNECT or BOOT short-press on an ARMED slot (`tryAdoptReconnect`, CAS ARMED → ADOPTED) | success → OFF; failure or C_CANCEL_CONNECT → ARMED (`tryRearmAdoptedReconnect`); C_DISCONNECT / C_FORGET / long-press → OFF |

**Who arms.**
- Link loss: `releaseLostLinks()` → `releaseIfLinkLost(arm =
  slotHasSavedDevice[i])`, which stores ARMED inside the lifecycle lock.
- Boot: each `ConnectRequest` carries a `ConnectOrigin` (`USER`,
  `BOOT`, `BACKGROUND`). Only the first `autoConnectDevice()` pass is
  `BOOT`; later passes (the host re-sending `C_SET_PERIOD`) are `USER`,
  so they never arm a slot the user disconnected. A failed `BOOT`
  attempt on a slot with a saved name, whose sensor is not READY in
  another slot, is armed by `tryArmFailedConnect()` (CONNECTING → IDLE
  then OFF → ARMED under the lifecycle lock) and still pushes
  T_DEV_LIST, because the host saw the attempt as connecting.

**Retry schedule** (`reconnectLostSensors()`, loop task; per-slot
`ReconnectSchedule` is loop-only):
- The schedule starts on the first pass that sees the slot leave OFF,
  whoever armed it (a pass runs every loop iteration): first rescan
  `RECONNECT_FIRST_RETRY_MS` (1 s) later, then the gap after each
  failed rescan grows ×`RECONNECT_BACKOFF_FACTOR` (2) up to
  `RECONNECT_BACKOFF_MAX_MS` (30 s): gaps 1, 2, 4, 8, 16, 30, 30 ... s,
  measured from the end of the previous attempt. Starting it here
  rather than at each arming site means no arming path can leave
  `backoffMs` at 0 (0 × 2 stays 0: a rescan with no gap).
- **Give-up** (owner decision, 2026-10-30): `RECONNECT_GIVE_UP_MS` =
  10 min after the arming, checked between two attempts (an attempt
  in flight finishes first). The slot goes ARMED → OFF
  (`tryGiveUpReconnect`), logs `slot N: sensor not back after N ms,
  background reconnect stopped`, and pushes nothing: the host already
  shows it disconnected. The NVS name stays, so a Connect press, the
  next C3 boot, or the host's next `C_SET_PERIOD` pass still connect it
  the normal way. An ADOPTED interlude does not restart the clock.
  Why not "never": a board left on would keep scanning, the host has
  no control to stop it for a slot it already shows disconnected
  (Forget is on a connected slot only), and it would grab its old
  sensor whenever that advertises nearby, even while another board is
  pairing it. 10 min is about 20 attempts. `0` would mean never.
- An attempt is an ordinary `connect(false)` on bleWorker: one
  `GoGoVernier::open(<saved name>)`, a scan of up to `OPEN_SCAN_MS`
  (5 s) that stops at the first advert of the name, then the
  handshake. The target is `VernierAdapter::_open_device`, which
  `saveConnectedSensor` now sets to the device name after every
  successful connect; before v2.3.0 it was only set from NVS at boot,
  so after a proximity Connect a `connect(false)` would have scanned
  for "proximity" and taken the nearest GDX.
- One attempt at a time, queued only while no slot is REQUESTED or
  CONNECTING. A host or boot connect therefore waits behind at most
  the one attempt already running. Samples of the other slots keep
  flowing: `NimBleXport::write()` takes no lock; only connect,
  disconnect and unsubscribe take `g_ble_mutex`, so a C_DISCONNECT /
  C_FORGET of another slot during a scan blocks the UART task for up
  to ~5 s, as it already did during any connect.
- Radio cost while a sensor stays away, **per armed slot**: 5 s of
  scanning per 35 s (~14 %) once the gap is capped. Attempts are
  serialised, so it adds up: ~29 % with two armed slots, ~43 % with
  three, and close to continuous in the first minute after several
  sensors go away at once (gaps of 1-16 s). Roughly +12 mA average per
  armed slot [estimate from the C3's ~80-90 mA receive current, not
  measured]. The give-up bounds it to 10 min.

**Wire.** No new command or frame; protocol 2 unchanged. While ARMED,
the attempt pushes nothing: bleWorker skips its "connecting" T_DEV_LIST
and `publishConnectResult` skips the push after a failed attempt, so a
sensor that stays away costs no UART traffic. On success the normal
connect burst goes out: T_DEVINFO, T_DEVSTATS, T_FIELDS, then
T_DEV_LIST `connected=true`, which the host takes as CONNECTED (same
frames as the boot auto-connect, item (d) of the 2026-10-30 bench).
A host that renders `state` would need a new value to show
"reconnecting"; none does, so the slot stays plain disconnected.

**C_CONNECT meanwhile** (`requestUserConnect`): ARMED → ADOPTED, then
- between two attempts (slot IDLE): a normal proximity connect, ACK
  `"queued"`;
- during an attempt (slot REQUESTED / CONNECTING for the saved name):
  ACK ok `"reconnecting"` + T_DEV_LIST with the real state; the
  attempt answers the host. If it fails, the slot goes back to ARMED
  and the host's own 12 s connect timeout returns its card to idle.
  The host gets the push either way; it ignores `connected=false` for a
  CONNECTING slot.

**Races closed.** The review of the first cut (`3963cc5`) found the
link-loss, Forget-vs-save and two-slot items below still open; they
were fixed before the tag, together with the boot arming.
- Disconnect/forget vs. a link-loss release: `releaseIfLinkLost(true)`
  stores ARMED before it releases `_lifecycle_mutex`, and
  `disconnect(true)` (C_DISCONNECT, C_FORGET, long-press) stores OFF
  after it takes it. Whichever order the two tasks run in, the explicit
  action ends OFF. (The first cut stored ARMED after the lock, so a
  Forget waiting on the teardown could be re-armed; a second Forget
  then hit the "already empty" path, which now also stores OFF.)
- Disconnect/forget vs. a failed boot connect: `tryArmFailedConnect`
  needs CONNECTING, and a teardown sets IDLE, both under the lock; a
  teardown that ran first blocks the arming, one that runs after
  disarms it.
- Forget or link loss vs. a connect that just succeeded: the name is
  saved by `tryFinishConnecting`'s hook, after the CONNECTING → READY
  CAS and while the slot's lifecycle lock is still held (lock order
  lifecycle → `nvsMutex`, the same as `cmdForget`). No teardown can run
  between READY and the save: a Forget either tore the slot down first
  (the CAS fails, nothing is saved) or removes the key and clears the
  flag after the save; a link loss right at READY is handled after the
  save, so a new pairing is never left unsaved. `close()`, which zeroes
  `DeviceInfo`, cannot run while the hook reads the name. An empty name
  is never saved: it would make the slot's next saved-name connect a
  proximity connect. (The first cut saved before the CAS, so Forget
  during a handshake was undone; the second cut saved after the lock
  was released, so a link loss in that gap skipped the save of a new
  pairing and armed the slot for its previous sensor.)
- One sensor, two slots: after a successful connect,
  `disarmOtherSlotsWaitingFor` sets OFF on every other non-OFF slot
  whose `_open_device` is the connected name (bleWorker is its only
  writer). Otherwise a slot that lost sensor A, after A was connected
  to another slot, kept scanning for it and took it back as soon as
  the user forgot it there. A failed boot connect is not armed when its
  sensor is READY in another slot.
- Disconnect/forget vs. a queued attempt: OFF is stored before the
  teardown; bleWorker drops a `BACKGROUND` request whose slot is OFF
  after its REQUESTED → CONNECTING claim. A disconnect that lands later
  stores IDLE and the CONNECTING → READY CAS abandons the connect.
- User connect vs. an attempt that just failed: the worker sets IDLE
  then tries ADOPTED → ARMED; `requestUserConnect` tries ARMED →
  ADOPTED then enqueues. Usually this ends either with a queued
  proximity connect (the slot was IDLE) or with the user's ADOPTED
  attempt resolving and pushing T_DEV_LIST. Not every interleaving:
  if the user's IDLE → REQUESTED claim lands between the worker's IDLE
  store and its re-arm (or between the two steps of
  `tryArmFailedConnect`), the user's proximity connect runs on a slot
  that is ARMED. The host then sees the slot disconnected while it
  runs (ARMED masks the state in T_DEV_LIST) and a failure pushes no
  T_DEV_LIST; the host's own 12 s connect timeout returns its card to
  idle, and a success pushes `connected=true` as usual (and sets OFF).
  Harmless with today's host; a host that renders CONNECTING from the
  list would miss the spinner for that one attempt.
- `firstFreeSlot()` picks an ARMED slot only when no other slot is
  free, so a first-free connect does not take a waiting sensor's slot.

**Still open (P3, review of `3963cc5`).** A user Connect that lands
during a background scan waits up to the full 5 s scan (`abortScan()`
is a no-op), which can push it past the host's 12 s connect timeout.
With two slots READY and the third ARMED mid-attempt,
`firstFreeSlot()` returns -1 and the BOOT short-press logs "all slots
full". A stale queued `BACKGROUND` request can still win the claim of
a slot that was disconnected and re-claimed by a new Connect (the
deferred stale-request gap below).

**Measured on board B (2026-10-30)**, GDX-ACC `0H4008J2` in slot 0
(GoGo "Slot 1"), host = ir-share-v3 debug image:
- Debug build, sensor off: `slot 0: BLE link lost, slot freed,
  reconnecting in the background` at C3 356572 ms; the host logged
  `reported disconnected by T_DEV_LIST` 15 ms later and showed No
  sensor. Rescans started at +1.0, +8.0, +17.0, +30.0 s (device ms
  357572, 364574, 373576, 386578), each `scan complete, no match`
  after 5.0 s, then `next in 2000 / 4000 / 8000 / 16000 ms`. No
  T_DEV_LIST was pushed for the failed attempts (the host's only
  traffic was its own 10 s `C_DEV_LIST` probe).
- Sensor on: the next rescan (407580 ms, after the 16 s gap) found
  the peer 53 ms after it started; `streaming started` 1.79 s after
  the scan start, ~4.5 s after the coordinator relayed "on". The GoGo
  went back from the No sensor panel to the Slot 1 page with no key
  press (owner, by eye, ~15 s after on).
- Forget with the sensor on: `forgotten`, then no scan in 70 s.
- Release build: Connect pressed 12 s into the absence got
  `ok=1 msg=reconnecting` (TOOK_OVER); the attempt failed, the host's
  12 s connect timeout reset its card, the slot was re-armed, and when
  the sensor came back it reconnected with no key press (host probe
  stopped, Press opened the slot page instead of sending C_CONNECT).
- Not exercised on hardware: C_DISCONNECT (no host UI sends it), the
  BOOT long-press, two sensors at once, the QUEUED branch of a
  Connect between two rescans.

**Fix build on board B (2026-10-30, after the review fixes)**, same
sensor and host, no owner at the desk:
- C3 reset with the sensor on (debug): boot connect `scan start
  name="GDX-ACC 0H4008J2"` at 4798 ms, peer found 64 ms later,
  `streaming started` at 6591 ms, then `NVS slot 0: saving deviceName0
  = GDX-ACC 0H4008J2` at 6624 ms, i.e. after READY (the save moved
  behind the CONNECTING → READY CAS).
- Forget with the sensor on (debug): `forgotten`, host `reported
  disconnected by T_DEV_LIST` in the same 50 ms, then no scan in 65 s.
  Re-paired with Connect: `C_CONNECT: slot 0 queued`, streaming 6.8 s
  later, name saved.
- A host Connect on an empty slot with no free sensor around (USER
  origin) failed after the 5 s scan and left the slot OFF: no rescans.
- Release build: host `Vernier proto v2, fw=2 v2.3.0`; the boot
  auto-connect reconnected slot 0 and the GoGo opened its slot page.
- Not yet run (owner needed): the boot case (sensor off at C3 reset,
  on ~20 s later) and the give-up (checked with the constant shortened
  to 60 s in a scratch build only).

**Not covered.** A later `C_SET_PERIOD` still re-arms
`startAutoConnect` and so also reconnects a slot that was
C_DISCONNECTed (existing behaviour; the host UI has no Disconnect, only
Forget, which clears the NVS name). That pass is `USER`, so a failure
there does not arm the slot.

**Duplicate NVS keys (P3, accepted for v2.3.0).** When sensor A that
slot 0 lost is connected to slot 1, `disarmOtherSlotsWaitingFor` stops
slot 0's background search, but slot 0 keeps its NVS key
`deviceName0 = A` and its `slotHasSavedDevice` flag. Both slots then
"own" A. Effects:
- After the user forgets A on slot 1, slot 0's key still names A: the
  next `C_SET_PERIOD` pass or the next C3 boot reconnects A into slot 0,
  and with boot arming a boot that does not find A hunts it for up to
  `RECONNECT_GIVE_UP_MS` (10 min). The user forgot A and it comes back
  in another slot.
- At boot, while A is on, the lower slot wins it; the other slot's
  attempt fails and is not armed (`savedSensorReadyElsewhere`), but it
  costs one 5 s scan per boot.
Forget was already per-slot before v2.3.0, so this could happen then
too; v2.3.0 makes it more likely because the BOOT short-press now
steers a new pairing away from an ARMED slot. Fix if it bites: when A
is saved to one slot, remove A from the other slots' keys and clear
their flags inside the same `NvsScope`. Workaround today: when the
forgotten sensor shows up again in the other slot, Forget it there too.

### Bug #2 (since v2.0.0): T_HELLO per-connect is a host-reset storm

T_HELLO is the GLOBAL boot handshake; the host's T_HELLO handler
calls `_resetConnectionState()` (it reads T_HELLO as "peer
rebooted"). The old `connectAndReport` emitted T_HELLO on **every**
connect, so during a sequential 3-slot reconnect each new connect
wiped the previously-connected slots back to "connecting" — only the
last slot kept its fields, the earlier two recovered ~30 s later via
the periodic T_FIELDS re-emit. **Rule: T_HELLO + T_STATUS are sent
exactly once per session**, before the first per-slot frame (on the
first host command, or by bleWorker if a connect starts while the host
is still silent; see "Boot auto-connect choreography"), NEVER per
connect. `publishConnectResult`
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
   **Hostless fallback:** if nothing has started auto-connect by
   `AUTO_CONNECT_HOSTLESS_DELAY_MS` (5 s, `main.h`) after boot and a
   saved device exists, `autoConnectDevice` arms it itself. This
   covers the co-MCU booting after the GoGo's `setup()` wrote its boot
   commands, a co-MCU-only reset (sketch upload, USB replug, brownout)
   while the GoGo keeps running, and boot `C_SET_PERIOD`s that were
   all NACKed (the GoGo's 120 s preset exceeds the 16-bit limit). The
   GoGo answers its own boot commands about 1.2 s after its reset
   (bench, 2026-10-18), so the fallback does not fire in a normal
   shared power-up.
   **Handshake ordering (`HostLink` in `control-loop.cpp`):** `SILENT`
   → `HEARD` on the first host command (handshake sent), or `SILENT` →
   `ANNOUNCED` when bleWorker starts a connect while the host is still
   silent (`announceIfSilent` sends the handshake first, so the host
   never sees a per-slot frame before T_HELLO). `ANNOUNCED` → `HEARD`
   on the first host command afterwards repeats the handshake and
   re-sends T_DEVINFO / T_DEVSTATS / T_FIELDS for every READY slot plus
   T_DEV_LIST (the whole burst is skipped when that first command is
   `C_DEV_LIST`, whose handler sends the same list and frames): the
   host may have booted after the announce (it would
   otherwise wait up to 50 samples for the T_FIELDS re-emit), and if it
   was listening its T_HELLO reset is undone in the same burst.
   The hostless path streams at `VERNIER_DEFAULT_PERIOD_MS` (1 s) per
   slot, since the vernier does not persist periods; the GoGo's preset
   only comes back when it sends `C_SET_PERIOD` again (menu change,
   GoGo reboot, or a host that re-sends its boot sequence on T_HELLO).
4. Next loop tick, `autoConnectDevice` walks slots; for each with
   `slotHasSavedDevice[i]` it `enqueueConnect(i)` (CAS IDLE→REQUESTED;
   a duplicate pass after another `C_SET_PERIOD` re-arm is a no-op).
   bleWorker drains the queue, connecting one slot at a time. Requests
   of the first pass carry `ConnectOrigin::BOOT` (later passes `USER`):
   a saved sensor that pass does not find (switched off, out of range)
   is armed for the background reconnect (v2.3.0, see "Background
   reconnect after a link loss or a failed boot connect").
5. For each successful connect, `publishConnectResult(slot)` emits
   only the per-slot frames — T_DEVINFO / T_DEVSTATS / T_FIELDS —
   then T_DEV_LIST. **No T_HELLO/T_STATUS here**: emitting T_HELLO
   per connect made the host's T_HELLO handler
   (`_resetConnectionState`) wipe every already-connected slot back
   to "connecting" on each new slot's connect, so only the last slot
   of a multi-slot reconnect kept its fields. The handshake moved to
   step 2 (once) to fix this.

GoGo-only reboot: the GoGo comes back with an empty slot table while
this firmware keeps running with its host link already HEARD, so no
T_HELLO arrives on its own. This does NOT trigger `C_HELLO`: the GoGo's
co-MCU detector (gogo-firmware `CoProcDetector`) locks passively, at
about 1159 ms on the board, on the T_ACK / T_DEV_LIST answers to
setup()'s `C_SET_PERIOD` / `C_DEV_LIST`.

`C_HELLO=7` goes out only when the GoGo's boot commands get no answer:
the co-MCU booted late, was held in download mode, or its receiver lost
frame sync. On the board it was sent at 4166 ms with the C3 held in
download mode. `cmdHello` answers T_HELLO (only T_HELLO, no ACK, and not
a second time when C_HELLO was itself the first contact). The GoGo then
sends `C_DEV_LIST` as in the host-side note below. Before C_HELLO
existed the GoGo got `T_ACK "unknown command"` for it, which its
detector also treats as "Vernier present".

The detector then sends an Arduino-lib request (`54 FE 20 02 DC DC`)
and a Tasmota line (`\nStatus 10\n`) 300 ms apart, but only if no
co-MCU answered yet. If this firmware is slow to answer and those bytes
arrive, `FramedMsgPackReceiver` drops the half-read "frame" after
`kFrameGapResetMs` (100 ms) of line silence instead of skipping
`0x54FE` = 21758 bytes. Host-side check (throwaway shim harness,
2026-10-13): before the resync, a C_DEV_LIST sent 300 ms after the
Arduino or Tasmota probe was never dispatched; after it, it was; a
whole frame already buffered when the task stalls 1 s still parses.
Board result: the resync case confirmed on the board was a Logo
`tasmotasendcommand` writing `Status 10\r\n`. It recovered because the
quiet gap before setup()'s commands (the GoGo reboot) exceeded
`kFrameGapResetMs` (100 ms).

Host side (post-fix):
- The auto-detect-co-MCU path on the host **no longer** issues an
  unsolicited `C_CONNECT` (it used to). Instead it sends
  `C_DEV_LIST` so the host's slot table populates immediately.
  Vernier-firmware's saved-device auto-reconnect handles the
  pairing per-slot, and the host learns about it via the resulting
  T_DEV_LIST + per-slot frames. Avoids the host-vs-vernier slot
  race that lit up the wrong slot's CONNECTING animation.

**`C_DEV_LIST` also re-sends the slot frames.** After the ACK and the
`T_DEV_LIST` push, `cmdDevList` calls `resendReadySlotFrames()`:
`T_DEVINFO` + `T_DEVSTATS` + `T_FIELDS` for every READY slot (same
frames and order as `publishConnectResult`). Why: a host that restarts on
its own (the co-MCU keeps running, host link state stays HEARD, so no
boot handshake is repeated) learns "slot connected" from `T_DEV_LIST`
but would not get the field list until the next 50-sample `T_FIELDS`
re-emit (about 50 s at 1 s period; at 60 s period, about 50 min).
The host sends `C_DEV_LIST` on every boot and when it recovers a
silent slot, so no new command is needed. Event-driven `T_DEV_LIST`
pushes (connect, disconnect, forget, link loss) do not carry the frames.
Compatibility: all three are ordinary per-slot frames a host already
accepts at any time, so an older host just re-stores the same values; a
newer host with an older co-MCU simply keeps waiting for the periodic
re-emit as before. Cost: about 530 B per READY slot with 11 fields
(T_FIELDS ~395 B + T_DEVINFO ~75 B + T_DEVSTATS ~55 B) plus the ACK and
the T_DEV_LIST: about 0.7 KB (~60 ms at 115200) for one slot, ~1.8 KB
(~160 ms) for three.

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
- `loop()` → reads startAutoConnect, dispatches autoConnectDevice
  (and flips startAutoConnect itself for the 5 s hostless fallback).
- `uartHandler` + `bleWorker` → `hostLink` (`std::atomic<HostLink>`,
  relaxed: one variable, no data published with it).
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
- Submodule: `lib/GoGoVernier` (v1.0.0 plus the `isLinkUp()` and supervision-timeout-cap commits)
  carries the GDX driver including `refreshStatus()` battery support.
- Host-side display patterns:
  `gogo-firmware/.claude/knowledges/vernier-display-patterns.md`.
