# cap41_uf8_softkey_palette

Date: 2026-04-28
Capture host: Windows StoerPC (192.168.177.197) via SSH-driven scheduled-task running USBPcapCMD as SYSTEM
Interface: `\\.\USBPcap1` (UF8 device address `1.26.x` this session)
Duration: ~24s active interaction
Size: 4.87 MB

## What was captured

User toggled between two SSL 360° colour-palette states for the per-strip top-soft-key LEDs above each UF8 scribble strip.

- **Bank A** (8 strips, page-1 colours): white / red / green / blue / cyan / magenta / yellow / orange
- **Bank B** (4 strips, page-2 colours): purple, light-green, pink/?, light-blue, with 4 strips left dark

Toggled the bank selector A/B several times in SSL 360°; SSL360 re-pushed the LED colours each time.

## Decoded findings

**Top-soft-key LED cells: `0x18..0x1F`**

Reverse map: `cell = 0x1F - strip` (strip 0 / leftmost → 0x1F, strip 7 / rightmost → 0x18). Same family as per-strip SOLO/CUT/SEL (cells 0x00..0x17, formula `0x17 - 3*strip - led_offset`).

Frame family: `FF 38 04 <cell> 00 <a> <b> CKSUM` + companion `FF 39 04 <cell> 00 <a> <b> CKSUM` — same paired-write scheme already used for SEL/CUT/SOLO.

In this capture **only OFF/dim states were sent** (FF38 == FF39 for every event) — SSL360 ships colour assignments with both frames carrying the dim payload, lighting up the LED to that colour but in idle/unselected mode. To capture the BRIGHT state, the user would need to press one of the per-strip top-soft-keys during recording (= SSL360 toggles the LED to its focused-on rendering).

## Palette additions

The 8 colours observed in Bank A all matched existing SEL palette dim bytes. Bank B introduced two values not present in `kSelPalette`:

| Colour       | dim `<a><b>` | bright `<a><b>` |
|--------------|--------------|------------------|
| light-green  | `21 F1`      | TBD (not in this capture) |
| light-blue   | `11 F2`      | TBD |

The `00 F0` state seen on un-coloured strips is "no colour assigned / dark" — distinct from any palette colour's dim form.

## Bank-toggle timestamps (for cross-reference)

| t (s) | Bank | Notes |
|-------|------|-------|
| 6.73  | B    | 4 colours + 4 dark |
| 15.45 | A    | 8 colours |
| 19.68 | B    | (same as 6.73) |
| 23.57 | A    | (same as 15.45) |

## Tooling

Decoded via `analysis/decode_softkey_leds.py` — parses tshark output, splits FF 38 / FF 39 frames per cell, classifies novel cells (= not yet in `KNOWN_CELLS`).
