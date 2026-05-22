# vernier-firmware 2.1.0 — non-blocking BLE connect + hygiene sweep

No 2.0.1 patch release. Everything from the 2.0.0 code-review
backlog folds into 2.1.0:

- **Centrepiece (§Design–§Phases)** — take `dev.connect()` off the
  `uartHandler` task so host commands stay responsive during the
  ~7 s BLE scan + GATT handshake.
- **Hygiene sweep (§Hygiene)** — six small correctness / observability
  patches captured during the 2.0.0 review.
- **Documentation refresh (§Docs)** — audit CLAUDE.md and the
  `.claude/knowledges/` files against the post-2.0.0 codebase.

This is a planning doc, not a spec. All four design questions
(§Decisions) were ratified before phase 3 — see that section for
the call and the reasoning kept against each one.

## Problem

Today three sites can drive a connect:

| Caller            | Task / context     | Blocking impact                          |
|-------------------|--------------------|------------------------------------------|
| `cmdConnect`      | `uartHandler` task | **Stalls host-frame polling for ~7 s.**  |
| `buttonHandler`   | `loop()`           | OK — loop already off the host-RX path.  |
| `autoConnectDevice` | `loop()`         | OK — runs once at boot via `C_SET_PERIOD`. |

The `cmdConnect` path is the only one with user-visible damage: while
the UART task is parked inside NimBLE's scan + GATT discovery, no
host frame is parsed. The RX FIFO buffers ~1 KB of pending commands
which then all dispatch in a burst once the connect resolves — the
host perceives a stutter and (depending on UI) may have already
re-sent a now-redundant command.

The deferred TODO comment lives in `src/main.cpp:362` (the
`pendingConnect` flag pattern).

## Goal

- `cmdConnect` returns within a few ms; host commands keep flowing
  for the duration of the BLE pairing.
- All three connect sites (UART, button, auto) funnel through one
  code path so the "single producer of `dev.connect()`" invariant is
  enforceable.
- Sample stream from already-connected slots is **not** delayed by a
  new slot's pairing.

## Non-goals (defer to 3.0.0 or later)

