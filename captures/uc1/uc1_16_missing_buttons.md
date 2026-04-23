# uc1_16_missing_buttons

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

Same session as `uc1_15`. 4K E still loaded on the focused track.

## Action

15 s capture. User pressed **EQ IN** 5× and **Solo Clear** 5×, one after the other (~1 s between presses).

## Summary

- 16 990 packets total on USBPcap3
- 8 novel IN events — all `FF 22 03 1B 00 <state>`, i.e. button id `0x1B`
- 0 IN events for the second button (Solo Clear)

Four press/release cycles of 0x1B observed (instead of 5 expected — one physical press may not have registered). Solo Clear produced **no `FF 22` events whatsoever** during its 5 presses.

## Conclusions

- **Button 0x1B = EQ IN** (direct evidence — this is the only button fired in this capture)
- **Solo Clear does not generate `FF 22` button events** — the physical button either uses a different command family, is handled internally by SSL 360° (and only surfaces as mouse-side state in the 360° UI), or the event path is routed differently. A narrow follow-up capture watching ALL command bytes (not just 0x22) during Solo Clear presses would confirm which.
- Paired with `uc1_14` direct evidence for 0x1A = S/C Listen, the uc1_08 alignment that previously assigned 0x1B = Polarity was wrong. Polarity's ID remains unknown and is likely also in the "no `FF 22` event" class.
