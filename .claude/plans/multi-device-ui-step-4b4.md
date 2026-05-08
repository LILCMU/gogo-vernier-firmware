# Phase 4 step 4b.4 — host UI multi-slot redesign (kids-UX, v2)

Plan for the multi-slot UI on host side (`gogo-firmware`), deferred from
the 4b mini-batch (4b.1 + 4b.2 + 4b.3) because of display-layer surface
area.

This document is **v2** — the original v1 plan (D8 Option C: list +
drill-down) was reviewed under a kids-UX lens and replaced. v1 design
intent recap is preserved at the end of this file under "Design history"
for traceability.

Predecessors landed (4b.1–4b.3):
- T_DEV_LIST request fires at host boot → slot table populated early.
- `VERNIER_CYCLE` sub-menu item rotates `_primary_slot` to the next
  CONNECTED slot.
- Existing `VERNIER_CONNECT` button calls `disconnect()` /
  `connect()` per-slot via `_primary_slot` default in the host
  adapter (4a.5).

These will be **removed / re-purposed** in the v2 redesign — see
sub-step 4b.4.0 cleanup.

---

## Design principles (kids-UX)

1. **No modes.** Single main screen. All 3 slots always visible.
2. **Color = identity.** Slot 1 = red, slot 2 = blue, slot 3 = green.
   Same chip colors echo into plots / legends / printouts later.
3. **Words, not abbreviations.** "Light", "Temperature", "Forget".
4. **Big primary value.** Current first-field value is the dominant
   text on each row — kids look for the number first.
5. **One button, one action.** No long-press semantics. No hidden
   gestures.
6. **Cursor IS primary.** Whatever slot the cursor sits on becomes
   `_primary_slot`. No separate "cycle" concept.
7. **Hide expert controls.** Period preset, battery %, signal
   dBm, Forget all live in `Vernier > Settings` sub-page, opened
   from the main screen and scoped to the cursor's slot.
8. **Field names must be discoverable.** Many GDX sensors expose
   multiple fields (e.g. GDX-LC: Light / UV / 615 nm / 525 nm /
   465 nm). The kid needs to **read the exact field name** so they
   can reference it later in their GoGo program. The Settings
   sub-page lists all fields by name with their live values; the
   main view shows the kid's chosen "primary field" per slot.

## Hardware constraints

- Display: **160 × 128 px, horizontal**.
- Existing widgets to reuse: `drawButton`, alpha-blended row highlight,
  page banner band.
- Page accent stays purple (`TFT_GOGO_PURPLE`); slot identity colors
  are additional palette entries (red/blue/green), not replacements.

---

## Screen layouts (160 × 128)

### Main view — always-on slot list

Each row shows the slot's **primary field** (default = field 0,
re-pickable in the Settings sub-page). When the slot has more than
one field, a `★ N/M` indicator on the device-name line tells the
kid "this slot has more — see Settings to read all field names".

```
y=0   ┌─────────────────────────────────────────────────┐
       │ Vernier Sensors                 2/3 connected   │  banner (purple)
y=14   ├─────────────────────────────────────────────────┤
       │┃● 1  GDX-LC                            ★ 1/5    │  cursor row
       │┃     Light                        56.2  lux     │  primary field
y=42   ├ - - - - - - - - - - - - - - - - - - - - - - - - ┤
       │ ● 2  GDX-TMP                                    │  single-field sensor
       │      Temperature                  23.4  °C      │  no ★ indicator
y=70   ├ - - - - - - - - - - - - - - - - - - - - - - - - ┤
       │ ○ 3  Empty                                      │
       │      press to add sensor                        │
y=98   ├─────────────────────────────────────────────────┤
       │   [ Disconnect ]            [ Settings ]        │  action bar
y=128  └─────────────────────────────────────────────────┘
```

Reading guide:
- `★ N/M` — slot's primary field is N (1-indexed) of M total fields.
  Kid sees "★ 1/5" → "there are 5 fields here, I'm seeing field 1".
  Pressing Settings reveals all 5 by name with live values.
