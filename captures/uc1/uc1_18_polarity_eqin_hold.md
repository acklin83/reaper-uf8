# uc1_18_polarity_eqin_hold

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

SSL **Native Channel Strip 2** loaded on the focused track (different plugin from `uc1_15`'s 4K E — CS 2 has Gate Hold, which 4K E doesn't). UC1 mirroring.

## Action

20 s capture. User pressed:
- Polarity 4×
- EQ IN 4×
- Gate Hold knob swept 4 times

## Summary

- 23 888 packets total on USBPcap3
- 2 button `FF 22` press events, both id `0x0A`
- 674 knob events, all id `0x18`
- Polarity: 0 events — reconfirms Polarity doesn't emit `FF 22` frames

## Conclusions

| ID | Role | Direct evidence |
|----|------|-----------------|
| button `0x0A` | **EQ IN** | Zone 0x03 display toggles `"EQ              Out"` → `"EQ              In"` per press. Only 2 of 4 user-reported presses generated events — the other 2 may not have registered physically. |
| knob `0x18` | **Gate Hold** (confirmed with CS 2 loaded) | Zone 0x03 shows `"Hold            0.34s"` … `"4.0s"` … back to `0.00s`, tracking the physical knob position continuously. With 4K E loaded (uc1_15) the same knob fired events but received no display updates because 4K E has no Gate Hold param. |
| Polarity | **Does not fire `FF 22`** | 4 physical presses, 0 events. Confirmed non-emitter across uc1_08, uc1_17, uc1_18. |

### Button-ID table correction (important)

`uc1_08`'s 11-button sequence-alignment put `0x0A` as "Bell HF" (user's position #1). `uc1_18` direct evidence proves `0x0A = EQ IN`. The likely explanation is that in `uc1_08` the user's physical press order deviated from the reported order — either "EQ IN" was pressed at position #1 instead of "Bell HF", or the user's recall of which hardware button was which had minor mislabels.

**Button IDs now confirmed by direct display evidence:**

| ID | Button | Source |
|----|--------|--------|
| `0x0A` | EQ IN | uc1_18 — "EQ In/Out" display |
| `0x1A` | S/C Listen | uc1_14 — "S/C Listen" display |
| `0x1B` | Solo Clear | uc1_17 — "Solo Clear Off" display |
| `0x1C` | Solo | uc1_17 — "Solo On" display |

**IDs still only inferred by `uc1_08` sequence alignment (likely unreliable):** `0x0B`, `0x0C`, `0x14`, `0x15`, `0x16`, `0x17`, `0x18`, `0x19`. Their candidate labels from user's reported sequence — Type (E), Bus Comp IN, Bell LF, Fast Attack, Peak, Dyn In, Expand, Fast Attack (Gate) — should be re-verified with narrow per-button captures if Rea-Sixty implementation hits any specific button for handling. For now, none of them need rigid labeling until a consumer in the code requires it.

**Button that doesn't emit `FF 22`:** Polarity.
