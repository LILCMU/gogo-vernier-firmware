# vernier-firmware v2.1.0 hardware smoke addendum

Focused validation for the v2.1.0 deltas (non-blocking BLE connect via
`bleWorker`, `C_CANCEL_CONNECT`, `state:u8` on `T_DEV_LIST`, H1–H6
hygiene sweep). This is an **addendum** to
`.claude/plans/step-5-hardware-smoke.md` (the Phase 4 multi-device
backend plan) — sections A–G of that doc remain the regression
baseline and must still pass on the 2.1.0 firmware before this
addendum's checks are run.

Scope: only the v2.1.0 wire- and timing-visible changes. The
build-clean acceptance for `pio run -e release` and `pio run -e debug`
is in section H0 below.

---

## Hardware required

Identical to step-5-hardware-smoke.md §"Hardware required":
- 1 × GoGo Board v7 host
- 1 × Vernier co-MCU flashed with `vernier-firmware @
  feature/2.1.0-cycle` (or `version-2.1.0` once tagged)
- 3 × GDX sensors (any mix of GDX-LC, GDX-TMP, GDX-EA, GDX-3MG)
- A wire sniffer is helpful but not required — vernier USB-CDC log
  + host MCU log are sufficient to verify timing.

Optional:
- A 4th GDX for the "all slots full" / "queue full" coverage.

---

## Pre-test setup

1. Run step-5-hardware-smoke.md §"Pre-test setup" verbatim.
2. Confirm firmware identity: vernier USB-CDC `T_HELLO` log line shows
   `version_major=2, version_minor=1, version_patch=0`.
3. Confirm wire: capture one `T_DEV_LIST` frame after auto-connect
   settles. Each entry must carry a `state` key (numeric 0..4). If
   the key is absent, the build is pre-G008 — abort and re-flash.

---

## Test cases

### H0 — Build cleanliness (no hardware required)

| # | Action | Expected |
|---|--------|----------|
| H0.1 | `pio run -e release` | SUCCESS. Single binary in `dist/gogo-co-firmware-vernier.factory.bin`. |
| H0.2 | `pio run -e debug` | SUCCESS. Single binary in `dist/gogo-co-firmware-vernier-debug.factory.bin`. |
| H0.3 | `pio check -e check_medium_or_high_defects` | No medium/high cppcheck findings introduced by the v2.1.0 cycle. |

H0 gates everything below — a build regression invalidates the test.

### H1 — Non-blocking C_CONNECT (centerpiece)

| # | Action | Expected |
|---|--------|----------|
| H1.1 | Cold boot vernier with one saved sensor (slot 0). Host sends `C_DEV_LIST` 100 ms after the vernier announces `T_HELLO`. Time the ACK round-trip on the host side. | T_ACK for `C_DEV_LIST` arrives within **50 ms** of send, regardless of whether auto-connect is in progress. (Pre-2.1.0 this stalled up to 7 s during the BLE handshake.) |
| H1.2 | Repeat H1.1 with `C_SET_PERIOD` on slot 2 (a different slot than the one auto-connecting). | ACK within 50 ms. Sample stream on already-connected slots 1 + 2 must not drop a sample during slot 0's handshake. |
| H1.3 | Host sends `C_CONNECT` (no `dev`) while another slot is mid-handshake. | Early-ACK `ok=true msg="queued" dev=<slot>` arrives within 50 ms. Slot transitions to CONNECTING in the next `T_DEV_LIST` push. |

Failure mode: any ACK >100 ms during a concurrent CONNECTING window
means the bleWorker handoff isn't taking effect — re-check that
`cmdConnect` truly enqueues rather than calling `connectAndReport`.

### H2 — C_CANCEL_CONNECT happy path

