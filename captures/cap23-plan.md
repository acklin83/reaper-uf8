# cap23 — systematic LED ID enumeration

## Why

cap22 (2026-04-21) decoded the LED command format (`FF 3B 03 <id> 00
<state> CKSUM`) but left the `id` → (strip, button) mapping ambiguous.
The 7 IDs observed (0x03, 0x05, 0x09, 0x0B, 0x0F, 0x12, 0x14) didn't
line up cleanly with the REAPER-mixer actions performed during that
capture. Live test with MCU-note-style ID (ARM=0x00+s, SOLO=0x08+s,
MUTE=0x10+s, SEL=0x18+s) produced scrambled cross-strip LED behavior:

| REAPER action | UF8 LED lit |
|---|---|
| Solo Ch1 | Solo Ch6 |
| Cut Ch1  | Cut Ch3 |
| Solo Ch2 | Sel Ch5 |
| Mute Ch2 | Solo Ch3 |
| Solo Ch3 | Cut Ch5 |
| Solo Ch4 | Solo Ch5 |
| Cut Ch4  | Cut Ch2 |
| Cut Ch5  | Solo Ch2 |
| Solo Ch6 | Cut Ch4 |
| Cut Ch6  | Sel Ch1 |

No clean stride / offset fits. The mapping is either permuted
(strip-major with different stride, or indexed through a lookup), or
it's layer-gated (LED command works differently in PM Layer vs DAW
Layer).

## Capture setup

- UF8 on **DAW Layer (Layer 1)** — NOT PM. Same layer as cap22 so the
  LED command actually fires.
- SSL 360° running.
- REAPER with Mackie Control Universal surface on SSL V-MIDI Port 1.
- 8 tracks visible, all idle (no solos, mutes, selections, arms).

## Procedure

Systematic pass through every (strip, button_class, on) triple. Aim for
one isolated action every ~2 seconds so the per-action LED frame is
easy to time-align.

```
Solo track 1 on / off
Solo track 2 on / off
...
Solo track 8 on / off
Mute track 1 on / off
...
Mute track 8 on / off
Select track 1 (click track 1 panel)
Select track 2
...
Select track 8
(switch to REC selection mode on UF8)
Rec Arm track 1 on / off
...
Rec Arm track 8 on / off
```

8 × 2 × 4 classes = 64 events. ~2 min capture at 2 s/event pace.

## Analysis plan

After capture, produce:

```
python3 analysis/parse_usbpcap.py cap23_led_enum.pcapng \
  --baseline ... \
  | grep "^ff3b03"
```

Pair each LED frame with the nearest REAPER action time. Produce a
table in `docs/protocol-notes.md`:

| Strip | ARM id | SOLO id | MUTE id | SEL id |
|-------|--------|---------|---------|--------|
| 0 (Ch1) | ? | ? | ? | ? |
| 1 (Ch2) | ? | ? | ? | ? |
| ...     |   |   |   |   |

Then refine `sendMcuLed` to use that exact map and re-enable LED
feedback in the extension.

## cap24 — V-Pot Readout Bar encoding (same session)

Parallel target since the UF8 is on Windows anyway. cap20 identified
`FF 66 11 0F <16 bytes>` as the V-Pot bar command but two encoding
attempts failed:
- byte[0] = linear 0..255 → flicker + partial rendering
- byte[0] = bit-mask (1<<n)-1 fill → renders nothing

Need a capture with SSL 360° driving the bar to **specific known param
values** (not just raw V-Pot rotation deltas that produce a running
counter).

### Setup
- UF8 on **PM Layer (Layer 2)**, 4K E on Track 1
- Camera-record the UF8's V-Pot bar (LED segments) throughout
- Start capture

### Procedure
Set the HF Gain knob to these exact normalized values in REAPER (via
typed value entry on the plug-in GUI so we know the exact norm):
- 0.00 (full CCW, -15 dB)
- 0.125 (≈-11 dB)
- 0.25 (≈-7.5 dB)
- 0.375 (≈-3.7 dB)
- 0.50 (0 dB, center)
- 0.625 (+3.7 dB)
- 0.75 (+7.5 dB)
- 0.875 (+11 dB)
- 1.0 (full CW, +15 dB)

Wait 1 second at each value so the bytes settle. Record the camera
state for each. Stop capture.

### Analysis
- Extract `FF 66 11 0F` frames, isolate strip 0 bytes at each settled
  moment.
- Cross-reference: for normalized 0.5, byte = ??; for 1.0, byte = ??; etc.
- Build a lookup table or formula. Encode same in `vpotPosFromNormalized`.

## Bonus: PM-Layer LED test

After cap23 decodes the DAW-Layer ID map, run a quick test: switch UF8
to PM Layer (Layer 2) and send the same commands. If LEDs don't light
in PM mode, LED feedback is layer-gated and we have to choose between
(a) always run in DAW Layer (lose scribble colors) or (b) switch layers
on-the-fly (complex). If LEDs DO light in PM, we just wire the DAW-Layer
IDs and we're done.
