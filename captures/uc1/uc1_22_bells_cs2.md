# uc1_22_bells_cs2

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

**SSL Native Channel Strip 2** loaded on the focused track (same as `uc1_20`). UC1 mirroring.

## Action

30 s capture. User pressed **HF Bell 10×** then **LF Bell 10×** to force registration of the buttons that went silent in `uc1_20`.

## Results

| Button | ID | Presses reported | `FF 22` events captured | Display text | LED cell |
|--------|----|-----------------:|------------------------:|--------------|----------|
| HF Bell | `0x08` | 10 | 8 | `"HF Bell         In"` / `"Out"` toggling | `0x02 0x89` |
| LF Bell | `0x0B` | 10 | 10 | `"LF Bell         In"` / `"Out"` toggling | `0x02 0x23` |

## Conclusion

- `uc1_20`'s "HF Bell and EQ Type silent with CS 2" finding was **wrong** — caused by inconsistent physical contact on two presses in that specific take, not by plugin gating. CS 2 supports HF Bell fully (and presumably EQ Type and LF Bell too).
- HF Bell's LED cell is `0x02 0x89` — this was the last missing entry in the per-button LED cell table.
- No revision needed to the "event emission is plugin-gated" principle as a *theoretical* possibility (e.g. a plugin that genuinely lacks a param may well produce no event for that button), but specifically for CS 2 + HF Bell it's not happening.
