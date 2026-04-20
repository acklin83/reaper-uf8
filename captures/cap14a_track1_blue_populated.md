# cap14a_track1_blue_populated

**Date:** 2026-04-20
**Windows host:** 192.168.177.197 (via SSH)
**SSL 360°:** running in PM mode (SSL360Core 11276, SSL360Gui 2292)
**REAPER:** running (PID 10700)

**Session setup:**
- 16 REAPER tracks named DRM, BASS, GTR, VOC, BVOC, KEYS, BUS, then empty
- T1–T3: SSL 4K Channel Strip
- T4: CS 2
- T5: BusComp
- Various REAPER track colors

**Action:** At ~T=3s, changed Track 1 (DRM) color from previous to **blue** in REAPER.

**Key findings:**
- Color command fired: `FF 66 09 18 05 05 03 06 00 00 00 00 9A`
  - Strip 0 (T1 DRM) = 0x05 ← **new palette index for REAPER "blue" #009FD5**
  - Strip 1 (T2 BASS) = 0x05 (also blue)
  - Strip 2 (T3 GTR) = 0x03 (green ✓)
  - Strip 3 (T4 VOC) = 0x06 (magenta)
  - Strips 4–7 = 0x00 (no SSL Channel Strip → no color rendered)
- Palette correction: our hardcoded `0x04 = blue` was wrong. `0x04` is a different shade. **`0x05` is the REAPER default "blue".**
- Single color change triggers ~10 state frames besides the color cmd: `FF 66 02 11 05`, `FF 1B 01 02`, fader events, meter updates.