| # | Action | Expected |
|---|--------|----------|
| H2.1 | Power on vernier with slot 1's saved sensor OFF. Host sends `C_CONNECT dev=1` (will pair via scan). Immediately (within ~100 ms) send `C_CANCEL_CONNECT dev=1`. | First C_CONNECT early-ACKs `ok=true msg="queued"`. Cancel ACKs `ok=true msg="cancelled"`. Slot 1 returns to state IDLE in the next `T_DEV_LIST`. bleWorker logs `slot 1 cancelled, skipping connect`. No T_DEVINFO/T_FIELDS for slot 1. |
| H2.2 | Host sends `C_CANCEL_CONNECT dev=1` on an IDLE slot. | NACK `ok=false msg="not pending" dev=1`. |
| H2.3 | After H2.1, host sends C_CONNECT for slot 1 again. | Should accept (slot is back to IDLE). |

### H3 — C_CANCEL_CONNECT race vs worker

| # | Action | Expected |
|---|--------|----------|
| H3.1 | Send `C_CONNECT dev=2` followed by `C_CANCEL_CONNECT dev=2` as fast as the host can issue them (back-to-back wire frames, no inter-frame delay). Repeat ~10 times. | Each pair resolves deterministically — either: (a) cancel wins → slot to IDLE, worker skips; (b) worker wins → slot transitions CONNECTING then READY/IDLE, cancel NACKs `not pending`. No state leak (no slot stuck in REQUESTED/CONNECTING after the dust settles). Pass criterion: the slot is IDLE or READY at the end of every pair. |

H3 stresses the `tryAcquireConnecting` / `tryCancelConnect` CAS. A
slot stuck in REQUESTED at the end of a pair means a race window
the CAS didn't close.

### H4 — `state:u8` wire visibility

| # | Action | Expected |
|---|--------|----------|
| H4.1 | Capture a `T_DEV_LIST` frame at each transition during H1.3: IDLE → REQUESTED → CONNECTING → READY. Decode MsgPack. | Each entry has a `state` key with the matching ConnState numeric value (0/1/2/3). `connected` is true iff `state == 3`. |
| H4.2 | Capture `T_DEV_LIST` after a failed connect (turn the GDX off mid-handshake — sensor disappears). | Slot transitions straight to `state=0` (IDLE) — there is no distinct FAILED state on the wire (the ConnState enum tops out at 3=READY). The host distinguishes "connect failed" from "connect succeeded" by whether it ever saw `state=3` before the return to `state=0`. |

H4.2 confirms the reserved-but-unreached note in the enum docstring is
truthful at runtime.

### H5 — 3-way concurrent C_CONNECT

| # | Action | Expected |
|---|--------|----------|
| H5.1 | Power on 3 GDX sensors. Host issues `C_CONNECT dev=0`, `C_CONNECT dev=1`, `C_CONNECT dev=2` within a single 20 ms window. | All three early-ACK `queued` within 50 ms. bleWorker processes them sequentially (slot 0 first, then 1, then 2 — see worker log). Each takes ~5–7 s of BLE handshake; total settle time ~15–21 s. Each transitions IDLE→REQUESTED→CONNECTING→READY in T_DEV_LIST pushes. No lost ACKs, no dropped enqueue (xQueueSend never returns false at queue depth 3). |
| H5.2 | While H5.1 is still running (slot 2 still CONNECTING), host sends `C_DEV_LIST`. | ACK + push within 50 ms, contents show slot 0 = READY, slot 1 = READY, slot 2 = CONNECTING. |

### H6 — Sample-stream stability during a new slot's CONNECTING

| # | Action | Expected |
|---|--------|----------|
| H6.1 | Slot 0 streaming at 100 ms (10 Hz). Pair a new sensor on slot 1 via `C_CONNECT`. Time the sample interval on slot 0 across the ~7 s slot-1 handshake. | Slot 0 inter-sample interval stays within **±10%** of 100 ms throughout. No gap >250 ms. |
| H6.2 | All three slots streaming at 100 ms. Issue `C_CONNECT dev=<any>` (will NACK busy). | The NACK ACK arrives within 50 ms. No sample dropped on any of the three streaming slots in the 200 ms surrounding the NACK. |

### H7 — H1–H6 hygiene sweep regression

These come from the v2.1.0 hygiene patches. They're already
build-guarded but worth a runtime spot-check.

