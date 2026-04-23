# uc1_20_buttons_cs2

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

**SSL Native Channel Strip 2** (CS 2) loaded on the focused track (replacing the 4K E from `uc1_19`). UC1 mirroring. No track solo'd at start.

## Action

60 s capture. Same 16-button sequence as `uc1_19` (one press per button, ~3 s apart), same order:

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

- 67 372 packets total on USBPcap3
- **15 press events** (vs. 16 expected): `0x08` (HF Bell) and `0x09` (EQ Type) did **not** emit `FF 22` events with CS 2 loaded. Plus one double-bounce on `0x16` around t=8.5 s (user's finger hit twice or a bounce/tshark-artifact — treated as one logical press).
- 14 of the 16 buttons fired events with stable IDs matching `uc1_19`

## Direct-evidence alignment (CS 2)

| user press # | expected | observed id | observed display | match? |
|--:|----------|-----------:|-----------------|:------:|
| 1 | HF Bell | — (no event) | — | silent with CS 2 |
| 2 | EQ Type | — (no event) | — | silent with CS 2 |
| 3 | EQ In | `0x0A` | `"EQ              Out"` | ✓ |
| 4 | LF Bell | `0x0B` | `"LF Bell         In"` | ✓ |
| 5 | Fast Attack (Comp) | `0x14` | `"Fast Attack     In"` | ✓ |
| 6 | Peak | `0x15` | `"Peak            In"` | ✓ (display appears on CS 2; 4K E had none) |
| 7 | Dyn In | `0x16` | `"DYN             In"` | ✓ |
| 8 | Expand | `0x17` | `"Expand          In"` | ✓ |
| 9 | Fast Attack (Gate) | `0x18` | `"Fast Attack     In"` | ✓ |
| 10 | Polarity | `0x19` | `"Ø               In"` | ✓ |
| 11 | S/C Listen | `0x1A` | `"S/C Listen      On"` | ✓ |
| 12 | Solo Clear | `0x1B` | `"Solo Clear      Off"` | ✓ |
| 13 | Solo | `0x1C` | `"Solo            On"` | ✓ |
| 14 | Cut | `0x1D` | `"Cut             On"` | ✓ |
| 15 | Fine | `0x1F` | `"Fine            On"` | ✓ |
| 16 | Channel IN | `0x1E` | `"Channel Strip   Out"` | ✓ |

## Hypothesis test result

**User's hypothesis ("button IDs remap per plugin, like the V-Pots do") is not supported.** Button IDs are stable across 4K E and CS 2. What **does** vary by plugin:

- **Event emission**: some buttons are gated by whether the underlying plugin implements a matching param. With CS 2, HF Bell and EQ Type emit no events. (CS 2's EQ has no HF bell/shelf toggle — it's a 4-band with different band typing — and its "EQ Type" is documented as "In/Out", which is functionally the same as EQ In, so SSL 360° likely suppresses the duplicate.)
- **Display feedback per button**: same event can yield different display text (or no display) depending on plugin. Peak button fires display on CS 2 but not on 4K E — matching user's note that Peak is "nur verwendet von CS2".

## Implementation implication

Rea-Sixty can use **one stable button-ID table** for all plugins. To handle plugin-gated buttons correctly, pass-through is the safe default — forward every press to the currently focused plugin's param (if mapped) and simply not crash when the plugin has no corresponding param.
