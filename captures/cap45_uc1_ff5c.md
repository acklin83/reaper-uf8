# cap45 — UC1 FF 5C disambiguation: BC-only? Position-fixed?

**Date:** 2026-04-28
**Interface:** `\\.\USBPcap1`
**Devices in stream:** UC1 (VID 31e9 PID 0023, addr 33), UF8 (VID 31e9 PID 0021, addr 37)
**Size:** ~7.5 MB

## Why

cap43 left two questions about `FF 5C 02 00 <pos> <ck>` (the mystery sibling of `FF 5B`):

1. Does it fire only on BC bypass, or also on CS/EQ/Dyn bypass?
2. Are the position bytes (0x0A on press, 0x32 on un-press) fixed cosmetic poses, or do they vary with current GR / context?

cap43's sweep made it impossible to isolate FF 5C from the streamed FF 5B GR motor frames. cap45 deliberately runs **without** a GR sweep to keep FF 5B traffic at idle (position 0).

## Action timeline

1. ~3 s idle (baseline).
2. **BC In → Bypass** (press #1), hold ~2 s.
3. **BC Bypass → In**, hold ~2 s.
4. **BC In → Bypass** (press #2), hold ~2 s.
5. **BC Bypass → In**, hold ~2 s.
6. ~3 s idle.
7. **CS Channel In → Off** (CS section bypass), hold ~2 s.
8. **CS Channel In → On**, hold ~2 s.
9. **EQ In** off → on toggle.
10. **Dyn In** off → on toggle.

## Findings

### FF 5C fired EXACTLY 4 times, at the 4 BC bypass-toggle timestamps

| t (s) | frame | inferred press |
|---|---|---|
| 30.81 | `FF 5C 02 00 0A 68` | BC bypass-on  #1 |
| 34.45 | `FF 5C 02 00 32 90` | BC un-bypass  #1 |
| 37.73 | `FF 5C 02 00 0A 68` | BC bypass-on  #2 |
| 40.93 | `FF 5C 02 00 32 90` | BC un-bypass  #2 |

- **Position bytes are fixed**: always `0x0A` on bypass-press, always `0x32` on un-bypass-press. Not GR-dependent.
- **Single frame per toggle** — fire-and-forget, no streaming.
- Each FF 5C **coincides exactly with the BC VU-backlight binary-toggle** (cell `bank=0x02 cell=0x01 byte5=0x01`) at the same timestamp.

### CS / EQ / Dyn bypass produced ZERO FF 5C frames

The post-42s window (CS Channel In, EQ In, Dyn In toggles) shows clear LED-cascade activity (cells `0x83`, `0x78`, `0x6D`, `0x63`, `0x57`, `0x4A`, `0x3F`, `0x34` etc. toggling between `0x0A` and `0x33` brightness states), confirming the user did press these buttons. But not a single `FF 5C` was emitted.

→ **FF 5C is exclusively a BC-bypass cosmetic command.** Other section bypasses do not use it. The mechanical needle is BC-only hardware, so this is consistent.

### Side discovery: CS bypass dim level = 0x0A, not 0x33

CS Channel In → Off pushed many CS-section cells to brightness `0x0A` (= 4%). When CS came back on, they went to `0x33` (= 20%). This contradicts our current cascade-dim implementation that uses `0x33` everywhere.

Hypothesis: SSL uses **per-section dim levels**. BC bypass dims to `0x33`. CS bypass dims to `0x0A`. EQ-Off / Dyn-Off may be other levels still. Worth a follow-up capture to characterize all four cascades.

## Conclusion for FF 5C

The frame is a **cosmetic single-shot needle-pose** sent on every BC-bypass-state-change press, with:

- Position `0x0A` (= 10 = ~1 dB GR equivalent on the 0..200=0..20 dB scale) when transitioning **into** bypass.
- Position `0x32` (= 50 = ~5 dB GR equivalent) when transitioning **out of** bypass.

These are likely the firmware's "park" positions for the analog needle — a quick pose to acknowledge the state change, since FF 5B streaming is briefly absent during the click event. To match SSL behaviour, our extension should emit:

- `FF 5C 02 00 0A 68` once when `bypassParam` transitions 0 → 1
- `FF 5C 02 00 32 90` once when `bypassParam` transitions 1 → 0
- ... and continue normal `FF 5B` streaming based on real GR data thereafter.

Skipping FF 5C entirely would still yield a working meter, but lose the cosmetic acknowledgement blip.
