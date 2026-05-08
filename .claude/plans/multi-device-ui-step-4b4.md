# Phase 4 step 4b.4 — host UI slot list + drill-down (D8 Option C)

Plan for the full multi-slot UI refactor on host side (`gogo-firmware`),
deferred from the 4b mini-batch (4b.1 + 4b.2 + 4b.3) because of
display-layer surface area.

Predecessors landed (4b.1–4b.3):
- T_DEV_LIST request fires at host boot → slot table populated early.
- `VERNIER_CYCLE` sub-menu item rotates `_primary_slot` to the next
  CONNECTED slot. Existing `vernierMonitor` renders whichever slot
  is primary; cycle button greyed when ≤1 slots connected.
- Existing `VERNIER_CONNECT` button calls `disconnect()` /
  `connect()` — already per-slot via `_primary_slot` default in
  the host adapter (4a.5).

What 4b.4 still owes per design D8 Option C: a dedicated **slot list
view** as the entry screen for the vernier page. Drill-down view =
the existing per-slot detail layout (currently the only view).

---

## User-visible flow after 4b.4 lands

```
┌─────────────────────────────┐    ┌─────────────────────────────┐
│ Vernier Sensor              │    │ Vernier Sensor              │
├─────────────────────────────┤    ├─────────────────────────────┤
│ ▶ Slot 1: GDX-LC            │    │ Slot 2: GDX-TMP             │
│   Light: 56.2 lux           │    │   Temperature: 23.4 °C      │
│ ─────────────────────────── │    │   Period: 1 s               │
│   Slot 2: GDX-TMP           │    │   Battery: 87%              │
│   Temperature: 23.4 °C      │    │   ────────────────────────  │
│ ─────────────────────────── │    │   Light: ...                │
│   Slot 3: Empty             │    │   ...                       │
│   Press to connect          │    │                             │
├─────────────────────────────┤    ├─────────────────────────────┤
│ [Connect][Period][Info][Cyc]│    │ [Disconnect][Period][⌫Back] │
└─────────────────────────────┘    └─────────────────────────────┘
   Slot list view (default)            Drill-down view
```

Cursor scrolls slot rows in list view. Press = enter drill-down on
selected slot. Drill-down "⌫Back" returns to list. Cycle button on
the drill-down view rotates the primary slot (= the slot rendered
in drill-down) without leaving drill-down — useful for quick
side-by-side comparison if the user keeps pressing it.

---

## Sub-step breakdown

Each row a separable commit. Build + smoke between each.

### 4b.4.1 — `DISPLAY_VERNIER_SLOT_LIST` mode + state machine

- New `display_id_t` value `DISPLAY_VERNIER_SLOT_LIST` alongside
  `DISPLAY_VERNIER_SENSOR`.
- New global flag `gblVernierInDrillDown` (or a new state machine
  variable) toggled by Press / Back.
- Main loop's `case DISPLAY_VERNIER_SENSOR` branches:
  - drill-down mode → `vernierMonitor` + `vernierControlMenu`
    as today.
  - list mode → new `vernierSlotListView()`.
- Long-press button on list row = enter drill-down. Long-press on
  drill-down = back to list. (Or: short-press = press, long-press
  = back. Discuss.)

### 4b.4.2 — `vernierSlotListView()` render function

Layout (in `src/display/gogo-display.cpp`):

```
y=22  ┌─────────────────────────────┐
      │ ▶ Slot 1: GDX-LC            │  highlighted (cursor)
      │   Light: 56.2 lux           │
      │ Slot 2: GDX-TMP             │
      │   Temperature: 23.4 °C      │
      │ Slot 3: Empty               │
      │   Press connect to add      │
y=110 └─────────────────────────────┘
```

Row layout:
- Row height ~30 px (= 3 slots fit in ~90 px content area).
- First line: slot id + device name (or "Empty").
- Second line: first sensor value + unit (or hint text).
- Cursor row gets `▶` prefix + slight bg highlight.

Inputs:
- `VernierSlotListData` snapshot struct (new) — N entries each
  with `connected`, `name`, `first_field_name`, `first_field_unit`,
  `first_field_value`. Populated by new
  `GoGoVernier::snapshotSlotList(out)` accessor that walks
  `_slots[]` and fills.

### 4b.4.3 — Cursor navigation in list view

- Repurpose existing rotary / button up-down events to scroll
  through `0..VERNIER_MAX_SLOTS-1` rows when in list mode.
- New `_currentSlotListCursor` member on `GoGoDisplay` (analog to
  `_currentDisplayMenu`).
- Press → `setPrimarySlot(_currentSlotListCursor)` then enter
  drill-down.

