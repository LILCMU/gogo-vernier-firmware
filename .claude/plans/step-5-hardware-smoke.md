# Phase 4 step 5 — multi-device hardware smoke plan

End-to-end verification of the multi-slot stack on real BLE peers.
Step 4a (backend) + step 4b (kid-UX) shipped without ever running
under genuine 3-sensor concurrent load — this plan is the gate.

Scope: GoGo Board host display + vernier co-MCU + 3 GDX sensors
streaming simultaneously, exercising NVS persistence, per-slot
period, primary field, Forget, slot allocation, and the
"connecting..." UX gap.

---

## Hardware required

- 1 × GoGo Board v7 (ESP32-S3 host) flashed with
  `gogo-firmware @ feature/co-mcu-auto-detect` (latest tip).
- 1 × Vernier co-MCU (ESP32-C3 devkit M-1 or integrated) flashed
  with `vernier-firmware @ develop` (latest tip, includes
  `lib/GoGoVernier @ main` with CMD_GET_STATUS).
- 3 × Vernier Go Direct sensors (mix of multi-field and
  single-field for full coverage):
  - **GDX-LC** (Light) — 5 fields (Light, UV, 615 nm, 525 nm, 465 nm).
    Exercises field list scroll + primary field picker.
  - **GDX-TMP** (Temperature) — 1 field. Exercises single-field
    no-indicator branch.
  - **GDX-EA** (Electrode Amplifier / pH) — 2 fields (Potential mV,
    pH unitless). Exercises unit-clear-on-field-swap.
  - Substitutes accepted: GDX-3MG (3-axis magnetic) for ACC, any
    multi-field GDX for LC, any single-field for TMP.