| # | Action | Expected |
|---|--------|----------|
| H7.1 | (H4) Host sends `C_SET_PERIOD dev=0 period_ms=5` (sub-min). | NACK `ok=false msg="period below min" dev=0`. Slot 0 sample period unchanged. |
| H7.2 | (H3) Host sends a malformed MsgPack frame (e.g. truncated map). | Vernier USB-CDC log shows `FramedMsgPackReceiver: deserializeMsgPack failed (...)`. No host-protocol stall on the next valid frame. |
| H7.3 | (H5) Trigger a long NimBLE log line during BLE scan — verify USB-CDC output preserves the trailing newline after truncation. | Long log lines never run together; each gets a `\n` even at the 256 B truncation boundary. |
| H7.4 | (H6) Cold boot + auto-connect, then read `T_DEVSTATS` for any slot. | battery / charge / rssi triple is internally consistent (e.g. not "100% / charging / -120 dBm" — a torn read pattern). Repeat 20 times across the next 5 minutes; no inconsistent triple. |

---

## Failure modes specific to v2.1.0

1. **bleWorker task crash.** If the worker dies, the queue stops
   draining and every subsequent `C_CONNECT` early-ACKs `queued` but
   never resolves. Symptom: T_DEV_LIST shows the slot stuck at
   REQUESTED forever. Watchdog supervision is in §"Out of scope for
   2.1.0" of the plan; if this fires, capture the panic dump.
2. **Cancel-race state leak.** H3 stresses the CAS pair. If a slot
   shows up stuck in REQUESTED or CONNECTING after H3 settles,
   re-examine `tryAcquireConnecting` and `tryCancelConnect`.
3. **state field absent in T_DEV_LIST.** Means the build is pre-G008
   or `emitDevList`'s `entries[i].state` assignment regressed.
4. **Sample-stream backpressure.** If H6 shows slot 0 inter-sample
   gaps >250 ms during a new slot's CONNECTING, bleWorker is
   starving vernierHandler — re-check `HANDLER_TASK_PRIO` is
   shared (all three handlers at the same prio).
5. **C_CANCEL_CONNECT during CONNECTING.** Plan says cancel after
   CAS-into-CONNECTING returns "not pending" — verify the message,
   not just `ok=false` (helps the host distinguish "too late" from
   "invalid slot").
6. **NimBLE close()-during-scan crash.** buttonHandler long-press
   while a slot is mid-CONNECTING dispatches `dev.disconnect()` on
   uartHandler while bleWorker is inside `dev.connect()`. The plan
   §Risks notes this is best-effort — GoGoVernier's `session_mutex`
   should serialise, but watch for any NimBLE panic / assert.

---

## Sign-off criteria

v2.1.0 hardware smoke passes when:

- [ ] step-5-hardware-smoke.md sections A–G still pass on this
      firmware (regression baseline).
- [ ] H0–H7 above all green or with documented deviations.
- [ ] No reboots, NimBLE asserts, or watchdog resets across the
      session (target uptime: 60 minutes).
- [ ] Acceptance criteria from `.claude/plans/release-2.1.0.md`
      §"Acceptance criteria" verified:
  - [ ] C_DEV_LIST + C_SET_PERIOD return <50 ms during CONNECTING.
  - [ ] 3-way concurrent C_CONNECT resolves sequentially.
  - [ ] Builds clean in both release + debug envs.

If any check fails, capture: serial log from both MCUs, the
T_DEV_LIST capture for H4, and the test case ID. File under
`.claude/notes/<topic>.md` or attach to a bug ticket before
deciding whether to block the v2.1.0 tag.

---

## Time budget

Estimated 90 min including setup, the original step-5 regression
sweep, and the H0–H7 addendum. Plan for a 2-hour slot.

---

## After v2.1.0 smoke

If green: G010 release-cut may proceed — tag `version-2.1.0`,
build factory.bin, bundle via release-comcu skill, push.

If red: triage by severity per the §"Failure modes" list. Wire-
protocol regressions block the tag; UX / log nits can ship as
2.1.1.
