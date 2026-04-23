# uc1_19_buttons_4ke

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

**SSL 4K E** Channel Strip loaded on the focused track. UC1 mirroring. No track solo'd at capture start.

## Action

60 s capture. User pressed each of the 16 UC1 buttons once, ~3 s apart, in this exact order (no multiple presses — single clean press per button):

1. HF Bell
2. EQ Type
3. EQ In
4. LF Bell
5. Fast Attack (Comp)
6. Peak
7. Dyn In
8. Expand
9. Fast Attack (Gate)
10. Polarity
11. S/C Listen
12. Solo Clear
13. Solo
14. Cut
15. Fine
16. Channel IN

## Summary

- 67 384 packets total on USBPcap3
- 32 novel IN events (16 press + 16 release)
- 105 novel OUT frames (display + LED updates)

## Complete button-ID table (direct display-text evidence)

| ID | Button | Display text in zone 0x03 |
|----|--------|---------------------------|
| `0x08` | HF Bell | `"HF Bell         In"` |
| `0x09` | EQ Type | `"EQ Colour       Orange"` |
| `0x0A` | EQ In | `"EQ              Out"` |
| `0x0B` | LF Bell | `"LF Bell         In"` |
| `0x14` | Fast Attack (Comp) | `"Fast Attack     In"` |
| `0x15` | Peak | (no display text emitted) |
| `0x16` | Dyn In | `"DYN             Out"` |
| `0x17` | Expand | `"Expander        In"` |
| `0x18` | Fast Attack (Gate) | `"Fast Attack     In"` — same label as Comp's Fast Attack (shared wording) |
| `0x19` | **Polarity** | `"Ø               In"` — **Polarity does fire `FF 22`**. Earlier (uc1_08/17/18) presses didn't register because user's physical contact was inconsistent, not because firmware suppresses the event. |
| `0x1A` | S/C Listen | `"S/C Listen      On"` |
| `0x1B` | Solo Clear | `"Solo Clear      Off"` |
| `0x1C` | Solo | `"Solo            On"` |
| `0x1D` | Cut | `"Cut             On"` |
| `0x1F` | Fine | `"Fine            On"` |
| `0x1E` | Channel IN | `"Channel Strip   Out"` |

Missing from this list: **Bus Comp IN** — part of the UC1's Bus Comp section (top center), not in user's 16-button Channel-Strip-focused list. `uc1_08` last press at t=17s was `0x0C` which was reported as "Bus Comp IN" by the user. Direct evidence for 0x0C = Bus Comp IN still needed.

## Implications

- Every uc1_08 button-ID assignment was reviewed; where direct evidence in uc1_19 conflicts with the uc1_08 guess, uc1_19 wins. The main overturn: `uc1_08`'s early ID-to-label alignment (0x0A = Bell HF, 0x0B = Type E, …) was wrong — 0x08/0x09/0x0A/0x0B is actually the HF Bell / EQ Type / EQ In / LF Bell cluster.
- **The user's pressed-button report was correct throughout.** Missing events in earlier captures (Polarity in uc1_17/18, various in uc1_08) were due to inconsistent physical contact, not wrong-button presses.
- Button IDs occupy the ranges `0x08`-`0x0B` (EQ section), `0x14`-`0x19` (Dyn section), and `0x1A`-`0x1F` (Channel section).

## Next step

`uc1_20` will repeat the same 16-button sequence with **SSL Native Channel Strip 2** loaded instead of 4K E, to test user's hypothesis that button IDs may remap per plugin (analogous to the top V-Pot knob repurposing).
