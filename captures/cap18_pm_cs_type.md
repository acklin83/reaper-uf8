# cap18_pm_cs_type

**Date:** 2026-04-20
**Session setup:** T1=CS 2, T2=4K B, T3=4K E, T4=CS 2 (user reloaded plugins between cap14a and cap18)

**Action:** User pressed UF8 Quick Key 1 (Channel Strip Mode) ↔ Quick Key 2 (Bus Compressor Mode) a few times to trigger state re-push.

**Key finding — Channel Strip Type zone decoded:**

```
FF 66 06 17 <strip> <4 ASCII chars> CKSUM
```
Examples from capture:
```
ff66 06 17 00 43 53 20 32 6b   → strip 0: "CS 2"
ff66 06 17 01 34 4b 20 42 65   → strip 1: "4K B"
ff66 06 17 02 34 4b 20 45 69   → strip 2: "4K E"
ff66 06 17 03 43 53 20 32 6e   → strip 3: "CS 2"
```
- Total frame: 9 bytes
- len = 0x06 (6 payload bytes: subcmd + strip + 4 text chars)
- subcmd = 0x17
- Text is always 4 chars, space-padded for shorter names ("CS 2 " is actually "CS 2\0" or similar — verify)

**Other content in cap18:**
- Bus Compressor parameter labels in `FF 66 15 0E`: "Threshold    20.0dB", "Ratio         6.0:1", etc. Confirms Value Line zone decoding.
- `FF 66 0A 04 00 "DYNAMICS"` — longer Parameter Label showing section header.
- `FF 38/39 04` meter frames reappear when Quick Keys pressed — these ARE real meters (we deferred decoding in commit 1a9a688).
