# cap16_pm_fader_dB

**Date:** 2026-04-20
**Session setup:** same as cap14a

**Action:** Moved T1 (DRM) fader slowly from −∞ through 0 to +12 dB and back, over ~15 s.

**Key findings:**
- `FF 66 0A 0C <strip> <4 ASCII> 00 00 64 42 CKSUM` = **O/PdB Fader Readout zone**
  - len = 0x0A (10 bytes payload after len, before cksum)
  - subcmd = 0x0C
  - strip = 0-indexed strip byte (0x00 in this capture since we only moved T1)
  - 4 ASCII chars = the numeric fader value, left-padded/right-padded with NUL (`2d 30 2e 31` = "−0.1")
  - 2 NUL bytes
  - `64 42` = ASCII "dB"
  - Total frame: 14 bytes
  - Examples observed: "−0.1dB", "−0.2dB", ..., "−1.3dB", "0.0dB" etc.

- `FF 66 08 0C <strip> <5 bytes> CKSUM` appears paired with 0A 0C. Content pattern: incrementing 0x04, 0x05, 0x06, 0x07 as first byte — likely V-Pot Readout Bar filling level (8 bytes total → fader position encoded across the arc).

- `FF 1E 03 <strip> <LSB> <MSB>` already known (fader motor position, bidirectional).

- `FF 13 04 <4 bytes> CKSUM` appears during fader activity (81×). Unknown purpose; possibly an automation-state indicator (first byte varies in 0x01 region). Not in the main LCD zones — deferred.
