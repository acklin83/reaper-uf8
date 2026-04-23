# Rea-Sixty UC1 — Resume Point

**Last commit:** TBD on `main`, pushed.
**Phase status:** UC1 core + track-state integration working. Both UC1 and UF8 follow REAPER selection; LED feedback working for the central-section track buttons.

## What works end-to-end (tested on macOS with real UC1)

- **Device lifecycle** (unchanged from previous resume): handshake + init flood + 50 Hz GR heartbeat + 150 ms keepalive. UF8 optional in the same extension instance.
- **Bus Comp 2 V-Pots** + **CS knobs** drive VST3 params with display readout.
- **Channel Strip plugin-param buttons** (HF Bell, EQ Type, Dyn In, …) toggle plugin params + LEDs. bank=0x02 + state=0xFF per cap21 — works for this section.
- **Track-state buttons** (Solo / Cut / Solo Clear / Polarity / Channel IN) → REAPER track state, with LEDs.
  - Solo (0x1C) → `CSurf_OnSoloChange`
  - Cut (0x1D) → `CSurf_OnMuteChange`
  - Solo Clear (0x1B) → `Main_OnCommand(40340)` ("Unsolo all")
  - Polarity (0x19) → toggles `B_PHASE` on focused track (note: button doesn't emit USB events on some firmware — LED still reflects state via `refresh()`)
  - Channel IN (0x1E) → toggles plugin-internal IN param (scanned by name) on SSL Channel Strip; falls back to `TrackFX_SetEnabled` when no match; on non-SSL tracks bypasses FX index 0 (placeholder — future settings let user define channel-strip registry)
  - LED encoding override: all five buttons use `bank=0x01, state=0x01` for on (not the cap21 bank=0x02 which turned out to be a status register). See `~/.claude/projects/.../memory/uc1-led-banks.md`.
  - Solo Clear LED lights whenever any track in project is soloed (global indicator).
- **CHANNEL encoder** (0x0D) and **BC encoder** (0x15) both:
  - Scroll REAPER's track selection as before.
  - **New:** trigger `reasixty_followSelectedInMixer` → REAPER MCP scrolls so the new track is visible, AND UF8 rebanks around the selection using the `BucketSnap` follow-mode.
- **Track name**, **7-segment display**, **CHANNEL encoder** all unchanged from prior resume.

## UF8 changes this session

- **Empty strips get fully blanked.** Previously when the bank window extended past the last track (e.g. 12 tracks, bank=8 → strips 5–8 empty), the display still showed the previous bucket's residue in CS Type / Parameter Label / track name / dB readout / Value Line. Now `pushZonesForVisibleSlots` in main.cpp explicitly clears all six zones (CS Type "    ", Parameter Label "", Channel Number "", Track Name "", Fader dB "    ", Value Line "") for empty slots, with `bankChanged` forcing the first-tick push so the display actually reflects the blank even when the dedup cache was just cleared.
- **Color bar for empty strips + default-color tracks = OFF.** `ColorSync::refresh` now emits palette index `0x00` (the hardware OFF state) for any slot whose RGB input is 0. Works correctly; 0x0C was a false lead (the Palette.cpp comment labelled 0x0C "OFF" but it actually renders light blue on this hardware).
- **UF8 follows REAPER selection.** `ReaSixtySurface::SetSurfaceSelected` now calls `followSelectedInMixer(tr)` on any sel=true edge — clicks in TCP/MCP, ReaScript, other surfaces all rebank the UF8 to the BucketSnap containing the selected track.
- **Fine button (UC1)** now toggles (latches); previously required holding.

## Shared infrastructure

- `reasixty_followSelectedInMixer(MediaTrack*)` is the external symbol in main.cpp that exposes the anonymous-namespace `followSelectedInMixer` helper so UC1Surface (different TU) can trigger the same MCP-scroll + UF8-rebank behaviour.

## Outstanding / nice-to-have

- **Channel IN param lookup on CS 2** — the "Channel In" VST3 param name our scan looks for doesn't match SSL Native Channel Strip 2's actual naming. `UC1Surface::channelInParam_` dumps every param name on first access; need to read the console output, identify the real name, and add it to `kCandidates`. Current fallback bypasses the whole plugin, which works but isn't what the user wants.
- **Cleanup**: one-shot diag lines in `handleButton_` (Solo/Cut/SoloClear press logs) and `pushButtonLed_` (LED hex dump) should be removed once the Channel IN name lookup is settled. Also `kDumpedParams` flag in `channelInParam_`.
- **Per-settings toggles** (documented in `reaper-uf8-project.md` memory backlog):
  - "CHANNEL encoder selects on scroll" on/off
  - "Bank-follow mode on selection" BucketSnap vs LeftmostStrip
  - "Non-SSL channel-strip mapping" user-defined plugin registry
- GR-JSFX-Probe → `pushGainReduction()` wiring
- VU meter feed (track peaks → `pushVu(meter, level)`)
- V-Pot LED rings (protocol still undecoded)
- 4K E / 4K G / 4K B binding tables
- Link-System config page (Phase 3)

## Environment notes

Unchanged — see git history for the 2026-04-22 notes.
