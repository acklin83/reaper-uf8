# uc1_15_knob_channelstrip_sweep

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

Single track in REAPER, **SSL Channel Strip 4K E** loaded (zone 0x10 shows `"CS 2"` as the plugin-tag). UC1 focused on this track. No Bus Comp 2 present — so the 7 top-center V-Pots are repurposed to act as Channel Strip controls instead of Bus Comp params.

## Action

180 s capture. User turned each labeled pot one at a time, ~5 s each, in this reported order:

Left side (14 pots):
1. Input Trim
2. Output Trim (= Fader Level)
3. Low Pass
4. Hi Pass
5. HF Gain
6. HF Freq
7. HMF Gain
8. HMF Freq
9. HMF Q
10. LMF Gain
11. LMF Freq
12. LMF Q
13. LF Freq  ← user reports this was swapped with LF Gain
14. LF Gain

Right side (6 pots — Gate Hold knob exists physically but 4K E has no corresponding param, so its sweep yields knob events with no display response):

15. Ratio (Dyn Comp)
16. Threshold (Dyn Comp)
17. Release (Dyn Comp)
18. Gate Range
19. Gate Threshold
20. Gate Release

## Summary

- 209 802 packets total on USBPcap3
- 4 469 novel IN, 4 179 novel OUT
- Zone 0x03 text (`"<label><spaces><value>"`) appears for every CS knob turn; **zone 0x03 is the Channel Strip equivalent of Bus Comp's zone 0x05**

## Knob-ID map (Channel Strip, confirmed via zone 0x03 display text)

| `knob_id` | Knob | Notes |
|----------:|------|-------|
| `0x00` | Low Pass | dedicated CS pot |
| `0x01` | High Pass | dedicated CS pot |
| `0x02` | HF Gain | EQ HF band |
| `0x03` | HF Frequency | EQ HF band |
| `0x04` | HMF Gain | EQ HMF band |
| `0x05` | HMF Frequency | EQ HMF band |
| `0x06` | HMF Q | EQ HMF band |
| `0x07` | LMF Gain | EQ LMF band |
| `0x08` | LMF Frequency | EQ LMF band |
| `0x09` | LMF Q | EQ LMF band |
| `0x0A` | LF Frequency | EQ LF band |
| `0x0B` | LF Gain | EQ LF band |
| `0x0C` | Input Trim | V-Pot (repurposed) |
| `0x16` | Fader Level (= Output Trim) | V-Pot (repurposed) |
| `0x17` | Gate Release | dedicated right-side pot |
| `0x18` | Gate Hold | dedicated right-side pot — 4K E doesn't map it, but the knob exists and the event fires |
| `0x19` | Gate Threshold | dedicated right-side pot |
| `0x1A` | Gate Range | dedicated right-side pot |
| `0x1B` | Release (Dyn Comp) | dedicated right-side pot |
| `0x1C` | Threshold (Dyn Comp) | dedicated right-side pot |
| `0x1D` | Ratio (Dyn Comp) | dedicated right-side pot |

## Cross-check with uc1_07 (Bus Comp sweeps)

`uc1_07` saw `0x0E` Ratio, `0x0F` Makeup, `0x11` Release, `0x12` Threshold, `0x14` Mix, `0x16` S/C HPF — all with zone **0x05** display. `uc1_15` sees `0x16` as Output Trim with zone **0x03** display. The apparent conflict on `0x16` resolves as:

- The top-center 7 V-Pots on UC1 have IDs in the `0x0C–0x16` range and **repurpose depending on the focused plugin**. With Bus Comp 2 loaded, the V-Pots drive Bus Comp params; without it, SSL 360° remaps them to Channel Strip params (e.g., `0x0C` → Input Trim, `0x16` → Output Trim).
- The dedicated labeled CS pots (IDs `0x00–0x0B` and `0x17–0x1D`) always drive Channel Strip params and don't repurpose.
- Zone 0x05 is the Bus Comp readout display, zone 0x03 is the Channel Strip readout display — which one SSL 360° writes to depends on which section's V-Pots/pots were last touched.

Implementation implication: Rea-Sixty needs a per-focused-track "what's the top-V-Pot mapping right now?" view based on the plugins present, and switch the rendering target accordingly.
