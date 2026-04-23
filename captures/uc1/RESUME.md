# Rea-Sixty UC1 — Resume Point

**Last commit:** `ede90ec` on `main`, pushed.
**Phase status:** UC1 implementation is live and working on hardware. Core feature set complete; nice-to-haves remain.

## What works end-to-end (tested on macOS with real UC1)

- **Device lifecycle**: handshake (FF 01/02/05/4B/4E) + 1394-frame LED init flood, 50 Hz GR stream as liveness heartbeat, 150 ms FF 1B keepalive. UF8 optional in the same extension instance.
- **Bus Comp 2 V-Pots** (all 7, IDs 0x0E/0x0F/0x10/0x11/0x12/0x13/0x14) drive the right VST3 params with display readout at position 16 in zone 0x05. Values compact ("12.1dB" not "12.1 dB"), non-numeric tokens drop the unit ("OFF" not "OFFHz").
- **Channel Strip knobs** (4K E / CS 2) drive VST3 params with display readout in zone 0x03.
- **Channel Strip buttons** (16 total, IDs 0x08..0x1F) toggle plugin params + LED feedback via `FF 13 04 02 <cell> 01 <0xFF/0x00>`.
- **Channel IN / Bus Comp IN** toggle `TrackFX_SetEnabled` bypass.
- **Track name** pushed to zone 0x02 (CS slot) and zone 0x04 (BC slot). CS slot always shows the focused-track name; BC slot only when a Bus Comp plugin is loaded.
- **CHANNEL encoder (0x0D)** scrolls REAPER tracks, 4 ticks per click, direction-change + 100 ms timeout accumulator reset.
- **BC encoder (0x15)** jumps to nearest BC-hosting track relative to current focus, 3 ticks/click.
- **7-segment display** shows REAPER track index (001..099 correct, 100..199 best-effort). Hundreds digit still partial decode — cells 0x00/0x03/0x04/0x05 light for "1"; cells 0x01/0x02/0x06/0x07 always cleared to wipe init residue.

## Critical protocol details

- Frame: `FF <cmd> <len> <data> <chk>`, chk = sum(cmd+len+data) mod 256.
- `FF 13 04 <bank> <cell> <byte3> <state>`:
  - byte3 = `0x01` → button/VU LEDs, state `0xFF` on / `0x33` dim / `0x00` off
  - byte3 = `0x00` → 7-seg, state `0x01` on / `0x00` off
- Display zone widths: CS zone 0x03 uses 22 chars ASCII. Zone 0x02 has CS track name at byte 12. Zone 0x04 has BC track name at byte 14.
- Plugin-bypass: LEDs dim to `0x33`, not off.

## Outstanding / nice-to-have

- Hundreds digit of 7-seg: full per-segment decode (only 4 of 7 cells known)
- GR-JSFX-Probe → `pushGainReduction()` wiring (JSFX lives at `extension/jsfx/rea_sixty_gr_probe.jsfx`)
- VU meter feed (track peaks → `pushVu(meter, level)`)
- Solo / Cut / Solo Clear buttons → REAPER track-state routing (currently just consumed)
- 4K E / 4K G / 4K B binding tables (CS 2 knob/button IDs are stable across plugins; just need the VST3 param indices per-variant — existing UF8 PluginMap has the numbers)
- Link-System config page: map any VST3 plugin's params to UC1 BC section + CS section (Phase 3 of the roadmap)

## Environment notes

- Windows capture box: 192.168.177.197 (`claude`/`claudepass`), UC1 captures in `C:\Users\claude\uc1_capture\`, USBPcap interface `\\.\USBPcap3`.
- UC1 device address rotates on replug — `parse_usbpcap_uc1.py` auto-detects.
- UF8 must stay physically disconnected during UC1 captures (capture hygiene).
- Reference handshake capture: `uc1_23_ssl360_startup.pcapng`. Used for the replayed init flood in `extension/src/uc1_init_sequence.inc`.
- Hardware quirk: after a bad init or aborted session, UC1 firmware can hang and require a power-cycle at the unit (USB replug alone isn't always enough). Observed twice during debugging.

## Capture inventory (UC1 work)

- uc1_01..uc1_22: initial decode sweeps (knobs, buttons, LEDs, GR, VU, init)
- uc1_23: SSL 360° cold-start handshake reference
- uc1_24b: BC track name @ zone 0x04 pos 14
- uc1_25: CS track name @ zone 0x02 pos 12
- uc1_26: CHANNEL-encoder 110-position scroll (uncovered the 7-seg FF 13 04 01 mechanism)
- uc1_27: systematic 7-seg decode (positions 1→20 and 99→105 — enough to decode ones + tens alphabetical)

All captures committed to `captures/uc1/` (force-added; `captures/` is otherwise gitignored).
