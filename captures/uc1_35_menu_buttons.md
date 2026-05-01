# uc1_35_menu_buttons.pcapng

Captured 2026-05-01 to decode the IDs of the UC1 navigation/menu buttons
that hadn't been mapped yet.

## What was captured

20-second tshark capture on `\\.\USBPcap3` (UC1 + UF8 share the bus).
User pressed the six menu-row buttons in this order, each held ~150-250 ms
with ~2.5 s gap between presses:

1. Back
2. Confirm
3. Routing
4. Presets
5. 360 (the central UC1 360 knob press)
6. Magnifier

SSL360Core was running and driving the UC1, so the device sent its usual
`FF 22 03 <id> 00 <state>` button events on EP 0x81 IN.

## Decoded IDs

Filter: `usb.src contains "3.41" and usb.capdata`, slice to `3160ff…`
preamble + `ff2203` payload prefix. Every press produced a clean
press(state=01) + release(state=00) pair on the same id.

| Button     | id     |
|------------|--------|
| Back       | `0x0E` |
| Confirm    | `0x0F` |
| Routing    | `0x10` |
| Presets    | `0x11` |
| 360 (knob) | `0x12` |
| Magnifier  | `0x13` |

Wired into `extension/src/UC1Protocol.h` as `button::kBack` …
`button::kMagnifier`.