- Single-field sensors omit the indicator (no clutter).

When cursor sits on the empty slot, the action bar swaps:

```
y=98   ├─────────────────────────────────────────────────┤
       │   [ + Add sensor ]          [ Settings ]        │
y=128  └─────────────────────────────────────────────────┘
```

While a connect attempt is in flight on the cursor's slot:

```
y=70   ├ - - - - - - - - - - - - - - - - - - - - - - - - ┤
       │┃◐ 3  Searching…       (5 s)                     │  ◐ pulses
       │┃     please wait                                │
y=98   ├─────────────────────────────────────────────────┤
       │   [ Cancel ]                [ Settings ]        │
y=128  └─────────────────────────────────────────────────┘
```

### Vernier > Settings sub-page

Opened by pressing **[ Settings ]** on the main view. Always scoped to
the slot the cursor was on when entered. Breadcrumb header includes
slot id + device name so it's visually distinct from the GoGo
top-level Settings page and the kid sees which sensor they're
configuring without ambiguity.

Page is **scrollable** — needed because GDX-LC has 5 fields, GDX-3MG
has 4, GDX-ACC has 4 (3 axes + magnitude). Worst case ~10 fields
per sensor.

```
y=0   ┌─────────────────────────────────────────────────┐
       │ Vernier > Settings · Slot 1: GDX-LC             │  breadcrumb + device
y=14   ├─────────────────────────────────────────────────┤
       │┃Period           [  1 s  ▾ ]                    │  cursor on Period
       │  Battery 87% · Signal -52 dBm                   │  inline status (no cursor)
       ├ - - - - - - - - - - - - - - - - - - - - - - - - ┤
       │ ★ Light          56.234   lux                   │  ★ = primary field
       │   UV              0.000                         │  press = make primary
       │   615 nm          1.881                         │
       │   525 nm          1.987                         │  scrolls if more
       │   465 nm          2.439                         │
       ├ - - - - - - - - - - - - - - - - - - - - - - - - ┤
       │              [ Forget ]                         │
y=98   ├─────────────────────────────────────────────────┤
       │   [ ◀ Back ]                                    │
y=128  └─────────────────────────────────────────────────┘
```

Cursor stops, in order:
1. `Period` row — press = enter period preset cycle (yellow pill);
   writes per-slot preset.
2. Each field row (one stop per field) — press = set that field as
   the slot's primary (★ moves to the new row, main view re-renders
   to show it next time).
3. `Forget` row — press = `disconnect(slot)` + clear slot's NVS keys
   (`deviceName{slot}` and `vernierPeriodPreset{slot}`).