Optional but ideal for the "all slots full" branch:
- 4th sensor of any kind (won't pair, host should reflect "all
  slots full").

---

## Pre-test setup

1. Power both GDX sensors on; verify they advertise (LED solid).
2. **Wipe NVS on both MCUs** — fresh device test:
   ```
   pio run -e debug -t erase
   pio run -e debug -t upload
   ```
   on both `vernier-firmware` and `gogo-firmware`. (If platformio
   doesn't expose erase, jumper IO0 + reset, run `esptool.py
   erase_flash`.)
3. Open serial monitors on both MCUs (115200 + 921600 respectively
   per `platformio.ini`). Vernier monitor goes via USB-CDC; host
   via the dedicated USB-UART.
4. Capture the boot transcript from both for the report.

---

## Test cases

Tests run in order; later tests depend on earlier state. Note any
deviations against the "Expected" column.

### A. Cold boot & first pairing (no NVS)

| # | Action | Expected |
|---|--------|---------|
| A1 | Power on host + vernier with GDX-LC + GDX-TMP + GDX-EA on. | Host boots, navigates to MAIN. Vernier MCU log `Auto-connecting slot 0 to saved device ...` SKIPPED (no NVS). |
| A2 | Navigate host to Vernier page. | All 3 rows render: outline chips R/B/G with `+` symbol, "Empty" label, "press to add sensor" hint, footer `↑ ↓ slots · press to add sensor`. |
| A3 | Press button on slot 1 (cursor on row 1). | Vernier MCU scans, picks first nearby (probably GDX-LC closest). Host row 1 transitions: outline chip → filled red chip with "1" inside, device name "GDX-LC ...", "connecting..." hint, then field name + value + unit. |
| A4 | Wait until streaming, repeat for slot 2. | Slot 2 picks up next sensor. Same flow. |
| A5 | Repeat for slot 3. | All 3 slots streaming. |
| A6 | Verify each row shows correct values updating ~1 Hz. | Light value updates each second. Temperature stable. Potential/pH alternates if cursor swap. |

### B. NVS persistence & auto-connect (warm boot)

| # | Action | Expected |
|---|--------|---------|
| B1 | Power-cycle host + vernier (turn off all sensors first to test BLE re-discovery, then on). | Vernier MCU log: `Auto-connecting slot 0 to saved device GDX-LC ...`, slot 1, slot 2 in sequence. Total ~14 sec to all-streaming. Host shows "connecting..." on each slot until T_FIELDS lands. |
| B2 | Once all streaming, verify slot ordering. | Slot 1 = GDX-LC (red), slot 2 = GDX-TMP (blue), slot 3 = GDX-EA (green). Names persisted. |
| B3 | Verify GDX-LC shows `*` indicator at name end. | Yes — multi-field sensor. GDX-TMP no indicator (1 field). GDX-EA shows `*` (2 fields). |
| B4 | Power-cycle a SINGLE sensor (GDX-LC off, then on after ~30 sec). | Liveness watchdog on vernier MCU resets slot 0 at the timeout; host row briefly shows "connecting..." then re-streams. Other slots untouched. |

### C. Settings sub-page — Period + field + Forget

| # | Action | Expected |
|---|--------|---------|
| C1 | Cursor on slot 1 (GDX-LC), press. | Sub-page opens, banner `Slot 1 · GDX-LC` BOLD. Period row shows `Period 1 s`. Status row shows battery icon + percent + signal bars + dBm. Field list shows all 5 fields with `★ Light` (primary by default). |
| C2 | UP/DOWN walks Period → Light → UV → 615 nm → 525 nm → 465 nm → Forget. | Cursor (3 px purple bar) flips between rows. No flicker. Footer hint adapts per row: Period→`press to edit`, fields→`press to pick`, Forget→`press to forget`. |
| C3 | Cursor on `UV`, press. | `★` moves to UV. Back arrow (rotary LEFT). Main view row 1 now reads `GDX-LC *` and bottom line `UV  0.000` (no unit). |
| C4 | Re-enter sub-page, cursor on Period, press. | Yellow pill on Period row. Footer: `← → adjust · press: save`. UP/DOWN locked (cursor stays on Period). |
| C5 | LEFT/RIGHT cycles preset: `1 s → 2 s → 5 s → 10 s → 30 s → 60 s → 120 s → 100 ms → 200 ms → 500 ms → 1 s`. | Each tick updates the value in the pill immediately. setRate sent over the wire each click (vernier MCU log: `Set sampling period to NN ms`). |
| C6 | Pick `100 ms`, press to save. | Pill clears. NVS write logged on host. Slot 1 now streams at 10 Hz; slots 2/3 stay at 1 Hz. **Per-slot independence verified.** |
| C7 | Cursor on Forget, press. | C_FORGET sent to vernier; vernier disconnects + clears `deviceName0` NVS; T_DEV_LIST repopulates. Host bounces to main view; slot 1 row goes empty (outline chip, `+` symbol, "press to add sensor"). |
| C8 | Power-cycle host + vernier. | Slot 1 stays empty across reboot (NVS cleared). Slots 2 + 3 auto-reconnect with their per-slot period preserved. |

### D. Slot allocation edge cases

| # | Action | Expected |
|---|--------|---------|
| D1 | After C8, slot 1 empty + slots 2+3 connected. Press to add on slot 1. | First-free allocator picks slot 0 (the empty one). New sensor pairs there. |
| D2 | (If 4th sensor available) try to pair while all 3 slots filled. | T_ACK back from vernier with `ok=false msg="all slots full"`. Host UI shows... TBD — check current behaviour. Connecting indicator should clear; row should remain "Empty" if user pressed there. |
| D3 | Disconnect a streaming sensor (power off). | Host row goes empty after liveness watchdog (~5 sec). NVS retained — power-cycling host re-attempts auto-connect. |

### E. UX rendering & polish

| # | Test | Expected |
|---|------|----------|
| E1 | Slot row top line for GDX-LC (long name like "GDX-LC 091001F5"). | Truncated with ".." if needed; `*` always at the right edge. |
| E2 | Slot row bottom line for GDX-LC at 100 ms period. | `Light  56.234  lux` updates 10 Hz; no flicker; unit cell stable. |
| E3 | Swap primary on slot 1 from `Light` to `UV` (no unit). | "lux" cleared; bottom line reads `UV  0.000` with blank unit cell. |
| E4 | Sub-page status row at battery 100% / signal -50. | `[batt-icon-green-full] 100%   [4 signal bars] -50 dBm`. Fits 160 px; no wrap. |
| E5 | Sub-page status row at battery 15%. | Battery glyph fill bar in RED, ~3 px wide. |
| E6 | Cursor move on slot list (UP/DOWN). | Only the two affected cursor-bar segments flash. Slot text + chips stay still. **No full-page blink.** |
| E7 | Idle ticks on sub-page (no input, no value change). | Zero visible flicker on Period row, status row, field list, footer. |
| E8 | UP/DOWN at slot 0 (top row). | UP cycles to previous page (Onboard or Settings). DOWN moves to slot 1. |
| E9 | UP/DOWN at slot 2 (bottom row). | DOWN cycles to next page (Run Program). UP moves to slot 1. |
| E10 | Long-press anywhere on Vernier page | Returns to MAIN. |
| E11 | Long-press inside Vernier > Settings sub-page | Returns to MAIN (saves any in-flight Period edit on the way). |
| E12 | Rotary LEFT inside sub-page | Returns to main vernier view. |
| E13 | Rotary RIGHT on main vernier slot row | Opens sub-page (redundant shortcut). |

### F. Battery readout (relies on submodule bump)

| # | Action | Expected |
|---|--------|---------|
| F1 | Cold boot, observe sub-page status row for each slot. | Real percentage (NOT 0%). |
| F2 | Discharge a sensor (use heavily, or use a known low-battery one). | Battery glyph fill shrinks; color steps green→orange→red as percent drops below 50% then 20%. |
| F3 | Charge a sensor while observing host. | charger_state value reflected — currently shown via icon color only; raw value visible via vernier MCU log if needed. |

### G. Throughput stress

| # | Action | Expected |
|---|--------|---------|
| G1 | Set all 3 slots to 100 ms period in sub-page. | Vernier MCU streams at 10 Hz × 3 = 30 frames/s on UART. Host display values update at 10 Hz; no dropped frames or flicker. |
| G2 | Run for 5 minutes. | No host lockup, no vernier MCU reboot, no missed_seq counter growth on host (visible if INFO row resurrected; otherwise via log). |

---

## Failure modes to watch for

1. **xTaskPriorityDisinherit assert on host** during bursty
   rendering — known race documented in
   `.claude/knowledges/vernier-display-patterns.md`. If it
   recurs, capture full backtrace + the active drawString call
   site for analysis.
2. **NVS key collisions** if the format buffer is too small on
   any host code path — check that all per-slot keys for slot 9
   would fit within 15 chars. Currently we only ship 3 slots,
   but defensive against future expansion.
3. **First-slot delay** (~7 sec) during cold boot multi-slot
   auto-connect — the "connecting..." hint covers it. Document
   actual observed delay; if much longer, slot watchdog timeout
   may be misconfigured.
4. **Battery readout still 0** — means submodule bump didn't
   make it onto vernier MCU. Verify
   `git -C lib/GoGoVernier rev-parse HEAD` matches `aee108c`.
5. **Forget doesn't survive reboot** — vernier MCU's
   `deviceName{slot}` clear path may have failed silently. Check
   vernier log for `forgotten` ack with correct dev id.
6. **Period preset doesn't survive reboot** — host's
   `vernierPrd{slot}` write didn't fire. Verify
   `saveVernierPeriodPreset` is called on press commit.
7. **Primary field doesn't survive reboot** — host's
   `vernierPrFld{slot}` write didn't fire. Verify
   `saveVernierPrimaryField` is called on field-row press.

---

## Sign-off criteria

Step 5 passes when:

- [ ] Sections A–G all green or with documented expected
  deviations.
- [ ] No reboots, asserts, or watchdog resets across the full
  test session (target: 30 minutes total uptime).
- [ ] All NVS persistence tests survive at least one full
  power-cycle (B series + C8).
- [ ] All three sensors stream at user-set period within ±10%
  for at least 5 minutes (G1).
- [ ] Battery + charger state read correctly on at least 2 of
  the 3 sensors (F1).
- [ ] Memory: free heap on host stays > 50 KB throughout
  (host log every 60 s, optional but useful).

If any check fails, capture: serial log from both MCUs, photo
of the failed display state, and the test case ID. File as a
findings note under `.claude/notes/` or attach to a bug
ticket before deciding whether to block phase 4 close-out.

---

## Time budget

Estimated ~60–90 min for a clean run including setup +
documentation. Plan for a 2-hour slot.

---

## After step 5

If green: file phase 4 as complete. Open a follow-up issue for
each deferred item flagged in
`multi-device-ui-step-4b4.md::Deferred / known issues`
(multi-slot connect serialization, lambda fat,
wire-protocol enum drift).

If red: triage by severity. UI bugs → polish on
`feature/co-mcu-auto-detect`. Backend bugs → phase 4 cannot
close; file as phase 4.5 work.
