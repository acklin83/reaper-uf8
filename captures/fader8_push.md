# fader8_push — Fader 8 touch + drag on SSL 360° / Windows

Date: 2026-05-06
Windows host: 192.168.177.197 (USBPcap3, ssh-driven from macOS)
SSL 360°: running, UF8 connected, plugin-mixer mode
Action: Frank pressed Fader 8 + dragged ~7 times in 30 seconds.

## Why captured

Companion to `fader8_test.pcapng` — needed SSL 360°'s motor management for the moving case to confirm the conditional-re-enable hypothesis.

## Findings

```
inbound  ff 20 02 07 01 (touch press)   — 7×
inbound  ff 20 02 07 00 (touch release) — 7×
outbound ff 1d 02 07 00 (LIMP en=0)     — 8×
outbound ff 1d 02 07 01 (re-enable en=1) — 2×
outbound ff 1e 03 07 ... (motor target) — 292× (during drag)
```

**SSL 360° re-enables the motor sometimes after touch release — only when the touch included movement.** Combined with `fader8_test.pcapng` (touch-only: 0 re-enables), the rule is unambiguous: re-enable follows movement, not raw release.

## Resolution

See `fader8_test.md` and commit `b1adad9`. Conditional re-enable in `commitDebouncedTouchReleases` matches this captured behaviour.

The 8 LIMPs vs 7 touches and 2 re-enables vs 7 releases also reveal additional SSL 360° timing nuances (preemptive LIMP before next touch, occasional release-without-re-enable) but the basic "re-enable only after movement" rule is the load-bearing fix.
