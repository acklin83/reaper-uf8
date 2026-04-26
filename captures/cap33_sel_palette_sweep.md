# cap33_sel_palette_sweep

**Date:** 2026-04-26
**Goal:** Decode SSL360's per-track-colour byte encoding for the FF 38/39 LED
colour-write pair, by sweeping 12 REAPER track colours through SEL DAW-Colour
mode and capturing the resulting `<a> <b>` byte pairs. Also confirms FF 38/39
fires when UF8 is in **PM Layer** (the layer our extension actually uses) —
cap31 only proved it for DAW Layer.

## Setup
- UF8 → Windows, **PM Layer (Layer 2)**
- SSL 360° → "SEL Button Colour: **DAW Colour**"
- SSL Channel Strip 2 on Track 1 (so SSL360 reads track colour via plugin IPC)
- 12 REAPER tracks pre-coloured by user with the 12 RGB targets below

## Sequence
- 3 s idle
- Click track 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 (each held ~2 s)
- Bank Right (so tracks 9-12 land on UF8 strips 1-4)
- Click track 9 → 10 → 11 → 12 (each held ~2 s)
- 3 s idle
- (User did one more bank shift back at the end — visible at t≈38.7 s)

## Track-colour assignment

| Track | RGB         | Name      |
|-------|-------------|-----------|
| 1     | 255,   0,   0 | red       |
| 2     | 255, 128,   0 | orange    |
| 3     | 255, 255,   0 | yellow    |
| 4     | 128, 255,   0 | lime      |
| 5     |   0, 255,   0 | green     |
| 6     |   0, 255, 255 | cyan      |
| 7     |   0, 128, 255 | lightblue |
| 8     |   0,   0, 255 | blue      |
| 9     | 128,   0, 255 | purple    |
| 10    | 255,   0, 255 | magenta   |
| 11    | 255,   0, 128 | pink      |
| 12    | 255, 255, 255 | white     |

## Findings

### FF 38/39 in PM Layer — confirmed

cap31 captured the FF 38/39 LED-write family in DAW Layer. cap33 was
recorded with UF8 in PM Layer and the same frames flow — including normal
heartbeats `FF 66 21 09/0A` and the PM keepalive `FF 1B 01 00`. So the
colour-write path is **layer-agnostic**: the extension can use it in PM
Layer (its normal operating mode).

### Per-colour byte table

| Colour    | RGB         | Bright (a, b) | Dim (a, b) |
|-----------|-------------|---------------|------------|
| red       | 255,0,0     | `0F F0`       | `01 F0`    |
| orange    | 255,128,0   | `3F F0`       | `12 F0`    |
| yellow    | 255,255,0   | `EF F0`       | `11 F0`    |
| lime      | 128,255,0   | `F0 F0`       | `10 F0`    |
| green     | 0,255,0     | `F0 F0`       | `10 F0`    |
| cyan      | 0,255,255   | `F0 FF`       | `10 F1`    |
| lightblue | 0,128,255   | `F0 FF`       | `10 F1`    |
| blue      | 0,0,255     | `00 FF`       | `00 F1`    |
| purple    | 128,0,255   | `03 FF`       | `01 F3`    |
| magenta   | 255,0,255   | `2F F4`       | `12 F1`    |
| pink      | 255,0,128   | `0F FF`       | `01 F1`    |
| white     | 255,255,255 | `FF FF`       | `11 F1`    |

SSL collapses lime↔green and cyan↔lightblue to identical byte pairs —
SSL's effective palette has 10 distinct colours plus off.

### FF 39's `b` byte is fixed at 0xF0 when lit

cap31's SEL-white sample suggested FF39 might mirror FF38's b-byte (both
were FF), but cap33 disproves it: every bright entry across all 12
palette colours has FF39 = `00 F0`, regardless of FF38 b ∈ {F0, FF, F4}.
The implementation now hard-codes `(0x00, 0xF0)` for the FF39 lit-state.

### Bit-pattern guess (informational)

- `a` high-nibble  ≈ green
- `a` low-nibble   ≈ red
- `b` low-nibble   ≈ blue
- `b` high-nibble  ≈ constant `F` (anomaly: magenta `b`-hi-nibble is `F`
  but `b`-lo-nibble is `4` instead of `F`)

Bright = full-scale 0..F per channel; dim = compressed 0..3. SSL applies
its own quantization step (so 128-mid-tones in REAPER often snap to either
0 or full). We don't decode the bit-pattern — we just replay captured
byte pairs via `ledColourForTrackRgb()` which Euclidean-snaps REAPER RGB
to the nearest of 10 palette anchors.

### Universality across LED cells

During bank-shift refresh SSL360 also writes MUTE cells (0x10/0x13/0x16/
0x0D) — but the value is a constant `0F F0` (red bright pattern), not the
track colour. That means SSL360 reserves DAW-Colour-mode LED-by-track for
SEL only; SOLO/CUT keep their own state-driven colours.

The cell namespace and frame format are however cell-agnostic, so we are
free to drive SOLO/CUT with arbitrary palette colours (or track colour)
in our extension. The user-facing default in this build:
- SEL → track colour (palette-quantized)
- SOLO → yellow
- CUT → red

The user's Settings UI (planned Phase 2.5/3) will let each LED class be
re-bound to any palette colour or track-colour-follow.

## Capture window
- File: `cap33_sel_palette_sweep.pcapng`  (6.3 MB, 60 s)
- Bright SEL events at: t = 5.6, 7.7, 9.6, 11.4, 13.3, 15.3, 17.4, 19.5,
  22.2, 23.9, 25.7, 27.7 s (12 selections, matching the 12 colours)
- Bank-shift refresh bursts at t ≈ 21.0 s and t ≈ 38.7 s
