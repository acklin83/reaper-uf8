# fader8_test — Fader 8 capacitive touch (no drag) on SSL 360° / Windows

Date: 2026-05-06
Windows host: 192.168.177.197 (USBPcap3, ssh-driven from macOS)
SSL 360°: running, UF8 connected, plugin-mixer mode
Action: Frank tapped Fader 8 ~22 times in 30 seconds without dragging.

## Why captured

After a full day of fruitless "Fader 8 sperrt" debugging on macOS — multiple hypotheses tried (-1 shift, wraparound, FF 33 02 secondary sensor) and reverted — needed the actual SSL 360° behaviour for touch-only Fader 8 to compare against our extension's `/tmp/reaper_uf8_motor.log`.

## Findings

```
inbound  ff 20 02 07 01 (touch press)   — 22×
inbound  ff 20 02 07 00 (touch release) — 22×
outbound ff 1d 02 07 00 (LIMP en=0)     — 22×
outbound ff 1d 02 07 01 (re-enable en=1) — 0×
```

**SSL 360° NEVER re-enables the motor on touch release when the user did not move the fader.** Compare with `fader8_push.pcapng` where the user dragged: that one has 2 re-enables for 7 touches.

## Resolution

Our extension's `commitDebouncedTouchReleases` was unconditionally sending re-enable 150 ms after every touch release. For brief taps, the user's finger was still on the strip when the timer fired, motor engaged, user perceived "sperrt". Fix gated the re-enable on `g_lastTouchPbValid` (= position event seen during touch ≈ user moved). Commit `b1adad9`.

Preserved memory: `uf8-fader-input-protocol.md` final state.