- Parallel multi-slot BLE connect (the NimBLE controller can't anyway).
- NVS v1 legacy-key cleanup (v3-era concern).
- Full FreeRTOS task graph re-think.

## Design

### State machine

Per-slot connection state, declared on `VernierAdapter` and exposed
read-only:

```
IDLE  →  REQUESTED  →  CONNECTING  →  READY
                                  ↘
                                   FAILED  →  IDLE   (timeout / NACK)
```

- **IDLE**: no live BLE link, no pending request.
- **REQUESTED**: worker hasn't picked the slot up yet (queued).
- **CONNECTING**: worker is inside `dev.connect()` / handshake.
- **READY**: same gate as today's `isReady()` — D2PIO handshake done.
- **FAILED**: transient, single TX of NACK, then drop back to IDLE.

State variable is `std::atomic<ConnState>` on `VernierAdapter`
(landed in G004 / commit `2fca27d`). Memory ordering: `relaxed` on
read/write because transitions are independent scalar publishes —
no fill-then-flag pattern, no other adapter field needs to be
visible-before this write.

### Worker task

New FreeRTOS task `bleWorker`:

- Priority: between `uartHandler` and `vernierHandler` (so it
  preempts the idle sampler but not the host-protocol loop).
- Core affinity: same as `vernierHandler` (BLE stack runs on ARDUINO
  RUN core; keep all radio-touching work pinned there).
- Body: blocks on a FreeRTOS queue of `ConnectRequest{slot, force,
  req_seq}`. For each item: flip state to CONNECTING, call
  `dev.connect()`, run the existing publish-result side-effects,
  flip to READY/IDLE.
- Queue shape: depth = `VERNIER_MAX_SLOTS` (3 today). A C_CONNECT
  for a slot already in REQUESTED / CONNECTING returns NACK with
  `msg="slot busy"` instead of coalescing (per §Decisions Q1).
- One worker for all slots, not one-per-slot (per §Decisions Q2).
  The BLE controller serialises connects anyway and a single task
  with a queue is simpler to reason about than three with
  coordination.

### `cmdConnect` becomes enqueue-only

```cpp
static void cmdConnect(JsonVariantConst root) {
    uint8_t slot = pickSlot(root); // existing logic
    if (slots[slot].state() != IDLE && slots[slot].state() != FAILED) {
        uart.sendAck(req_seq, false, "slot busy", slot);
        return;
    }
    slots[slot].setState(REQUESTED);
    enqueueConnect({slot, force, req_seq});
    uart.sendAck(req_seq, true, "queued", slot);   // early-ACK
}
```

Side-effects (`sendHello`, `sendStatus`, `sendDeviceInfo`,
`sendDeviceFields`, `sendDeviceStats`, NVS save, `emitDevList`) move
into a new `publishConnectResult(slot, ok, req_seq)` helper invoked
by the worker after `dev.connect()` returns.

### `buttonHandler` and `autoConnectDevice` re-routed

Both also call `enqueueConnect(...)` instead of `connectAndReport`
directly. Uniform path means only `bleWorker` ever calls
`dev.connect()`, which lets us drop the "GDXLib internal
serialization" assumption flagged in the 2.0.0 code review.

### Wire protocol additions (non-breaking, no `proto_version` bump)

Two additive changes, both optional keys — hosts that don't read
them keep working:

1. **Early-ACK semantics**. `T_ACK` for `C_CONNECT` returns
   immediately with `ok=true, msg="queued"`. A second `T_ACK` is
   **not** emitted on completion (avoids breaking hosts that
   correlate one ACK per command). Connection settled-ness is
   reported via the existing `T_DEV_LIST` / `T_DEVINFO` push.
2. **`state` field in `T_DEV_LIST` entries**. Today `connected: bool`
   collapses CONNECTING and IDLE into the same value. Add
   `state: u8` carrying the enum so the host can render a
   CONNECTING spinner on the kid-UX slot card without inferring it
   from the absence of T_DEVINFO.

Per §Decisions Q3, transitions piggyback on the existing
auto-pushed `T_DEV_LIST` — no standalone `T_CONN_STATE` frame is
introduced. `T_DEV_LIST` is already emitted at the end of
`publishConnectResult` (D4), so every state change naturally rides
out on the next slot-table push. Keeps the wire surface narrow.

### Failure / timeout handling

- `dev.connect()` already has an internal timeout (~10 s in
  GoGoVernier). Worker treats a `false` return as FAILED.
- Worker task crash supervision: defer (no current watchdog
  infrastructure). Note for 2.2.0.
- If the host sends `C_DISCONNECT` while a slot is REQUESTED, the
  enqueued request is discarded; if CONNECTING, the disconnect waits
  in line behind the in-progress connect (BLE controller can't
  cancel an in-flight scan cleanly).

## Implementation phases

| Phase | Scope | Risk |
|-------|-------|------|
| **1. Extract** | Carve `publishConnectResult(slot, ok, req_seq)` out of `connectAndReport` so the side-effects can be invoked from a different task. Pure refactor, no behaviour change. | Low. Test by smoke. |
| **2. State machine** | Add `ConnState` enum + `std::atomic<ConnState>` on `VernierAdapter`. Initialise to IDLE. Existing `isReady()` becomes `state() == READY`. | Low. |
| **3. Worker** | New `bleWorker` task + `ConnectRequest` queue. Worker calls `dev.connect()` and `publishConnectResult` on dequeue. | Medium — priority + affinity to validate on real HW. |
| **4. Cutover** | `cmdConnect` switches to enqueue + early-ACK. `buttonHandler` and `autoConnectDevice` re-routed. `connectAndReport` retired. | Medium — host-side UX needs to handle "ACK before result". |
| **5. Protocol additive** | `state: u8` added to `T_DEV_LIST` entries. Document in CLAUDE.md. | Low if host is in lockstep. |
| **6. HW validation** | Real GDX-FOR + GDX-TMP + GDX-LIGHT in parallel — pair slot 1 while slot 0 is streaming, confirm slot 0 sample rate stays steady. Stress 3-slot concurrent C_CONNECT. | Buys the confidence to ship. |

Phases 1–2 can land on `develop` independently. 3–4 land together on
a feature branch. 5 follows after host-side support is ready. 6 gates
the tag.

## Hygiene + correctness sweep {#hygiene}

Six small patches lifted straight from the 2.0.0 code review's
post-ship list. Each is independently testable, independently
shippable, and lands on `develop` ahead of (or in parallel with)
the connect refactor.

| # | Patch | Where | Effort |
|---|-------|-------|--------|
| H1 | `static_assert(VERNIER_MAX_SLOTS <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS)` so a future slot bump can't silently overflow NimBLE. | `include/uart-adapter.h` | 1 line |
| H2 | Promote `VernierAdapter::_period_ms` and `_push_dropped` from `volatile` to `std::atomic<...>` with `memory_order_relaxed` on the hot paths. Drops the explicit load/store sidestep of `-Wdeprecated-volatile`. | `include/vernier-adapter.h`, `src/vernier-adapter.cpp` | ~10 lines |
| H3 | `FramedMsgPackReceiver::poll` logs `deserializeMsgPack` errors at `log_w` instead of silently dropping. Wire-corruption is observable in the field. | `include/framed-msgpack-receiver.h` | ~2 lines |
| H4 | `cmdSetPeriod` clamps `period_ms` to `[VERNIER_MIN_PERIOD_MS, ...]` instead of accepting any non-zero value. A misbehaving host sending `period_ms=1` currently spins GDXLib start/stop. | `src/control-loop.cpp` | ~5 lines |
| H5 | `serial_log_vprintf` preserves the trailing newline on truncated lines so long NimBLE format strings don't run together. | `src/main.cpp` | ~3 lines |
| H6 | Per-slot mutex inside `VernierAdapter` around `connect/disconnect/getDeviceInfo` so the cached status struct can't be torn-read while the 10 s status refresh runs concurrent with a host-initiated connect. Folds naturally into the bleWorker design — `bleWorker` becomes the only writer, vernierHandler the only reader, and the mutex documents the contract. | `include/vernier-adapter.h`, `src/vernier-adapter.cpp` | ~15 lines |

H1–H5 are safe to ship as standalone commits on `develop` at any
point. **H6 ships with phase 3 of the connect refactor** because the
locking discipline only makes sense once `bleWorker` exists.

The atomic promotion in H2 obviates the `volatile`-plus-barrier
pattern for those two fields specifically; the rest of the cross-task
flags (`startAutoConnect`, `foundSavedDevice`, `slotHasSavedDevice[]`)
stay `volatile` for now — see the §Out-of-scope note below.

## Documentation refresh {#docs}

The 2.0.0 cleanup pass moved code around (control-loop.cpp,
slot-helpers.cpp split out of main.cpp) and lifted protocol-layer
magic numbers into named constants. Some prose has drifted.

Targets:
- `CLAUDE.md` — verify the "Runtime layout" file list, the wire-
  protocol enum block, and the dependency list match HEAD. Confirm
  the "Code Quality Rules" section reflects the cross-task barrier
  pattern that's now used uniformly across vernier-firmware +
  GoGoVernier.
- `.claude/knowledges/vernier-mcu-internals.md` — sanity-check
  references to function names, file paths, and any line-number
  pointers. The MCU-internals snapshot was last refreshed before
  the main.cpp split.
- `.claude/knowledges/d2pio-debug-findings.md` — confirm symbol /
  constant names still match `lib/GoGoVernier/src/D2PIOProtocol.h`
  after the constant-lifting pass at `ec0adeb`.
- `.claude/specs/d2pio-protocol.md` — cross-check offset / opcode
  tables against the new header constants in D2PIOProtocol.h.

Scope is "fix what's stale", not "rewrite". One commit per file is
fine. Drop or archive any paragraphs that reference long-retired
internals (e.g. `g_active_impl`, `GDXLib`, the old polling sample
path).

## Pairing add-ons

- **`C_CANCEL_CONNECT`** (in scope per §Decisions Q4) — host abort
  of a REQUESTED slot (e.g. kid pressed the wrong card). Worker
  discards the queued item, flips the slot back to IDLE, and emits
  NACK with `msg="cancelled"` echoing the cancelled C_CONNECT's
  `req`. Wire enum value: `C_CANCEL_CONNECT = 6` — appended at
  the end of `UartAdapter::CmdType` per the wire-numbering
  discipline. Lands inside G007's cutover commit (same TU, same
  diff) since it's effectively the cancel path of the same state
  machine.
