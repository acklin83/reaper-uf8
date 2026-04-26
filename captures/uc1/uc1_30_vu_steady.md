# uc1_30_vu_steady

Date: TBD
Windows host: StoerPC
tshark: latest
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: TBD

## Why

`dual_36_cs_vu_ramp` documented VU cells as bank 0x02 ranges `0x71..0x7C` (main), `0x3A..0x3E` (peak hold), and multi-state `0x5B/5C/5D`. Several of those overlap with current EQ knob ring cells (HMF Q, HMF Freq, HMF Gain, HF Freq, LMF Freq). Need a steady-state VU capture with NO knob movement to confirm which cells SSL360 actually drives for VU and rule out the overlap being a midpoint-bucketing artefact.

## Setup

- REAPER project, single track, **SSL CS 4K E** loaded
- ReaSynth or any tone generator on the track playing a steady **sine at -20 dBFS**
- Transport playing
- **DO NOT TOUCH ANY POT OR BUTTON** during capture
- Capture ~30 s

## Decode targets

- Confirm the VU cell list on bank 0x02 — should be a stable repeating pattern at the audio update rate
- If we see writes to cells like 0x73, 0x7C etc. on bank 0x02 with no knob events, those are real VU cells and our knob ring maps that include them are wrong (need to be tightened to non-VU cells, or the rings render via bank 0x01 only and bank 0x02 is VU-only)

## Hypothesis to test

Maybe bank 0x01 = pot rings, bank 0x02 = VU/meters, and our current `pushKnobRing_` writing to BOTH banks is the bug — it's overwriting VU on every knob move. If true, the fix is to only write bank 0x01 for ring updates (drop the bank 0x02 brightness write).
