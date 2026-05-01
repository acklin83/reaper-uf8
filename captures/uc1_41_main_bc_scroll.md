# uc1_41 — MAIN-mode Sec-Encoder BC track scroll

Captured 2026-05-01 over USBPcap1 (UC1 on Windows; SSL360 driving the
unit) while the user rotated the secondary encoder back-and-forth in
MAIN mode. Decoded the previously-undocumented "BC scroll" overlay on
the central LCD.

## Findings

* Banner during BC scroll: `FF 66 03 00 01 02 6c` — same MAIN mode byte
  (0x01) as idle, but a SUB-MODE byte goes from 0x00 → 0x02. SSL360
  sends sub=0x02 on every detent and reverts to sub=0x00 ~1.5 s after
  the last detent.
* Header during overlay: `FF 66 0b 01 "BUS COMP 2"` (kind=0x01, same
  slot as `buildCentralLabel` so it OVERWRITES the regular
  "MAIN"/"CS 2" label).
* Triple kind=0x04 (large 3-slot, 14-char each) carries the
  prev/curr/next BC-bearing track names — same frame our extension was
  already emitting via `buildTrackNameTripleLarge` in refresh().
* Revert sequence: banner sub=0x00 + header back to "MAIN" (or whatever
  buildCentralLabel computes). Triple persists with the latest BC
  carousel content.

## Implementation derived from this capture

* `buildCentralMode(mode, subMode=0x00)` — added the optional sub-mode
  byte parameter.
* `bcScrollOverlayActive_` flag + `bcScrollOverlayUntil_` deadline on
  UC1Surface. BC encoder handler sets it; refresh()'s
  central-label branch picks "BUS COMP 2" instead of buildCentralLabel
  while active; poll() reverts after 1.5 s idle.
