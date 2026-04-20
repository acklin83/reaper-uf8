# cap17_pm_gain_reduction — inconclusive

**Date:** 2026-04-20
**Session setup:** same as cap14a + aggressive SSL Bus Comp on T1 (Threshold −30, Ratio 10:1)

**Action:** Intended to trigger REAPER playback so compressor would engage and push GR meter updates. User pressed Play on REAPER timeline.

**Result:** GR was active (user confirmed) but the 1344 `FF 13 04` meter frames went to **UC1 (USB addr 3.56)**, NOT UF8 (USB addr 3.55). SSL 360° routes Bus Compressor metering to UC1 (which has the dedicated GR display) when both controllers are present. UF8 saw only heartbeats during this capture.

**TODO:** redo as `cap17b_uf8_only_gr.pcap` with UC1 **disconnected**:
- unplug UC1 USB cable so SSL 360° has no choice but to route GR meters to UF8
- confirmed Bus Compressor on T1 with aggressive threshold
- audio playing, GR visible in REAPER
- capture 10 s while audio flows

`FF 13 04 <4 bytes>` turns out to be a UC1-specific command, not UF8 — ignore in UF8 analysis.

**User confirms (2026-04-20):** when UC1 is disconnected, the same GR data gets routed to UF8 instead — same meter payload, different destination. So the cap17b UC1-disconnect capture should yield the full `FF 13 04 …` frame family going to UF8 addr 3.55, and decoding will carry over.