- **`T_DEVSTATS` carries `state`** (optional, not committed) —
  same enum, same idea as the T_DEV_LIST addition, for hosts that
  subscribe to per-slot stats but not the whole slot table. Cost:
  trivial. Skip unless a host-side need surfaces during G009 HW
  validation; T_DEV_LIST piggyback already covers every state
  transition.

## Out of scope for 2.1.0 (recorded for 2.2.0 backlog)

- Watchdog supervision of `bleWorker`.
- Cooperative cancel during in-flight NimBLE scan (controller
  constraint — needs a NimBLE upstream feature).
- Replacing the `volatile` cross-task flags with `std::atomic` on
  the connect-related globals (`startAutoConnect`,
  `foundSavedDevice`, `slotHasSavedDevice[]`). H2 only handles the
  two `VernierAdapter` scalars where the change is purely local;
  sweeping the globals touches every publish site and earns a
  separate design pass.

## Risks

- **Host stuck on stale CONNECTING state** if a `T_DEV_LIST`
  transition push gets dropped. Mitigation: host should treat
  CONNECTING as a soft state with its own timeout, fall back to
  IDLE if no transition arrives within ~10 s.
- **Sample-stream backpressure when bleWorker preempts
  vernierHandler**. Mitigation: keep bleWorker priority strictly
  below vernierHandler; benchmark slot-0 throughput during slot-1
  pairing in phase 6.
