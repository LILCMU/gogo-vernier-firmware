# Vernier display patterns — TFT_eSPI quirks + diff-render + GDX battery

Durable findings from Phase 4 step 4b.4 (kids-UX redesign on the
GoGo Board host display, Sept–Oct 2026). Promoted from session
notes because each one cost real debugging time and applies to
future display work on this codebase.

---

## TFT_eSPI smooth font + outer startWrite/endWrite race

**Symptom:** assert failed: `xTaskPriorityDisinherit ... pxTCB ==
pxCurrentTCBs` inside `SPIClass::endTransaction` during high-volume
drawString calls. Backtrace ends at
`TFT_eSPI::drawGlyph (Smooth_font.cpp:531)` -> `end_tft_write` ->
`spiEndTransaction`.

**Cause:** `Smooth_font::drawGlyph` calls `endWrite()` (line 532)
unconditionally after rendering a glyph. The library's
`startWrite()` sets `lockTransaction = true` and `inTransaction =
true`; `endWrite()` always clears both. So a `drawString` invoked
inside a caller's `startWrite() / endWrite()` block tears down the
caller's transaction lock mid-block. The caller's eventual
`endWrite()` then double-releases the SPI bus mutex, asserting in
FreeRTOS priority disinheritance.

**Symptom-level pattern:** the bug only fires when *enough*
glyphs are drawn to trigger drawGlyph's mid-glyph yields and a
context switch lands at the wrong moment. Low-volume code (one or
two drawString calls) hides it for years.

**Fix in this codebase:** removed the surrounding
`_display.startWrite() / endWrite()` from
`vernierSlotListView` and `vernierSettingsSubPage`. Pure pixel-fill
blocks (`fillRect`, `drawCircle`) keep their batched wraps; any
function that invokes `drawString` runs without the outer wrap.
See `gogo-firmware/src/display/gogo-display.cpp` —
`drawVernierCursorBar` and the slot-row loop.

**Trigger-test:** UP/DOWN navigation on a 3-slot view forced
forceDraw=true on the cursor-update path, which fillRect-cleared
the content band then drew 3 rows × ~6 drawString calls (chip
number + name + field name + value + unit). That tipped the
race. Don't reproduce by drawing one button.

---

## Diff-render pattern for live-data displays

**Goal:** render once on entry, then per tick only repaint the
cells whose underlying state actually changed. Live values should
update silently; cursor moves should flash only the cursor-bar
cells.

**Pattern (per row / per cell):**

```cpp
struct CellCache {
    bool valid = false;
    /* whatever fields drive the visible content */
};
static CellCache s_cache[N];

if (forceDraw) {
    // Page entry: clear region once, then invalidate caches so the
    // diff loop redraws everything on the same tick.
    fillRect(content area);
    for (auto &c : s_cache) c.valid = false;
}

for each cell i:
    bool stateChanged   = ... compare cache vs current input ...;
    bool cursorChanged  = ... compare cursor flip flag ...;

    if (forceDraw || cursorChanged)
        drawCursorBar(...);

    if (forceDraw || stateChanged)
        drawCellText(...);

    s_cache[i] = current;
```

**Pitfalls:**

1. The cache key must cover **everything** the render reads. We
   missed `primary_field_index` initially — kid swap took 5 sec
   to reflect because `fields_version` was stable.
2. Source fields that update independently from the version
   counter need their own copy path. We hit this with
   `device_name`: T_DEVINFO arrives independently of T_FIELDS, so
   a fields_version-only cache keeps the prior name. Pull
   "always-copy" fields out of the gated block.
3. `forceDraw` should fire **once** on page entry, never on
   cursor moves or value updates — internal diff cache handles
   those without the full-page fillRect that causes a visible
   blink.
4. Static caches survive across page switches. If your render
   function is called for a new context (e.g., different slot in
   sub-page), include a slot-id field in the cache key and treat
   slot-id mismatch as a forced redraw.

**Footer-hint pointer cache:** if the hint text is one of N
file-scope constexpr literals, you can use pointer comparison
(`s_lastHint != hint`) as the cache key — it's cheaper than
strcmp. Promote the literals to file scope and label the
intent in a comment so a future maintainer doesn't innocently
move a hint string into a stack buffer and break the cache.

---

## GDX CMD_GET_STATUS — battery readout

**Problem we hit:** Vernier's published D2PIO opcode list does
not include a "get status" command, so the GDX driver
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

## Layout constraints on the 160 × 128 display

Hard pixel facts — used as design budget across vernier UI work:

- **Page banner**: y=0..18 (height 18 px). Centered title in
  `BOLD_TEXT` smooth font.
- **Footer hint**: y=114..128 (height 14 px). `drawFooterHint`
  fillRect-clears and writes — **always wrap calls in a cache**.
- **Page nav dots**: pop up at `NAV_DOT_X = 156, NAV_DOT_R = 2`
  during UP/DOWN page cycling. They occupy x=154..158 — content
  must keep a margin (we use `VERNIER_SLOT_RIGHT_PAD = 4`,
  ending visible content at x ≤ 154).
- **TFT_eSPI textPadding**: pre-clears a fixed-width rect under
  the upcoming drawString. Text that overflows the padding still
  draws but doesn't get pre-cleared, so previous-frame stale
  glyphs may bleed through. Set padding ≥ longest expected text;
  for inline columns, use an explicit truncation pass before
  drawString.
- **Smooth font width approx**: SMALL_TEXT (BalooRegular6) ~5–6
  px per char average, ~7 px for wide glyphs (W). Always use
  `_display.textWidth(...)` for layout decisions, not pixel
  estimates — the font handles kerning per glyph.

---

## Multi-slot connect serialization (vernier MCU side)

**Symptom:** in a 3-sensor auto-connect, slot 0's first sample
arrives ~7 seconds after slot 0 itself reports "streaming
started" — the UI sees `connected=true, has_sample=false` for
~7 sec.

**Cause:** `vernierHandler` is a single FreeRTOS task that
round-robins `vernier.poll()` across slots and also drives
`autoConnectDevice` sequentially. While slot N is in BLE
handshake (synchronous calls into ArduinoBLE that block ~3 sec
each), already-connected slots can't be polled. After all 3
slots finish handshake, polling resumes for all of them and
samples flow.

**UI mitigation:** `vernierSlotListView` shows "connecting..."
when `e.connected && e.field_count == 0` (or `!has_sample`).
Kid sees activity instead of a blank row.

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
| `vernierPrFld%u` | `vernierPrFld9` (13 chars) | ✅ |
| `vernierPrd%u`   | `vernierPrd9` (11 chars) | ✅ |
| `deviceName%u`   | `deviceName9` (11 chars) | ✅ |
| `vernierPeriodPreset%u` (rejected) | 21 chars | ❌ |

If a key formatter overruns 15 chars Preferences silently
truncates and you get key collisions (every slot writes to the
same truncated key). Always test with the maximum slot index.

---

## Cross-references

- Plan: `.claude/plans/multi-device-ui-step-4b4.md`
- Earlier knowledge:
  - `.claude/knowledges/d2pio-debug-findings.md` — protocol-level
    debugging session notes (preserved as-is, captures pre-Phase-4
    findings)
  - `.claude/plans/multi-device-design.md` — D-decisions ratified
    upstream of step 4b.4
- Related submodule commit: `aee108c` on
  `lib/GoGoVernier@main` (battery via CMD_GET_STATUS)
