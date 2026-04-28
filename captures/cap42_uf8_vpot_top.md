# cap42 — UF8 V-Pot top-row LEDs: dim baseline + ch1..ch8 sequential bright

**Date:** 2026-04-28
**Interface:** `\\.\USBPcap1`
**Devices in stream:** UF8 (VID 31e9 PID 0021), UC1 (VID 31e9 PID 0023)
**Size:** ~5.0 MB

## Why

This is the BRIGHT-state capture flagged in the 2026-04-28 handoff (Rule 1: don't write LED code without a real BRIGHT capture).

Background: cells `0x18..0x1F` on the UF8 control the LEDs of the **top soft-key row** sitting directly above the LCDs / V-Pots. cap41 only ever caught OFF/DIM transitions (FF38 == FF39 every event — SSL360 only ever assigned colours, never put a strip in BRIGHT). Without a BRIGHT-state capture we don't know the firmware's actual on-state encoding for these cells, and our 3-state implementation is a guess.

The user is calling these "V-Pot top LEDs" colloquially because they sit above the V-Pots in the strip layout. They are the same cells `0x18..0x1F` (one per strip, 8 strips total).

## What the user did (action timeline)

1. **Initial state** — all 8 strips assigned a colour (white-dim) via SSL360, but no strip is the focused/active one. So all 8 cells show the **DIM** state. Held for ~2 s.
2. **Sequential bright walk** — pressed the top soft-key for channel 1, briefly held (cell 0x18 → BRIGHT, others stay DIM). Released, pressed channel 2 (cell 0x19 → BRIGHT, channel-1 cell may return to DIM or stay BRIGHT depending on SSL360 state model). Continued through channels 3..8.

## What to look for in analysis

- **BRIGHT-state byte encoding** for cells `0x18..0x1F`: at each press, SSL360 should send a frame that sets one of these cells to a value distinct from the cap41 OFF/DIM bytes. Compare the FF38 bytes in cap41 (DIM only) vs. cap42 (DIM + BRIGHT) for the cell pressed — the new byte value is the BRIGHT encoding.
- **Mutual-exclusion vs. additive** — does pressing strip 2 send "0x18 = DIM, 0x19 = BRIGHT" (radio button) or just "0x19 = BRIGHT"? Read the frame deltas around press 2.
- **DIM baseline encoding** — confirm cap41's interpretation of the DIM bytes is correct. The 2-second initial hold is the clean baseline.
- **Press vs. release timing** — if SSL360 holds the BRIGHT only while the button is physically down, we'll see paired press/release frames. If it latches as a focus change, the cell stays BRIGHT until the next strip is pressed.

## Decoder hints

- Diff against `cap41_uf8_softkey_palette.pcap` for the OFF/DIM bytes already known.
- Use `analysis/decode_softkey_leds.py` — it already classifies novel cells; extend its `KNOWN_CELLS` set to include the cap41 DIM bytes so cap42's BRIGHT bytes pop out as novel.
- Eight discrete press events ≈ eight burst windows in the file. Slice by ~500 ms gaps if needed.

## Note

This capture supersedes the handoff's planned cap42 (which proposed driving the BRIGHT state via a UC1-knob-driven focus change). The user's chosen method — direct top-soft-key press on the UF8 — is cleaner: a single button-press maps 1:1 to a single strip's BRIGHT cell. No cross-device causality to untangle.
