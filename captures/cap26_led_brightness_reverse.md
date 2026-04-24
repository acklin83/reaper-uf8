# cap26_led_brightness_reverse

Date: 2026-04-24
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8 device address: 64
UC1: physically disconnected
SSL 360°: running, UF8 selected

## Session state

Continuation of cap25. Brightness slider started at **full**.

## Action

15 s capture. Slider clicked in reverse: full (start) → bright → half → dim → dark, ~1.5–2 s per step.

## Summary

Novel OUT: 8 frames (4 transitions × 2 frames). Novel IN: 4 incidental events.

## Complete brightness value table

Combining cap25 (forward) + cap26 (reverse):

| Step | `FF 2D 08 00 00 <b> 00 <b> 00 <b> 00 <chk>` | `FF 4F 02 <b> 00 <chk>` |
|------|---------------------------------------------|--------------------------|
| dark   | `0x05` | `0x18` |
| dim    | `0x0A` | `0x30` |
| half   | `0x10` | `0x50` |
| bright | `0x13` | `0x60` |
| full   | `0x20` | `0xA0` |

Ratio `FF 4F / FF 2D` ≈ 4.8× — two independent scales, always coupled in transmission.

## Interpretation (finalised)

Two brightness commands, both driven by the single SSL 360° "brightness" slider:

- **`FF 2D 08 00 00 <b1> 00 <b2> 00 <b3> 00 <chk>`** — LED master brightness. Three brightness channels; observed identical across all 5 steps. Probably per-region or per-RGB-channel with SSL 360° currently driving them in lockstep. We can safely write all three equal for now; probe separately if we ever want region-specific dimming.
- **`FF 4F 02 <b> 00 <chk>`** — LCD / scribble backlight brightness.

Reverse sweep confirms SSL just re-sends the target state each step — no differential encoding.

## Raw novel frames

```
  0.7953s  EP2 OUT  len=12  ff2d0800001300130013006e   # → bright
  0.7953s  EP2 OUT  len= 6  ff4f026000b1
  2.4278s  EP2 OUT  len=12  ff2d08000010001000100065   # → half
  2.4279s  EP2 OUT  len= 6  ff4f025000a1
  4.4224s  EP2 OUT  len=12  ff2d0800000a000a000a0053   # → dim
  4.4225s  EP2 OUT  len= 6  ff4f02300081
  6.1223s  EP2 OUT  len=12  ff2d08000005000500050044   # → dark
  6.1224s  EP2 OUT  len= 6  ff4f02180069
```

## Next

Brightness decode complete. Proceed to cap27 — SEL-follows-track-colour toggle.
