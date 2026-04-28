# cap43 — UC1 BC VU meter: bypass cascade + GR sweep

**Date:** 2026-04-28
**Interface:** `\\.\USBPcap1` (UC1 + UF8 share this hub this session)
**Devices in stream:** UC1 (VID 31e9 PID 0023), UF8 (VID 31e9 PID 0021)
**Size:** ~8.0 MB

## Why

Two unresolved questions about the UC1 Bus Comp VU meter:

1. **VU backlight on BC bypass** — handoff 2026-04-28 noted that BC bypass cascade in our extension dims all BC LEDs *and* clamps the meter LEDs to silent, but the user reports the VU strip has a **separate physical backlight** that also dims when SSL360 toggles BC bypass. We need to find the cell/command driving that backlight so our extension can match.

2. **GR rendering at intermediate values** — our `kInputCells`/`kOutputCells` mapping for the meter has gaps (Output table only had 8 of 16 cells decoded). A stepped GR sweep through 0/4/8/12/16/20 dB with SSL360 driving gives us the full transitional state for the GR meter strip too.

## What the user did (action timeline, in order)

1. **BC In → Bypass.** Watch UC1 — all BC-section LEDs dim, mechanical VU meter backlight visibly darkens.
2. **(short pause)** confirm dim state.
3. **BC Bypass → In** (re-enable). LEDs and backlight return to full brightness.
4. **GR sweep, reverse direction:** 20 → 16 → 12 → 8 → 4 → 0 dB gain reduction. Stepped through the SSL Bus Compressor's threshold/ratio in SSL360 (or hands-on UC1) so the BC GR meter strip walks DOWN from full deflection to silent.

User reports order was reversed from the originally proposed 0→20 — actual sequence was **20 → 16 → 12 → 8 → 4 → 0 dB**. Packet timing is what matters; direction is decoded from frame-by-frame analysis anyway.

## What to look for in analysis

- **Backlight cell**: a single byte/cell that toggles between bright and dim during action 1↔3. Should be *outside* the meter cells `0x3A..0x78` we already mapped — likely a sibling cell, possibly in the GR-strip range or a global-meter command.
- **Meter walk-down**: monotonically decreasing brightness/lit-cell-count across the BC GR strip (cells `0x5C..0x60` per `uc1-led-encoding.md`, possibly extended) as user steps from 20 dB to 0 dB GR.
- **Bypass cascade beyond meter**: cross-check what SSL360 does to the BC knob-ring cells, BUS COMP IN button LED, etc., during action 1 — independently confirms our cascade is matching SSL behaviour for non-meter LEDs.

## Decoder hints

- Idle baseline: any prior idle UC1 capture (e.g. `cap24_idle_baseline_v2.pcapng`) should diff cleanly against the *steady-state bypass* segment of this capture (a window between actions 1 and 3 where nothing else changes).
- For the GR walk-down, slice into 6 sub-windows separated by ~1 s of quiescence — each window is a single GR step's frame burst.