- **Host build assumption mismatch** if host is shipped before the
  vernier-side cutover. Mitigation: `state` field is optional; host
  must keep reading the `connected: bool` until it learns it's
  talking to a 2.1.0+ co-MCU (check via `T_HELLO`'s version
  fields).

## Acceptance criteria

- During a `C_CONNECT` in CONNECTING state on slot N:
  - A `C_DEV_LIST` from the host returns within 50 ms (vs ~7 s today).
  - A `C_SET_PERIOD` on slot M (≠ N) returns within 50 ms and that
    slot's sample stream stays in cadence.
- 3-way concurrent `C_CONNECT` resolves sequentially without lost
  ACKs.
- `version-2.1.0` builds clean in both `release` and `debug` envs.
- HW smoke pass on real GDX gear (re-use `.claude/plans/step-5-hardware-smoke.md`).

## Decisions

Resolved in G005 before phase 3 implementation began. Each entry
keeps the question + the call + the reasoning so a future reader
can re-litigate without re-deriving the trade-off.

1. **Q1 — Worker queue: reject-on-busy or coalesce?**
   **Decision: reject** with `T_ACK ok=false, msg="slot busy",
   dev=<slot>`. Coalescing two C_CONNECTs onto the same slot
   would hide host bugs (e.g. spurious double-fire from the kid-UX
   button-debounce path) instead of surfacing them. The queue
   itself is sized at `VERNIER_MAX_SLOTS` (3) so the only way to
   hit "busy" is a real overlap on one slot, which the host UI
   should be debouncing anyway.

2. **Q2 — Single `bleWorker` task or one per slot?**
   **Decision: single.** The NimBLE controller serialises BLE
   scan + GATT discovery globally — three workers would block on
   each other anyway. One task + one queue is simpler to reason
   about, cheaper on RAM (one TCB + one ~4 KB stack vs three),
   and matches the existing pattern of one `vernierHandler` task
   round-robining across slots.

3. **Q3 — Standalone `T_CONN_STATE` frame or piggyback on
   `T_DEV_LIST`?** **Decision: piggyback.** `publishConnectResult`
   already emits `T_DEV_LIST` at the tail of every connect attempt
   (D4); adding the new `state` field to its entries means every
   transition naturally rides out without a second frame type to
   maintain. Keeps `UartAdapter::MsgType` narrow and avoids
   bumping the protocol version.

4. **Q4 — `C_CANCEL_CONNECT` in 2.1.0 or 2.2.0?**
   **Decision: in 2.1.0.** Closes the "kid pressed the wrong
   card" UX wart that auto-detect-fallback would otherwise eat.
   Implementation cost is small (~30 lines: enum entry, handler
   path, worker drains the queued request, ACK). Lands inside
   G007's cutover commit because it lives on the same state
   machine.
