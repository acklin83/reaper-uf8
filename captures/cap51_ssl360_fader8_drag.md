# cap51 — SSL 360° Fader 1 + Fader 8 touch + drag

Date: 2026-05-05
Windows host: 192.168.177.197 (USBPcap3 root hub, captured via SSH)
SSL 360°: running, UF8 connected via USB
REAPER: running, plugin mixer mode active

## Why
Resolve macOS-side fader regression: with our extension's `-1` strip shift, Fader 8 motor stayed engaged on touch and Fader 1 was silently broken. cap40b only captured Fader 1; we needed SSL 360°'s outbound motor commands for Fader 8 specifically to confirm the wire-byte convention.

## Action sequence
1. Touch + drag Fader 8 down ~5cm, hold ~1s, release
2. Touch + drag Fader 8 back up to top, release
3. Touch + drag Fader 1 down ~5cm, release (control sample)
4. Touch + drag Fader 1 back up, release

## Key findings — strip indexing is 0-INDEXED end-to-end

### Fader 1 (leftmost)
```
46.010256000  3.11.1 → host   31 60 ff 21 03 00 1f 62 a5  ff 20 02 00 01 23
46.011651000  host → 3.11.2   ff 1d 02 00 00 1f
```
- Inbound TOUCH: `ff 20 02 **00** 01` (rawStrip=0)
- Inbound POSITION: `ff 21 03 **00** 1f 62`
- SSL 360 LIMP response: `ff 1d 02 **00** 00`

### Fader 8 (rightmost)
```
34.138556000  3.11.1 → host   31 60 ff 21 03 07 18 62 a5  ff 20 02 07 01 2a
34.141593000  host → 3.11.2   ff 1d 02 07 00 26
```
- Inbound TOUCH: `ff 20 02 **07** 01` (rawStrip=7)
- Inbound POSITION: `ff 21 03 **07** 18 62`
- SSL 360 LIMP response: `ff 1d 02 **07** 00`

### Confirmation: motor target during drag
Long sequence of `ff 1e 03 07 <lsb> <msb> <ck>` frames at ~10ms cadence throughout the Fader 8 drag. Wire-byte stays at 0x07 — no rotation, no shift, no special wrapping.

## Resolution
Removed the `-1` shift in main.cpp's TOUCH handler. cap51 + macOS sweep (touch all 8 in sequence, only 7 distinct rawStrip values logged because Fader 1's rawStrip=0 was being rejected) confirmed: rawStrip is 0..7, used directly as internal strip index, unchanged in outbound motor commands.

See `extension/src/main.cpp` (FF 20 02 handler) and memory file `uf8-fader-input-protocol.md`.
