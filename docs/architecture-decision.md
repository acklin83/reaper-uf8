# Architecture Decision: Standalone SSL 360° Replacement (no CSI)

**Decided:** 2026-04-20 (mid-afternoon). Documented 2026-04-21.

## Decision
The extension replaces SSL 360° for the UF8 end-to-end. It does **not** rely on
Control Surface Integrator (CSI), virtual MCU MIDI, or any other
intermediary. The extension registers with REAPER directly as a
`csurf_inst` (`IReaperControlSurface`), reads track state via the REAPER
API, and pushes vendor-USB frames to the UF8. UF8 input events are
translated into `CSurf_On*` calls straight back into REAPER.

Target topology:
```
REAPER  <->  reaper-uf8 extension (csurf_inst)  <->  UF8 (vendor-USB)
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
3. **Full 16-bit fader resolution.** MCU pitch-bend is 14-bit; going
   direct via `CSurf_OnVolumeChange` uses REAPER's native 16-bit
   precision.
4. **Single source of truth.** Current state already has two pipelines
   writing to the scribble strips in parallel (CSI MCU-SysEx + direct
   `pushZonesForVisibleSlots`). The conflict resolves itself once CSI
   is gone.
5. **Config burden.** CSI-based routing requires users to install CSI,
   configure virtual MIDI ports, maintain CSI zone files, and open the
   CSI surface dialog to force a state flush (see commit b115c5f). A
   standalone extension is one `.dylib`/`.dll` drop.

## Current State (2026-04-21)
- **Direct REAPER→UF8 already implemented** for PM-zone content in
  `main.cpp onTimer()`: Parameter Label (b1b339c), CS Type / Value Line /
  O/PdB (ef5ea66), Color Bar (21806c4), Palette (61b80f5).
- **MidiBridge (ac05a01) is a transitional layer** — still active so
  regressions stay visible during migration, slated for removal.
- **UF8→REAPER input** currently synthesises MCU into the virtual port
  (a6cc2d1). Migration target: call REAPER `CSurf_On*` directly.
- `onUf8Input` already parses vendor-USB events independently of CSI —
  CSI has become just a lossy MCU transcoder.

## Migration Plan (incremental, each step testable)

1. **Branch off**: `claude/drop-csi-native-surface` from current main.
2. **Add `ReaperSurface` stub** — `extension/src/ReaperSurface.{h,cpp}`
   implementing `IReaperControlSurface`, registered via
   `rec->Register("csurf_inst", ...)`. Log all callbacks to
   `/tmp/reaper_uf8_surface.log`. Verify `Run()` fires and
   `SetSurfaceVolume` reaches us. MidiBridge still runs in parallel —
   zero behaviour change.
3. **Bank state** — small `BankState` class (8 slots → 8 `MediaTrack*`),
   updated on `SetTrackListChange`. v1 hardcoded slots 0..7;
   bank-left/right wires to UF8 bank buttons later.
4. **Outputs REAPER→UF8 via Surface callbacks**:
   - `SetSurfaceVolume` → `buildFaderPosition` (16-bit now)
   - `SetTrackTitle` → `buildStripTextUpper`
   - `SetSurfaceMute/Solo/RecArm/Selected` → LED frames
   - `SetPlayState/SetRepeatState` → transport LEDs
   - Move `ColorSync::refresh` + `pushZonesForVisibleSlots` from
     custom timer into `Run()`
   - Comment out (don't delete) `onMidiFromReaper`
   - Test: track rename in REAPER appears on UF8 with MIDI silent.
5. **Inputs UF8→REAPER via CSurf_* calls** (replacing MCU synth in
   `main.cpp:77`):
   - Fader → `CSurf_OnVolumeChange(tr, vol, false)` with full 16-bit
   - Fader touch → internal state answering `GetTouchState()` callback
   - Button → `CSurf_OnMuteChange` / `CSurf_OnSoloChange` / etc. per ID
   - V-Pot → `CSurf_OnPanChange(tr, delta, true)` (Pan-only for v1)
   - Pass `g_surface.get()` as `ignoresurf` everywhere to prevent
     feedback loops.
6. **Delete MidiBridge** — remove `MidiBridge.{cpp,h}`, Core-MIDI
   framework dep, `onMidiFromReaper`, `logMidi`, `g_midi`. Should be a
   satisfyingly negative diff.
7. **Drop custom timer** — remove `plugin_register("timer", onTimer)`;
   `Run()` is REAPER-driven.
8. **Docs update** — `README.md` narrative from "alongside CSI" →
   "drop-in SSL 360° replacement, no other surface plugin needed".
   `ROADMAP.md` milestone criterion. This document collapses once done.

## Open Questions to Resolve Before Step 5
1. **Button-ID → REAPER-action mapping.** UF8 button byte-IDs known
   from captures, but which semantic (Mute/Solo/RecArm/Select) maps to
   which strip-local button? Either derive from UF8 manual or
   catalogue interactively via press+log.
2. **V-Pot mode.** Pan vs. FX-param vs. Send-level — v1 Pan-only, rest
   deferred to a Phase-2 config UI.
3. **`Run()` frequency.** REAPER docs say "frequently" but not exact.
   Timestamp-log in step 2 to confirm — we don't want to accidentally
   poll colors at 200 Hz.

## Known Constraints Orthogonal to This Change
- Meter forwarding stays deferred (`FF 38 04` format not yet decoded —
  commit 1a9a688)
- SSL 360° exclusive USB claim still prevents coexistence (unchanged)
- Colors: REAPER has no color-change broadcast callback; `ColorSync`
  continues polling `GetTrackColor()` in `Run()`

## Rough Effort
- Steps 1-3: ~1 session (stub + bank)
- Step 4: ~1 session (outputs + LED frame builder)
- Step 5: ~1 session (inputs + button-ID catalogue)
- Steps 6-8: ~0.5 session (teardown + docs)

## Non-Goals
- Not a general-purpose control surface framework. UF8-specific;
  REAPER-specific; no abstraction layer.
- No CSI compatibility fallback. Once direct calls cover a feature,
  the MCU path is deleted.
