# uc1_17_polarity_soloclear

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

4K E on the focused track. UC1 mirroring.

## Action

20 s capture. User pressed Polarity 4×, then Solo 4× and Solo Clear 4× alternating (actual motion was Solo → Solo Clear → Solo → Solo Clear etc.).

## Summary

- 22 500 packets total on USBPcap3
- 16 novel IN events: 4× `0x1C` + 4× `0x1B` (press/release pairs, alternating)
- Polarity 4 presses produced **no** IN events

## Conclusions (confirmed via zone 0x03 display text)

| `button_id` | Button | Display text during press |
|------------:|--------|---------------------------|
| `0x1C` | **Solo** | `"Solo            On"` |
| `0x1B` | **Solo Clear** | `"Solo Clear      Off"` |

**Polarity continues to produce no `FF 22` event**, across `uc1_08`, `uc1_17`, and later `uc1_18` — confirmed as a button that doesn't generate device events on this firmware path.

`FF 13 04 <bank> <cell> <01> <bits>` frames also fire on Solo/Solo-Clear transitions, suggesting this command carries both meter data and non-meter LED/state writes:
- Solo on: `01 9a 01 01` + `02 97 01 ff`
- Solo clear: `01 9a 01 03` + `01 9a 01 00` + `02 97 01 00`
