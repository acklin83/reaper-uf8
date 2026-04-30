# cap48_pages

**Date:** 2026-04-30
**Action:** User cycled UF8 through soft-key banks V-Pot → Soft 1..5 while
SSL360° was driving the surface. Goal: capture Page Left / Page Right
LED frames so the cell IDs could be decoded.

## Findings

- **Page Right LED cell = `0x2C`.** Every previous probe (FF 38/39 04
  alone) had failed to light this cell because SSL360 always pairs the
  colour-pair frame with a legacy `FF 3B 03` mono frame.
- **Page Left LED cell = `0x2D`** (re-confirmed; first decoded earlier
  the same day).
- Both Page LEDs are 3-state, NOT 2-state:
  - **OFF**:   `FF 38 04 <cell> 00 00 F0` + `FF 39 04 <cell> 00 00 F0`
              + `FF 3B 03 <cell> 00 00`
  - **DIM**:   `FF 38 04 <cell> 00 11 F1` + `FF 39 04 <cell> 00 11 F1`
              (no legacy frame for dim)
  - **BRIGHT**: `FF 38 04 <cell> 00 FF FF` + `FF 39 04 <cell> 00 00 F0`
              + `FF 3B 03 <cell> 00 01`

## Other observations

- Soft 1..5 LED at cells `0x5E..0x5A` confirmed (radio group with V-POT
  Bank at `0x5F`).
- Bank-change events also touched per-strip top-soft-key cells
  `0x18..0x1F` — SSL360 paints those for visible banks based on which
  parameter slots have content in the new bank.
