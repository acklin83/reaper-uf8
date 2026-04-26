# uc1_28_dyn_gate_isolated

Date: TBD
Windows host: StoerPC
tshark: latest
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: TBD

## Why

`uc1_15` swept all 22 CS+Dyn pots in one continuous capture and used midpoint-split bucketing to attribute LED writes to knobs. The resulting Dyn/Gate cell maps overlap heavily with the EQ cell maps (e.g. `kHfFreq=0x73..0x7D` vs `kGateThreshold=0x72..0x7B`, `kHfGain=0x7E..0x88` vs `kGateRelease=0x7C..0x86`). Physically separate pots cannot share LED addresses — one of the maps is wrong, or the bucketing absorbed unrelated writes.

This isolated capture eliminates the ambiguity by sweeping ONE pot at a time with explicit pauses between, so each LED write can be unambiguously attributed.

## Setup

- REAPER project, single track, **SSL Channel Strip 4K E** loaded
- Audio: **STOPPED** (transport not playing) — we do NOT want VU/GR writes to muddy the LED diff
- Master output: muted as belt-and-suspenders
- All pot positions reset to default at session start

## Action

7 pots, sweep each from min → max → min once, ~5 s per pot. Between pots: hold still for 3 s (silence in the LED stream marks the boundary).

Order (right side of CS section, top to bottom):

1. **Comp Ratio** (`knob::kCSCompRatio` = id 0x0E in current map — verify ID in capture)
2. *(pause 3 s)*
3. **Comp Threshold** (`kCSCompThreshold`) — note: this is one of the **3-brightness-per-LED** knobs; expect bank 0x02 to show multi-step values (likely from the GR-LED brightness table `{0x19, 0x2D, 0x54, 0x99, 0xFF}`)
4. *(pause 3 s)*
5. **Comp Release** (`kCSCompRelease`)
6. *(pause 3 s)*
7. **Gate Range** (`kCSGateRange`)
8. *(pause 3 s)*
9. **Gate Threshold** (`kCSGateThreshold`) — also 3-brightness-per-LED
10. *(pause 3 s)*
11. **Gate Hold** (`kCSGateHold`)
12. *(pause 3 s)*
13. **Gate Release** (`kCSGateRelease`)

## Decode targets

For each pot in turn, extract:
- Set of bank 0x01 cells written (the dot/highlight bank) — these are the ring's cell map
- Set of bank 0x02 cells written and the byte values used — for Position-only knobs expect just `{0x00, 0xFF}`; for 3-brightness knobs (Comp Thr, Gate Thr) expect 3 distinct non-zero values per LED

Output: corrected `kCompRatioCells[]`, `kCompThresholdCells[]`, etc. + a brightness-step table for the 3-step knobs.

## Expected (from current code, may be wrong)

| Knob | Current cells |
|---|---|
| Comp Ratio | 0x3B..0x44 |
| Comp Threshold | 0x46..0x4F |
| Comp Release | 0x50..0x5A |
| Gate Range | 0x62..0x70 |
| Gate Threshold | 0x72..0x7B |
| Gate Hold | 0x87..0x91 |
| Gate Release | 0x7C..0x86 |