When the page has more rows than fit on-screen, cursor up/down past
the visible viewport scrolls the content (Period and Forget rows
stay sticky if there's room; otherwise scroll past).

For single-field sensors (e.g. GDX-TMP), the field list collapses to
one row; the ★ is decorative since there's nothing to pick.

When the slot is empty, sub-page shows a placeholder and only Back is
available:

```
y=14   ├─────────────────────────────────────────────────┤
       │ Empty slot                                      │
       │                                                 │
       │       Connect a sensor first.                   │
       │                                                 │
y=98   ├─────────────────────────────────────────────────┤
       │   [ ◀ Back ]                                    │
y=128  └─────────────────────────────────────────────────┘
```

---

## Cursor style — vertical left-edge bar

Reused across both screens. Replaces the alpha-blend horizontal pill
used elsewhere (specific to this page; main menu / settings stay
as-is). Reason: horizontal pills fight with the slot-color chip on
the same row; a thin vertical bar cleanly separates "selection" from
"identity".

Spec:
- Width: **3 px** at `x = 0..3`.
- Height: full row height (28 px on main view, 14 px on settings
  rows / 16 px on tab band).
- Color: `TFT_GOGO_PURPLE` (page accent).
- No row tint — bar alone signals the cursor.

```
┃●1  GDX-LC          56.2 lux        ← cursor row
 ●2  GDX-TMP         23.4 °C
 ○3  Empty
```

Same primitive on settings sub-page rows.

---

## Slot color palette

Fixed slot identity per K6:

| Slot | Color | Hex (TFT16) | Usage |
|------|-------|-------------|-------|
| 1 | Red | `TFT_RED` | filled chip, empty outline |
| 2 | Blue | `TFT_BLUE` | filled chip, empty outline |
| 3 | Green | `TFT_GREEN` | filled chip, empty outline |

Constants live in `gogo-display.h` as `VERNIER_SLOT_COLOR[VERNIER_MAX_SLOTS]`.
Same palette will be reused later by plotting / data export when
those land — kids learn "sensor 1 is always red" once.

---

## Sub-step breakdown

Each row a separable commit per the user's "split for look back"
preference. Build + smoke between behaviour-changing commits per
"wait me tested".

### 4b.4.1 — slot-color palette + chip widget

**Code-only, no user-visible change.**

- Add `VERNIER_SLOT_COLOR[VERNIER_MAX_SLOTS]` constant in
  `gogo-display.h`: `{ TFT_RED, TFT_BLUE, TFT_GREEN }`.
- Add helper `drawSlotChip(x, y, slot_id, connected)` in
  `GoGoDisplay`: filled colored circle when connected, outline
  circle when empty, slot number overlay either way.
- Not wired into any page yet — primitive only, exercised via main
  view in 4b.4.2.

> Safe to ship without smoke: dead code, will not affect any
> existing render path.

### 4b.4.2 — main view replacement (`vernierSlotListView`) + 4b.2 cleanup

**First user-visible change. Atomic swap: old vernier page → new.**

Folds the originally-planned 4b.4.0 cleanup into the same commit so
git history never carries a transient regression frame.

Add:
- `VernierSlotListData` snapshot struct: per-slot `connected`,
  `device_name`, `first_field_name`, `first_field_unit`,
  `first_field_value`, `battery`, `is_searching`, `is_primary`.
- `GoGoVernier::snapshotSlotList(out)` walks `_slots[]` and fills.
- `vernierSlotListView(const VernierSlotListData *)` renders 3 rows
  × 28 px with chip + name + value + unit per row.
- Action bar with placeholder `[ Disconnect ]` / `[ Settings ]`
  buttons (wired up properly in 4b.4.4; here they're static labels
  to fill the layout band).

Remove (4b.2 / pre-4b cleanup):
- `vernierMonitor` (replaced by row-per-slot rendering).
- `vernierControlMenu` switch and its CYCLE / PERIOD / INFO cases.
- `VERNIER_CYCLE`, `VERNIER_PERIOD`, `VERNIER_INFO`, `VERNIER_CONNECT`
  enum entries → replaced by new state machine in 4b.4.3.
- `VERNIER_MODE_COUNT` and mode-dot rendering — no modes anymore.
- Mode-dot constants in `gogo-display.h` (`VERNIER_MODE_DOT_*`).

Touched files (expected):
- `include/display/gogo-display.h` — palette consts already in
  4b.4.1; remove mode-dot block; add new view layout consts.
- `src/display/gogo-display.cpp` — delete vernierMonitor +
  vernierControlMenu; add vernierSlotListView.
- `include/peripherals/gogo-vernier.h` — declare snapshotSlotList.
- `src/peripherals/gogo-vernier.cpp` — implement snapshotSlotList.
- `include/display/display-assets/vernier-data.h` — add
  VernierSlotListData; trim VernierDisplayData fields no longer used.
- `src/gogo-firmware.cpp` — swap render call site for vernier page.

> **STOP HERE for user smoke.** First commit that changes what
> the kid sees on screen.
> Test plan:
>   - 0 sensors: 3 empty rows, slot colors visible (red/blue/green
>     outlines).
>   - 1 sensor: row 1 shows it (red filled chip + name + value +
>     unit), rows 2/3 empty.
>   - Reboot persists the saved sensor → row 1 re-shows it.

### 4b.4.3 — vertical bar cursor + cursor navigation

- Add `_currentSlotListCursor` member on `GoGoDisplay` (range
  `0..VERNIER_MAX_SLOTS-1`).
- Render 3 px purple bar at `x=0..3` on the cursor's row in
  `vernierSlotListView`.
- Wire button up/down (matches existing menu navigation device — to
  confirm at impl: rotary or two-button per the actual hardware) to
  move cursor among rows.
- Cursor change calls `gogoVernier.setPrimarySlot(_currentSlotListCursor)`
  — primary-slot follows cursor.

### 4b.4.4 — zone model + context-sensitive press + footer hint

Reshapes the vernier page to use the existing **zoned-page input
model** (motor/servo/relay convention) so the kid gets full coverage
with one short-press button:

- Each slot row is one **zone**. UP/DOWN walks zones; rotary
  LEFT/RIGHT is unused on this page (no within-zone items).
- Edge-overflow: UP at first slot / DOWN at last slot cycles to
  the next page (matches `canCyclePage` pattern).
- Long-press stays globally reserved for "go HOME" — not overridden.

**Press dispatch on the cursor's slot:**
- Empty slot → `gogoVernier.connect()` (vernier picks first-free).
- Connected slot → `setCurrentDisplayPage(DISPLAY_VERNIER_SETTINGS)`
  scoped to that slot's index. Disconnect path lives inside the
  Settings sub-page as the Forget action (4b.4.7).

**Footer hint** replaces the previously-planned bottom action bar:
- `↑ ↓ slots  ·  press to connect` when cursor on empty slot.
- `↑ ↓ slots  ·  press for settings` when cursor on connected slot.

Touched code:
- `gogo-display.h`:
  - Add `enum vernier_zone_t { VERNIER_ZONE_SLOT_0, _SLOT_1, _SLOT_2,
    VERNIER_ZONE_COUNT }` mirroring `VERNIER_MAX_SLOTS`.
  - Add `DISPLAY_VERNIER_SETTINGS` page id (sub-page placeholder;
    skeleton lands in 4b.4.5).
  - Resize slot rows to ~30 px now that the action bar's gone;
    drop `VERNIER_ACTION_*` constants.
- `gogo-display.cpp`:
  - `vernierSlotListView` reads `_currentZone` for the cursor row
    (was `_currentDisplayMenu`); appends `drawFooterHint` based on
    the cursor's slot connected-state.
  - Drop the DISPLAY_VERNIER_SENSOR case in `advanceDisplayMenu`
    (cursor moves via UP/DOWN now).
- `gogo-firmware.cpp`:
  - `canCyclePage` adds DISPLAY_VERNIER_SENSOR (true at zone 0
    going up / zone VERNIER_ZONE_COUNT-1 going down).
  - `advanceWithinPage` adds DISPLAY_VERNIER_SENSOR →
    `advanceZone(forward, VERNIER_ZONE_COUNT)` followed by
    `gogoVernier.setPrimarySlot(getCurrentZone())`.
  - `pageControlPress` DISPLAY_VERNIER_SENSOR case rewritten to
    dispatch on `getCurrentZone()` (slot index): empty → connect;
    connected → goToPage(DISPLAY_VERNIER_SETTINGS).
  - `pageControlAdjust` DISPLAY_VERNIER_SENSOR case becomes no-op
    (rotary unused on this page).
  - Page-init: `setCurrentZone(gogoVernier.primarySlot())` (was
    `setCurrentDisplayMenu`).
- DISPLAY_VERNIER_SETTINGS render path: minimal "Settings (WIP)"
  placeholder + back hint. Long-press still goes HOME.

> **STOP HERE for user smoke** after build verifies. New behaviour:
> kid uses UP/DOWN to walk slots; pressing on a connected slot
> opens an intentionally blank "Vernier > Settings (WIP)" page.
> 4b.4.5 fills the sub-page in next.

### 4b.4.5 — Vernier > Settings sub-page skeleton (read-only)

- Add render function `vernierSettingsSubPage(slot_id, snapshot)`
  with breadcrumb header `Vernier > Settings · Slot N: <device>`.
- Static layout: Period row (read-only display only in this commit),
  inline Battery/Signal status line, separator, Forget button (greyed).
- `[ ◀ Back ]` returns to main view, restoring main cursor.
- Cursor on sub-page navigates Period ↔ Forget (Forget greyed; press
  does nothing).
- **No field list yet** — landed in 4b.4.5b for separate review.

> Smoke between 4b.4.5 and 4b.4.5b: open settings, see Period /
> Battery / Signal, back. No write paths yet. Safe to ship.

### 4b.4.5b — Field list + primary field picker

Critical for kid programmability — lets the kid read all field
names exposed by their sensor so they can use them in GoGo programs.

- Extend `VernierSlotListData` (and host adapter snapshot) so it
  includes `primary_field_index` per slot.
- Add `_vernierPrimaryFieldPerSlot[VERNIER_MAX_SLOTS]` member on
  host adapter; default = 0; clamped on field-count change.
- Settings sub-page renders **scrollable field list** between Period
  and Forget. Each row: `★ <name>  <value>  <unit>` if primary, else
  `  <name>  <value>  <unit>`. Live-updating values.
- Each field row is a cursor stop. Press = `setPrimaryField(slot,
  field_idx)` → ★ moves; main view re-renders with new primary.
- Scrolling: when total cursor stops (Period + N fields + Forget)
  exceed visible viewport, the field-list band scrolls; Period and
  Forget stay sticky at top/bottom.
- NVS persistence: new key `vernierPrimaryField{slot}` (uint8),
  written on press. Default = 0 if absent.
- Main view (`vernierSlotListView`) updated to render
  `data->field_names[primary_field_index]` and
  `data->values[primary_field_index]` instead of always field 0.
- Star indicator `★ N/M` on main view row's device line shown when
  field count > 1; hidden for single-field sensors.

> Smoke: connect GDX-LC, open settings, see all 5 fields by name +
> live values; press 615 nm → ★ moves to it → back → main view now
> shows "615 nm  1.881  ".  Re-boot → primary persists from NVS.

### 4b.4.6 — per-slot Period editor + NVS schema bump

- Add `_vernierPeriodPresetPerSlot[VERNIER_MAX_SLOTS]` array on
  host adapter.
- `setPeriodPreset(idx, dev)` now writes to slot `dev`.
- Sub-page Period row press = enter preset cycle (yellow pill
  pattern) writing into displayed slot.
- NVS schema bump:
  - New keys: `vernierPeriodPreset0..N` (`vernierPeriodPreset%u`
    format).
  - One-shot legacy migration: on first v3 boot, copy
    `vernierPeriodPreset` → all slot keys, then leave legacy key
    in place (mirrors 3.7's `deviceName` migration).
- Wire commands per slot via `gogoVernier.sendSetRate(period, dev)`.

### 4b.4.7 — Forget action

- Sub-page Forget button press:
  - Calls `disconnect(slot)` to drop the BLE link if connected.
  - Clears slot's NVS keys: `deviceName{slot}` and
    `vernierPeriodPreset{slot}` (latter resets to default 1 s).
  - Returns to main view.

### 4b.4.8 — empty slot affordance polish

- Empty rows render with outline chip `○ N` + greyed text
  `Empty / press to add sensor`.
- `+ Add sensor` button on cursor-on-empty highlighted in slot
  color so the visual cue ties button to row.

---

## NVS schema

After step 4b.4.6:

| Key | Scope | Notes |
|-----|-------|-------|
| `deviceName0..N` | per-slot | populated since 3.7 |
| `vernierPeriodPreset0..N` | per-slot | new in 4b.4.6 |
| `vernierPrimaryField0..N` | per-slot | new in 4b.4.5b; uint8, default 0 |
| `vernierPeriodPreset` | legacy | kept for read-only fallback during one boot, never written after migration |

No protocol bump — wire format from 4a (v2 with `dev` field) is
sufficient for per-slot period commands.

---

## Removed from v1

| v1 element | Why removed |
|------------|-------------|
| Drill-down view | Single screen kid principle. No mode = no get-stuck. |
| `VERNIER_BACK` 5th cursor item | No drill-down to back out of. |
| Long-press = quick disconnect | Hidden gesture, kid-hostile. Disconnect is the explicit action button. |
| `VERNIER_CYCLE` mode + pill | Cursor on list IS the cycle. Two affordances for one action confused users. |
| NVS `vernierLastView` resume | Single view → nothing to resume. |
| Drill-down INFO mode | Folded into Vernier > Settings sub-page. |
| Per-slot period in drill-down | Moved to settings sub-page where it belongs (advanced control). |

---

## Smoke test plan (post 4b.4.x)

Single device (GDX-LC has 5 fields, exercises field picker):
- Boot → main view shows 1 occupied row (slot 1 red) + 2 empty rows
  (slot 2 blue outline, slot 3 green outline).
- Slot 1 device-name line shows `★ 1/5` indicator (5 fields, primary
  is field 0 = "Light").
- Slot 1 value line shows `Light  56.234  lux`.
- Cursor on slot 1: action button = `[ Disconnect ]`.
- Cursor on slot 2 / 3: action button = `[ + Add sensor ]`.
- Press Settings → sub-page header `Vernier > Settings · Slot 1: GDX-LC`.
  - Period row shows current preset.
  - Battery / Signal status line populated.
  - Field list shows all 5: `★ Light / UV / 615 nm / 525 nm /
    465 nm` with live values.
- Cursor down to `615 nm`, press → ★ moves to 615 nm; back → main
  view now shows `615 nm  1.881   ` and `★ 3/5` indicator.
- Re-enter Settings → ★ still on 615 nm (in-memory persistence).
- Reboot host → main view re-renders with `★ 3/5` from NVS.
- Sub-page Period press → cycles through presets (50ms / 100ms /
  500ms / 1s / 2s / 5s).
- Single-field sensor variant (e.g. GDX-TMP): main view row shows
  no `★ N/M` indicator; settings field list has 1 row only.
- Forget on slot 1 → BLE drops, slot 1 row becomes Empty, NVS
  `deviceName0`, `vernierPeriodPreset0`, `vernierPrimaryField0`
  all cleared.

Multi-device (requires step 5 hardware):
- Boot with 2 saved devices → both auto-connect → rows 1 & 2 show
  values, row 3 Empty.
- Move cursor between slots → primary-slot follows.
- Per-slot period: set slot 1 to 100 ms, slot 2 stays at 1 s. Verify
  on co-MCU log: `T_ACK seq=N dev=0` then `T_ACK seq=M dev=1`
  with different periods.
- Forget slot 1 → row 1 becomes Empty, slot 2 keeps streaming.
- Add sensor on row 1 (now empty) → vernier picks first-free
  slot = 0 → reconnects, slot color stays red.

---

## Estimated scope

- 8 commits (4b.4.1, 4b.4.2 [folds old 4b.4.0 cleanup], 4b.4.3,
  4b.4.4, 4b.4.5, 4b.4.5b, 4b.4.6, 4b.4.7, 4b.4.8 + final docs).
- ~1 dedicated session (4–6 hours of focused display-layer work +
  per-step smoke).
- Highest risk concentrated in:
  - 4b.4.2 atomic swap (delete-old + add-new in one commit; first
    user-visible change → mandatory smoke before continuing).
  - 4b.4.5b scrolling math (sticky Period/Forget + scrollable field
    band; touch up at impl time if scroll feels janky).
  - 4b.4.6 NVS migration (per 3.7 lesson: one-shot, idempotent,
    non-destructive of legacy key).

---

## When to start

After step 5 hardware smoke ratifies multi-slot backend on real
BLE peers. Doing UI on top of unproven backend risks debugging two
moving parts at once.

Sequence: 4b.1 (✅) → 4b.2 (✅) → step 5 (multi-device hardware
smoke) → 4b.4.0..4b.4.8.

---

## Design history

### v2 — kids-UX redesign (this document, 2026-05-08)

Driver: review under "kids usage" UX hat surfaced 7 issues with v1
(see "Removed from v1" table). Locked decisions:

| # | Decision | Outcome |
|---|----------|---------|
| K1 | Slot color chips with purple page accent | Approved — purple stays page accent, slot palette is identity-only |
| K2 | Per-slot period adjustability | Kept; lives in Settings sub-page (not main view) |
| K3 | Wording: "Forget" vs "Disconnect" | "Forget" |
| K4 | Row content: name + value | Both shown |
| K5 | INFO mode survival | Folded into Settings sub-page |
| K6 | Slot palette: fixed vs device-specific | Fixed (R/B/G) |
| extra | Settings page header | Breadcrumb `Vernier > Settings · Slot N: <device>` to disambiguate from GoGo Settings |
| extra | Cursor style | Vertical 3 px purple bar at left edge, applied across vernier pages |
| extra | Field discoverability | Settings sub-page lists ALL fields by name + live values; main view shows kid-picked primary field per slot with `★ N/M` indicator. Driven by need to read field names for GoGo programs. |

### v1 — D8 Option C (superseded)

Original plan: separate **slot list view** as entry, **drill-down
view** = single-slot detail (today's monitor). 5th cursor item
`VERNIER_BACK` to exit drill-down. Optional NVS resume of last view.
Per-slot period editor inside drill-down. Long-press for quick
disconnect of highlighted slot.

Why dropped: modal navigation, hidden gestures, and duplicate
affordances were all kid-hostile. Single-screen redesign covers the
same use cases without the cognitive load.

---

## Implementation status

All sub-steps of step 4b.4 shipped, plus a code-review pass and
an iterative smoke-feedback loop. Two repos affected:
- `gogo-firmware` on branch `feature/co-mcu-auto-detect`
- `vernier-firmware` on branch `develop` + submodule
  `lib/GoGoVernier` on branch `main`

### Sub-steps

| Step | Done | Commit | Note |
|------|------|--------|------|
| 4b.4.1 — palette + chip widget | ✅ | `6083a317` | TFT_GOGO_RED/BLUE/GREEN per slot |
| 4b.4.2 — slot list view + 4b.2 cleanup | ✅ | `db2617c7` | atomic swap; deletes vernierMonitor + vernierControlMenu |
| 4b.4.3 — vertical bar cursor + slot nav | ✅ | `0722351c` | superseded by 4b.4.4 zone model |
| 4b.4.4 — zone model + Settings sub-page entry | ✅ | `1a2b9d5d` | UP/DOWN walks zones; press → Settings; LEFT exits |
| 4b.4.5 — Settings sub-page skeleton | ✅ | `a4a35cfa` | Period row + Forget greyed |
| 4b.4.5b — field list + primary field picker | ✅ | `3ad584fd` | scrollable, NVS-persisted primary |
| 4b.4.6 — per-slot Period editor + NVS bump | ✅ | `a3757148` | yellow pill, LEFT/RIGHT cycle, press commit |
| 4b.4.7 — Forget action | ✅ | `8db6ebae`, `d8ce589` (vernier MCU) | C_FORGET wire command |
| 4b.4.8 — empty-slot affordance polish | ✅ | `eb0eaf6c` | "+" symbol on empty chip |

### Smoke feedback fixes

| Fix | Commit | Issue |
|-----|--------|-------|
| Banner overlap, footer flicker, SPI assertion | `b57a32b0` | 3 issues: title/subtitle overlap, padding fillRect every tick, smooth-font drawGlyph endWrite race |
| Per-row diff cache + nav-dot clearance | `1aed32a4` | Cursor move re-rendered whole page; right padding clipped nav dots |
| Right padding + name truncation policy | `d36b80c2` | Long names like "Temperature" got value-padding clipped |
| Unit text wrapping off-screen | `0767ad9c` | Sub-page unit overflowed past 160 px |
| Unit padding rect erasing nav dots | `7e898f5f` | Unit's 3-W padding reached nav dot column |
| Sub-page field flicker, full-page redraw on cursor, LEFT/RIGHT period | `4b6fb802` | Removed forceDraw on cursor change paths |
| Period + Forget row diff cache | `0a556a37` | Period kept blinking on idle ticks |
| Lock UP/DOWN during Period edit | `5279e6df` | Cursor escape mid-edit left flag set |
| Right-align indicator on slot row | `963a8955` | First inline placement; later swapped for `*` |
| Indicator → "*N/M" without brackets | `f1886630` | "[N/M]" felt heavy |
| Indicator collapsed to single purple "*" | `76e63709` | Even compact "*N/M" forced name truncation |
| Inline indicator + bold headers | `5cd923f6` | "*" moved to right-of-name; sub-page header BOLD |
| snapshot pf-drift refill | `c6a8348` | New primary took 5 sec to render |
| device_name always-copy | `6220d95` | First slot showed blank name post-boot |
| connecting... hint | `68ffd26` | Connected-but-no-fields blank gap |
| Status row visual glyphs (battery + signal bars) | `6f6c74d7` | "Battery N% · -N dBm" hard to read |
| GoGo palette slot colors | `9d241b37` | TFT_GOGO_RED/BLUE/GREEN |

### Code-review fixes

Triggered by an `oh-my-claudecode:code-reviewer` pass after the
feature-complete state. 14 findings (0 critical, 3 high, 6 medium, 5
low). Actioned the highs + selected mediums; the rest are deferred.

| Severity | Finding | Commit |
|----------|---------|--------|
| HIGH | Forget/Period press fires on empty slot | `22e2073` |
| HIGH | gblVernierSetting overload (flag + index) | `d7d85d8` |
| HIGH | setPeriodPreset broadcast path unused | `4773c75` |
| MED | Cursor-bar fillRect duplication | `2b4da9c` |
| MED | Footer hint pointer-equality + perf cache | `b2749aa` |
| LOW | C_FORGET unpaired-slot shortcut | `a91264a` |

### Wire protocol additions

| ID | Direction | Purpose |
|----|-----------|---------|
| `C_FORGET = 5` | host → vernier | disconnect + clear NVS deviceName{slot} |

### NVS schema additions (host side)

| Key | Type | Default | Purpose |
|-----|------|---------|---------|
| `vernierPrFld%u` (per slot) | uint8 | 0 | kid's chosen primary field index |
| `vernierPrd%u` (per slot) | uint8 | legacy `vernierPeriod` value | per-slot period preset (one-shot migration from legacy) |

### NVS schema additions (vernier side)

`deviceName%u` per slot was already in place since step 3.7.
`C_FORGET` now removes the per-slot key on demand.

### Submodule bump

`lib/GoGoVernier` `main` advanced by `aee108c`:
- `D2PIOProtocol.h` adds `CMD_GET_STATUS = 0x10` (godirect-py hardcoded opcode).
- `GoGoVernier::refreshStatus()` now sends CMD_GET_STATUS, parses
  battery_percent + charger_state from response (offsets 16/17 of
  the response frame). Resolves the always-0% battery readout.

### Deferred / known issues

- **Multi-slot connect serialization on vernier MCU.** During boot
  auto-connect, `vernierHandler` is single-task; while slot N is
  BLE-handshaking, slot N-1 can't pump samples. Slot 0's first
  sample is delayed ~7 sec on a 3-sensor setup. UI shows
  "connecting..." in the gap. Architectural fix would parallelize
  the connect off the poll task.
- **Lambdas in gogo-firmware.cpp** (`pageControlPress`,
  `pageControlAdjust`, `advanceWithinPage`) carry per-page logic
  for both vernier views; large-ish but not painful. Code review
  proposed extracting `VernierPageController` — useful but outside
  the kid-UX scope.
- **Wire-protocol enum drift.** `C_*` opcodes defined separately on
  host and vernier MCU (one source of truth in either repo, with a
  comment cross-reference). Single shared header / submodule for
  the protocol would prevent silent drift but touches build
  topology.
