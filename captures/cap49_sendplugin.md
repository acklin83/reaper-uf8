# cap49_sendplugin

**Date:** 2026-04-30
**Action:** User pressed Send/Plugin 1..8 sequentially while SSL360°
drove the surface. Goal: confirm Send/Plugin LED frame format.

## Findings

- Send/Plugin 1..8 LED cells = `0x37..0x30` (descending — SP1 = 0x37,
  SP8 = 0x30) — original cap35/36 decode was correct on cell IDs.
- The frame family is **both** colour-pair (FF 38/39 04) **and** legacy
  mono (FF 3B 03), sent together — same 3-state model as Page LEDs:
  - **BRIGHT** (button held): `FF 38 04 <cell> 00 FF FF` + `FF 39 04
    <cell> 00 00 F0` + `FF 3B 03 <cell> 00 01`
  - **DIM** (button released, baseline): only `FF 38/39 04 <cell> 00
    11 F1` (no legacy)
- User observation during capture: "leuchten hell, nicht farbig
  (obwohl möglich)" — SSL360 ships SP LEDs in white only despite the
  hardware supporting full RGB.

## Why our earlier probe missed this

Our first probe sent `FF 38/39 04 <cell> 00 FF FF / 00 F0` alone for
cells 0x30..0x37 — no LED lit. We then tried `FF 3B 03 <cell> 00 01`
alone, which DID light the LED. The conclusion at the time was "SP is
legacy-only". Wrong: SSL pairs both formats. The legacy frame alone is
enough to flip the on/off bit, but the matching colour-pair sets the
baseline colour (white-bright vs white-dim) the firmware should
display once the bit is set.
