# Architecture Decision: Standalone SSL 360° Replacement (no CSI)

**Decided:** 2026-04-20 (mid-afternoon, verbally). Documented 2026-04-21.

## Decision
The extension replaces SSL 360° for the UF8 end-to-end. It does **not** rely on
Control Surface Integrator (CSI), virtual MCU MIDI, or any other
intermediary. REAPER track state is read directly via the REAPER API and
pushed to the UF8 over vendor-USB; UF8 input events are translated and
posted back into REAPER via the extension API.

Target topology:
```
REAPER  <->  reaper-uf8 extension  <->  UF8 (vendor-USB)
```

Not this:
```
REAPER  <->  CSI  <->  virtual MCU MIDI  <->  reaper-uf8  <->  UF8
```

## Why
1. **MCU cannot carry colors.** The whole point of the project is track
   colors on the scribble strips. MCU/HUI have no color channel, so any
   CSI-based pipeline forces a direct REAPER→UF8 side-path for colors
   anyway — the bridge adds work without paying for the feature.
2. **PM-mode zones need richer data than MCU carries.** Channel Strip
   Type, Value Line, O/PdB readout, SSL plug-in detection — all require
   REAPER API access (FX list, track volume in dB, track name). CSI
   cannot forward any of that.
3. **Config burden.** CSI-based routing requires users to install CSI,
   configure virtual MIDI ports, open the CSI surface dialog to force a
   state flush (see commit b115c5f), and keep the surface config
   synchronised. A standalone extension is one install.
4. **Two-pipeline drift.** Mixing CSI-sourced text with direct-REAPER
   PM-zones (the state at end of 2026-04-20) means two code paths, two
   caches, two sync rules. One pipeline is simpler and more correct.

## Status
- **Direct REAPER→UF8 already implemented** for PM-zone content in
  `main.cpp onTimer()`: Parameter Label (b1b339c), CS Type / Value Line /
  O/PdB (ef5ea66), Color Bar (21806c4), Palette (61b80f5).
- **MidiBridge (ac05a01) is now a transitional layer** — kept working so
  regressions are visible during the migration, but slated for removal.
- **UF8→REAPER input** currently emits MCU back into the virtual port
  (a6cc2d1). Migration target: call REAPER extension API directly
  (`CSurf_OnVolumeChange`, `CSurf_OnFaderTouch`, action dispatch, etc.).

## Migration Path
In rough order, smallest-risk first:

1. Expand direct-REAPER pushes to cover upper/lower scribble strip
   text (currently CSI/MCU-sourced). Source of truth becomes
   `GetTrackName` + bank state held in the extension.
2. Replace MCU-outbound button/fader events with direct REAPER API
   calls. MidiBridge's `onMidi` handler gets shorter on each pass.
3. Delete MidiBridge entirely. Remove the `reaper_uf8 in/out` virtual
   endpoints. Remove CSI setup instructions.
4. Implement bank state (which 8 REAPER tracks are visible) inside the
   extension. Bank-switch buttons on UF8 drive it directly.
5. Implement meter pushes from REAPER's `Track_GetPeakInfo` (once the
   `FF 38 04` encoding is decoded per 1a9a688).

Each step is independently shippable — the extension stays usable
throughout.

## Non-Goals
- We are not trying to be a general-purpose control surface framework.
  UF8-specific; REAPER-specific; no abstraction layer.
- We are not preserving CSI compatibility as a fallback. Once direct
  calls cover a feature, the MCU path is deleted.
