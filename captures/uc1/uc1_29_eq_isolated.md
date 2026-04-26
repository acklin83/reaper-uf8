# uc1_29_eq_isolated

Date: TBD
Windows host: StoerPC
tshark: latest
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: TBD

## Why

Same reason as uc1_28: `uc1_15` bucketing was unreliable across overlapping pot ranges. Re-capture EQ pots one at a time so each gets a clean cell map. Also — user reports the 4 Gain pots (HF / HMF / LMF / LF Gain) render as **3 brightness steps per LED**, not single-dot Position. We need bank 0x02 byte values for those to derive the brightness table.

## Setup

- Same as uc1_28: SSL CS 4K E loaded, audio stopped, master muted
- All pots at default position

## Action

12 pots, sweep each min → max → min, ~5 s, with 3 s pause between.

Order (left side of CS, top to bottom — match physical layout):

1. **Low Pass** (`knob::kCSLowPass`)
2. **High Pass** (`kCSHighPass`)
3. **HF Gain** (`kCSHfGain`) — **3-brightness-per-LED**
4. **HF Freq** (`kCSHfFreq`)
5. **HMF Gain** (`kCSHmfGain`) — **3-brightness-per-LED**
6. **HMF Freq** (`kCSHmfFreq`)
7. **HMF Q** (`kCSHmfQ`)
8. **LMF Gain** (`kCSLmfGain`) — **3-brightness-per-LED**
9. **LMF Freq** (`kCSLmfFreq`)
10. **LMF Q** (`kCSLmfQ`)
11. **LF Freq** (`kCSLfFreq`)
12. **LF Gain** (`kCSLfGain`) — **3-brightness-per-LED**

## Decode targets

- Corrected per-knob bank 0x01 cell maps (eliminate the EQ↔Dyn overlap)
- For the 4 Gain pots: bank 0x02 byte sequence per LED (expect 3 non-zero levels). Confirm whether the values match the GR-LED brightness table `{0x19, 0x2D, 0x54, 0x99, 0xFF}` or use a different gradient.

## Expected (from current code, suspect for cells in 0x52..0x91 range)

| Knob | Current cells |
|---|---|
| Low Pass | 0x95..0x9F |
| High Pass | 0x8A..0x94 |
| HF Gain | 0x7E..0x88 |
| HF Freq | 0x73..0x7D |
| HMF Gain | 0x68..0x72 |
| HMF Freq | 0x5D..0x67 |
| HMF Q | 0x52..0x5C |
| LMF Gain | 0x45..0x4F |
| LMF Freq | 0x3A..0x44 |
| LMF Q | 0x2F..0x39 |
| LF Freq | 0x24..0x2E |
| LF Gain | 0x18..0x22 |
