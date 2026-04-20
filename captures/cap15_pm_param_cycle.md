# cap15_pm_param_cycle

**Date:** 2026-04-20
**Session setup:** same as cap14a

**Action:** User pressed V-Pot assignment soft-keys on UF8 over ~20s, cycling through INPUT / FILTER / DYN / EQ / SEND.

**Key findings:**
- `FF 66 <len> 04 <strip> <text> CKSUM` = **Currently Selected Parameter** zone (was our slot-active placeholder)
  - len = text-length + 2
  - Examples per V-Pot assignment:
    - **INPUT:** BYPASS / IN TRIM / (empty) / PRE / MIC/DRIVE / (empty) / IMPEDANCE IN / IMPEDANCE
    - **FILTER:** WIDTH / (empty) / (empty) / A/B / HIGH PASS / LOW PASS / EQ / EQ TYPE
    - **EQ LOW:** LF FREQ / LF GAIN / LF TYPE / (empty) / LMF FREQ / LMF GAIN / LMF Q / (empty)
    - **EQ HIGH:** HMF FREQ / HMF GAIN / HMF Q / (empty) / HF FREQ / HF GAIN / HF TYPE

- `FF 66 15 0E <strip> <19 ASCII chars> CKSUM` = **Value Line** zone (single row combining label + value)
  - Examples:
    - "ChannelIn        In"
    - "In Trim       0.0dB"
    - "A/B              On"
    - "HF Freq     8.00kHz"
    - "LMF Q           1.5"
  - Format: left-justified label, right-justified value, spaces between.

- `FF 66 09 0D <8 bytes> CKSUM` = V-Pot Readout Bar position (1 byte per strip, broadcast-style single frame for all 8).
- `FF 66 11 0F <16 bytes> CKSUM` = another 8×2-byte broadcast (fader/meter bars?).
- Button events `FF 22 03 <id>`: IDs 0x18–0x1F = V-Pot push buttons per strip; 0x68–0x6B seen = top soft-keys.
- Page/layer selector `FF 1B 01 <XX>` takes values 0x00, 0x01, 0x02, 0x03 — one per Quick Key / page.
