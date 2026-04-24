# cap25_led_brightness

Date: 2026-04-24
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8 device address: 64
UC1: physically disconnected
SSL 360°: running, UF8 selected

## Session state

REAPER open, one track, UF8 in PM mode. SSL 360° UF8 device-settings panel open with LED brightness slider at **dark**.

## Action

20 s capture. User clicked the brightness slider in five steps: dark (start) → dim → half → bright → full, ~2 s per step.

## Summary

- Baseline: 11 unique payloads (heartbeats), `cap24_idle_baseline_v2.pcapng`.
- Novel events: 8 OUT frames (4 step transitions × 2 frames per step) + 5 IN frames (non-brightness; likely incidental device status).
- `FF 2D 08` carries three repeating brightness bytes at offsets 4/6/8; `FF 4F 02` carries a single brightness byte at offset 3. Both fire on every step.

## Decoded value table (4 of 5 steps)

| Step | `FF 2D 08 00 00 XX 00 XX 00 XX 00 chk` | `FF 4F 02 YY 00 chk` |
|------|---------------------------------------|-----------------------|
| dark | unknown — starting state, no event fired |                       |
| dim  | `0x0A` | `0x30` |
| half | `0x10` | `0x50` |
| bright | `0x13` | `0x60` |
| full | `0x20` | `0xA0` |

## Interpretation

Two independent brightness commands, both driven by the single "brightness" slider in SSL 360° (confirmed by user: slider affects LEDs AND displays):

- **`FF 2D 08 00 00 <b1> 00 <b2> 00 <b3> 00 <chk>`** — 12-byte frame. Three brightness channels, identical across all four observed steps. Most likely the LED master brightness (per-RGB-channel or per-region; can't disambiguate yet because all three bytes always match in the observed captures).
- **`FF 4F 02 <b> 00 <chk>`** — 6-byte frame. Single brightness byte. Most likely LCD/scribble-strip backlight.

Both frames share the checksum convention (`chk = sum(header + payload) mod 256`).

## Next capture to plan

cap26 — reverse sweep "full → dark" at ~2 s per step. Goal: capture the "dark" endpoint values (and confirm the transition in the opposite direction is symmetric, i.e. 360° just re-sends the target state without differential encoding).

## Raw novel frames

```
 0.0000s  EP2 OUT  len=12  ff2d0800000a000a000a0053
 0.0001s  EP2 OUT  len= 6  ff4f02300081
 2.8496s  EP2 OUT  len=12  ff2d08000010001000100065
 2.8497s  EP2 OUT  len= 6  ff4f025000a1
 5.4826s  EP2 OUT  len=12  ff2d0800001300130013006e
 5.4827s  EP2 OUT  len= 6  ff4f026000b1
 8.0503s  EP2 OUT  len=12  ff2d08000020002000200095
 8.0504s  EP2 OUT  len= 6  ff4f02a000f1
13.0855s  EP1 IN   len= 9  3160ff210303605ce3   # non-brightness, probably incidental
13.1055s  EP1 IN   len= 9  3160ff210303405cc3
13.1154s  EP1 IN   len= 9  3160ff210303805c03
16.2948s  EP1 IN   len= 9  3160ff2103039f5c22
16.3148s  EP1 IN   len= 9  3160ff2103037f5c02
```

The EP1 IN events at 13 s and 16 s don't correlate with any brightness step — likely the user accidentally touched a V-Pot or the capture picked up a soft-key press during idle. Not part of brightness protocol.
