# cap19_pm_bank_position — page-shift only, no bank shift

**Date:** 2026-04-20
**Session setup:** same as cap18

**Action:** User reported "bank shifts and one page shift" but the capture shows only page shifts (DYN sub-pages — GATE vs COMP parameters).

**What we got (page shift traffic):**
- `FF 66 <len> 04 <strip> <text>` Parameter Label zone with DYN-section names:
  - GATE page: GATE HOLD / GATE THRESHOLD / GATE RANGE / GATE HOLD / GATE ATTACK / GATE/EXP / HQ MODE / OUT TRIM
  - COMP page: DYNAMICS / COMP MIX / COMP RATIO / COMP THRESH / COMP RELEASE / COMP ATTACK / PEAK/RMS / —
- `FF 1B 01 <00|01|02|03>` layer/page selector cycling through all four pages.

**What we did NOT get:**
- No `FF 66 09 18` color re-push → no actual bank shift happened
- No `FF 66 06 17` CS Type change → tracks didn't rotate

**Interpretation:** the "Position" indicator in the PM-mode LCD layout (top-left, labeled "No") may update via `FF 66 02 0B <value>` (saw this in cap18 with values 1-7) but can't confirm without an actual bank shift capture.

**TODO:** redo as `cap19b_pm_bank.pcap` later — ensure REAPER has 16+ tracks with SSL plugins on all of them, trigger physical BANK→ / BANK← buttons on UF8 (NOT page-shift V-Pot buttons).
