# cap31_solo_cut_led_colours

**Date:** 2026-04-26
**Goal:** Find the SOLO + CUT LED **colour** command. In standard MCU mode the
UF8 SOLO LEDs are yellow and CUT LEDs orange; in our extension they come up
white because we only send the on/off frame `FF 3B 03 …` and never set colour.

cap22 captured the on/off frame in DAW Layer but did not vary anything that
would have triggered a colour write — colour might be a sibling frame in the
`FF 3A` / `FF 3C` / `FF 3D` family (analogous to SEL's `FF 38`/`FF 39` pair),
or it might be embedded in the DAW-Layer init sequence which we do not
currently replay.

## Setup
- UF8 → Windows
- SSL 360° running, UF8 in **DAW Layer** (NOT PM)
- REAPER + Mackie Control Universal surface, output → "SSL V-MIDI Port 1"
- USBPcap, filter on UF8 (PID 0x0021)

## Sequence (annotate timestamps in capture comments where possible)
1. **5 s idle** — baseline DAW-Layer steady state
2. **Track 1 SOLO on**, hold 2 s, **SOLO off**
3. **Track 1 MUTE on**, hold 2 s, **MUTE off**
4. **Track 1 SOLO + MUTE both on**, hold 2 s, both off
5. **Track 2 SOLO on/off** (different strip — confirms per-strip cell mapping)
6. **Track 1 SEL on** — control, FF 38/39 sequence we already know

## Expected diff vs cap22
- cap22 = FF 3B 03 only (state on/off)
- cap31 should reveal a colour-write frame fired alongside SOLO/CUT state changes
- If colour writes are absent → fallback capture: cap32 = full DAW-Layer init (SSL 360° start with UF8 power-cycled), 10 s no user input

## Findings

**The colour command IS the FF 38/FF 39 pair we already knew from SEL DAW-Colour.**
No FF 3B in this capture at all — the FF 3B path from cap22 must be a separate
monochrome route (which is why our extension's LEDs come up white).

### Cell mapping — unified across SOLO/MUTE/SEL

24 per-strip LEDs share a single cell-id space, descending by 3 per strip:

```
cell = 0x17 - 3*(strip-1) - led_offset
  led_offset: SOLO=0, MUTE=1, SEL=2

Strip 1: SOLO=0x17, MUTE=0x16, SEL=0x15
Strip 2: SOLO=0x14, MUTE=0x13, SEL=0x12
...
Strip 8: SOLO=0x02, MUTE=0x01, SEL=0x00
```

This corrects cap30's SEL mapping (which was off-by-one — its "strip 1 = 0x12"
is actually strip 2; standard MCU note 0x18 → strip 1 → cell 0x15).

### Frame format

Pair of writes per toggle:
```
FF 38 04 <cell> 00 <a> <b> <chk>
FF 39 04 <cell> 00 <a> <b> <chk>
```

### Colour byte tables (a, b)

| LED class  | state | FF38 a,b | FF39 a,b |
|------------|-------|----------|----------|
| SOLO yellow | on   | EF F0    | 00 F0    |
| SOLO yellow | off  | 11 F0    | 11 F0    |
| MUTE orange | on   | 3F F0    | 00 F0    |
| MUTE orange | off  | 12 F0    | 12 F0    |
| SEL white   | on   | FF FF    | 00 F0    |
| SEL white   | off  | 11 F1    | 11 F1    |

Pattern:
- **OFF**: FF38 and FF39 carry the same `<a> <b>` (a "dim" value)
- **ON**:  FF38 carries the bright lit colour, FF39 carries a "base" colour
  (`00 F0` for SOLO/MUTE, `00 F0` for SEL too)

### Captured events vs MIDI sequence

| t (s) | MIDI in | UF8 OUT (FF38)         | UF8 OUT (FF39)         |
|-------|---------|------------------------|------------------------|
| 5.20  | SOLO T1 on | 04 17 00 EF F0 32   | 04 17 00 00 F0 44     |
| 7.20  | SOLO T1 off| 04 17 00 11 F0 54   | 04 17 00 11 F0 55     |
| 8.20  | MUTE T1 on | 04 16 00 3F F0 81   | 04 16 00 00 F0 43     |
| 10.20 | MUTE T1 off| 04 16 00 12 F0 54   | 04 16 00 12 F0 55     |
| 11.20 | SOLO T1 on | 04 17 00 EF F0 32   | 04 17 00 00 F0 44     |
| 11.42 | MUTE T1 on | 04 16 00 3F F0 81   | 04 16 00 00 F0 43     |
| 13.42 | SOLO T1 off| 04 17 00 11 F0 54   | 04 17 00 11 F0 55     |
| 13.62 | MUTE T1 off| 04 16 00 12 F0 54   | 04 16 00 12 F0 55     |
| 14.62 | SOLO T2 on | 04 14 00 EF F0 2F   | 04 14 00 00 F0 41     |
| 16.62 | SOLO T2 off| 04 14 00 11 F0 51   | 04 14 00 11 F0 52     |
| 17.62 | SEL  T1 on | 04 15 00 FF FF 4F   | 04 15 00 00 F0 42     |
| 19.63 | SEL  T1 off| 04 15 00 11 F1 53   | 04 15 00 11 F1 54     |

### Open

- Colour-byte encoding is empirical (not yet decoded as RGB / palette index).
  For our needs we just replay the captured `<a> <b>` per LED class — no need
  to understand the bit-field semantics.
- The FF 3B path observed in cap22 still exists in firmware (we use it today
  with white result). Likely a legacy / monochrome route — TBD if it's worth
  keeping for any LED class.