### 4b.4.4 — Back button on drill-down

- Drill-down's button menu (`VERNIER_CONNECT / PERIOD / INFO /
  CYCLE`) gains a 5th cursor `VERNIER_BACK`. Press = exit
  drill-down → list view.
- Bumps `VERNIER_MODE_COUNT` to 5.

### 4b.4.5 — List view default on entering vernier page

- When user navigates into the vernier page (from main menu), land
  on slot list view, not drill-down. Drill-down only on explicit
  press from list.
- Initial state save / restore: NVS `vernierLastView` (list vs
  drill-down + last cursor row) so the screen resumes where the
  user left it. Optional polish — could skip and always default
  to list.

### 4b.4.6 — Empty-slot row "Press to connect" semantics

- When cursor is on an Empty row and user presses, send
  `gogoVernier.connect()`. Vernier still picks first-free slot,
  which won't necessarily be the row the user pressed (if multiple
  empty slots). For v1 of the UI: just trigger connect, let the
  result land wherever vernier puts it. Highlight the resulting
  slot when its T_DEV_LIST update arrives.

### 4b.4.7 — Per-slot period control in drill-down

- Existing `VERNIER_PERIOD` editor mutates `_vernierSettings[VERNIER_PERIOD_PRESET]`
  globally. Per design D5 (per-slot period), it should mutate a
  per-slot value.
- Add `_vernierPeriodPresetPerSlot[VERNIER_MAX_SLOTS]` array.
  `setPeriodPreset(idx, dev)` writes to slot dev. UI in drill-down
  reads/writes the displayed slot's preset.
- NVS schema bump: `vernierPeriodPreset0..N` keys (mirror of
  step 3.7's `deviceName0..N`). Migrate `vernierPeriodPreset`
  legacy key to slot 0.

---

## Open decisions to revisit before coding 4b.4

| # | Question | Default proposal |
|---|----------|------------------|
| Q1 | Up-down navigation in list view: rotary encoder, two-button (next/prev), or single-button cycle? | Match existing menu navigation. Likely rotary on this device. |
| Q2 | "Press" vs "Long-press" for enter drill-down? | Short = enter; long = quick disconnect of highlighted slot. |
| Q3 | Empty-slot row: prompt "Press to connect" or skip rendering empty slots? | Render with hint — affordance for users to know slot exists. |
| Q4 | Drill-down BACK button: cursor item or hardware back button if available? | Add as 5th cursor item (4b.4.4). |
| Q5 | Per-slot period preset: per-slot UI (4b.4.7 in scope) or stay global? | Per-slot per ratified D5. Add to 4b.4.7. |
| Q6 | NVS migration for 4b.4.7 period schema | Same one-shot pattern as step 3.7's deviceName. |
| Q7 | Default sub-menu cursor on entering drill-down: CONNECT, CYCLE, or BACK? | CONNECT (matches today's default + most common action). |
| Q8 | Display memory budget for VernierSlotListData snapshot | One copy: ~100 B per slot × 3 = 300 B static. Same display task as today, no extra task contention. |

---

## Estimated scope

- ~5–7 commits (one per sub-step above, plus a docs commit).
- ~1 dedicated session (2–4 hours of focused display-layer work
  + iteration).
- Most risk concentrated in 4b.4.1 (state machine refactor) and
  4b.4.7 (per-slot period + NVS migration).

---

## Smoke test plan after 4b.4

Single device:
- Boot → list view shows 1 occupied row + N-1 empty rows.
- Press connected row → drill-down opens with values.
- Back → returns to list.
- Existing CONNECT/PERIOD buttons still work in drill-down.
- No regression vs 4b.2 behaviour.

Multi-device (requires step 5 hardware):
- Boot with 2 saved devices → both auto-connect → list shows both.
- Press row 0 → drill-down on slot 0. Back. Press row 1 →
  drill-down on slot 1. Values flow independently per slot.
- Per-slot period: change slot 0 to 100 ms in drill-down, slot 1
  stays at 1 s. Verify wire commands (T_ACK with dev field).
- Disconnect slot 0 in drill-down → list shows slot 0 empty,
  slot 1 still streaming.
- Empty-slot connect → first-free allocator places it; if both
  slots free, lands on slot 0.

---

## When to start

Pick up 4b.4 after step 5 hardware smoke ratifies the multi-slot
backend works on real BLE peers. Doing 4b.4 first risks UI work
against backend bugs. Hardware-first uncovers issues in the
proven path before fancy UI lands on top.

Sequence: 4b.1 (✅ done) → 4b.2 (✅ done) → step 5 (multi-device
hardware smoke) → 4b.4.
