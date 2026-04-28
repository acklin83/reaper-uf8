# Roadmap

Goal: an open-source replacement for **SSL 360°** that drives the **SSL UF8** (and eventually **UC1**) controllers — without the SSL-plugin-on-every-track requirement, with DAW-layer scribble-strip colors, and cross-platform (macOS first, Windows and Linux follow).

The project started 2026-04-19 as a "just the colors" REAPER extension. After decoding the vendor-USB protocol it became clear that:
- The UF8 needs a wakeup/init sequence from the host to render anything, so a simple color-push side-car isn't possible
- SSL 360° claims the vendor-USB interface exclusively — can't coexist
- Therefore the only technically honest solution is to re-implement SSL 360°'s host-side responsibilities

## Phase 1 — UF8 standalone, full replacement

**Goal:** The user can quit SSL 360° for UF8 and lose no functionality. Bonus: DAW-layer scribble-strip colors, which SSL 360° doesn't offer at all.

**Scope clarification (2026-04-21):** We replace CSI entirely. The extension is the full host — no MCU-MIDI bridge, no reliance on any REAPER control-surface plug-in. All UF8 input (buttons, fader, fader-touch, V-Pot) is routed via the REAPER C API; all UF8 output (scribble, colors, LEDs, fader motor, meters) is driven by the extension from REAPER state on a timer. This keeps PM-mode — the only mode with color bars — fully functional without any SSL- or MCU-stack dependency.

Deliverables:
- `reaper_uf8` REAPER extension that claims UF8 over libusb
- Init-sequence replay at open() so the UF8 actually wakes up
- Color push on REAPER track-color change + bank shift
- Native button router: every UF8 button (per-strip SOLO/CUT/SEL/V-Pot-Push, top soft-keys, Bank, Layer, Send/Plugin, Page, Flip, Automation, Transport, 360, Channel-Encoder, Norm/Rec/Auto, PAN, Fine) mapped to a REAPER API call or named extension action
- Native fader + fader-touch + V-Pot rotation routing via REAPER API (no virtual MIDI)
- LED feedback driven from REAPER state (solo/mute/select/arm) on the extension's 30 Hz timer — UF8 LEDs reflect what REAPER thinks
- Keyboard-macro engine: press UF8 soft-key → run a REAPER named action (equivalent of SSL 360°'s "Shift Opt N1" mappings)
- Layer management: DAW / Send / Pan / Plugin / EQ / Instrument selected via left-side Layer buttons
- Meter display: REAPER peak meters drive UF8 meter bands via the native vendor-USB meter command (once decoded)
- Minimal config via JSON/INI: mappings, colors, layer bindings

Next capture/decode work: LED command formats (solo/mute/select/arm), meter command format.

**Milestone complete when:** User starts macOS, SSL 360° is never launched, no MCU control-surface plug-in is configured in REAPER, UF8 drives REAPER fully via the native extension, scribble strips show REAPER track colors.

## Phase 2 — UC1 integration

**Goal:** SSL 360° becomes fully redundant. UC1 behaves as a native Rea-Sixty device driving whatever SSL Bus Compressor (or other supported plugin) sits on the currently-focused REAPER track — including its dedicated GR display.

UC1 was originally parked in a late phase on the assumption that UF8 could ship without it. Revisited 2026-04-22: the project stops being a credible SSL 360° replacement the moment it only covers one of the two controllers a typical user owns. Pulled forward, ahead of the config UI.

### 2a — Protocol capture + decode

- Windows + USBPcap session with **UF8 disconnected** (cap17 showed SSL 360° routes Bus-Comp GR to UC1 when both devices are present; we need UC1 to be the only target)
- Capture sequence `uc1_01` through `uc1_14` covering: init, idle, layer boot, every physical knob, every button, display output, track-selection follow, static GR, dynamic GR under audio, VU meters, external sidechain
- Decode work lands in `docs/protocol-notes-uc1.md` (living spec, mirrors `protocol-notes.md`)
- Capture-workflow recipe in `docs/windows-capture-workflow-uc1.md` (fork of UF8 workflow, first step "unplug UF8")

### 2b — UC1 device + parameter mirror (no GR yet)

- `UC1Device.{cpp,h}` parallel to `UF8Device.*`, PID `0x0023`, same libusb skeleton, same init-replay pattern
- Second `ReaSixtySurface`-style path for UC1, or extend the existing surface to fan out to both devices (TBD once the protocol is in hand)
- Selection-follow model (new, not bank-based): `FocusedTrack` listens to `SetTrackSelected`, resolves the first Bus Comp / Channel Strip on the track, retargets every UC1 knob and button at that FX
- Parameter mirror reuses `PluginMap` — the Bus-Comp-2 slot table already exists, it just needs an additional "physical-knob-id → LinkSlot" indirection per supported plugin instead of the V-Pot-page logic
- Checkpoint: physical UC1 knobs move parameters on the focused track's SSL Bus Comp in real time, with values mirrored back to the UC1's display

### 2c — GR pipeline

- `extension/jsfx/rea_sixty_gr_probe.jsfx` — sidechain-tap envelope follower that exposes a `GR (dB)` slider the extension reads via `TrackFX_GetParam`. Ships with the extension, auto-inserted next to detected SSL compressors
- Auto-insert policy: on `SetTrackFX*` callbacks, if an SSL Bus Comp lands on a track and the probe isn't already present, the extension inserts it post-comp with sidechain routing from the pre-comp tap
- Tick the probe's value into the UC1 GR bar-graph frame at ~30 Hz (same timer path as parameter mirror)
- A/B validation: record the UC1 GR readout driven by (A) SSL 360° + stock plugin vs (B) Rea-Sixty + JSFX probe under identical audio. Ballistic mismatch tolerance ≤2 dB on transients is acceptable; anything larger triggers a followup decision on whether to escalate to a Thrift-MITM path (documented but not preferred — see `docs/plugin-ipc-notes.md`)

**Milestone complete when:** SSL 360° is never launched, both UF8 and UC1 drive REAPER fully via Rea-Sixty, SSL plugins respond to UC1 controls, and UC1's GR display tracks audio-driven gain reduction on the focused track.

## Phase 2.5 — Session navigation & generic FX mapping

**Goal:** Features that SSL 360° does not offer at all — large-session ergonomics, and user-programmable control over *any* VST/JS/AU plugin, not just SSL's.

No new captures required; everything here is REAPER-API work on top of the finished Phase 1/2 foundation.

### 2.5a — Folder Mode

- Bank resolver filters to folder parents only; child tracks hidden by default
- `SEL` long-press (~500 ms, detected on press-edge + release-edge we already parse) toggles expanded state for that parent
- "Has children" indicator in the Channel Number zone (e.g. `+`/`−` sigil) and/or a SEL-LED blink pattern
- Default one level per expand; `Shift`-modifier (or equivalent) expands all nested levels
- Bank-position stable across expand/collapse — user never gets surprise-scrolled

### 2.5b — Show Only Selected + 8 selection slots

- Bank resolver switches from "`GetTrack(i)` 0..N" to a project-local selection list
- 8 slots persisted via `SetProjExtState("rea_sixty", "selset_N", …)`; store Track-**GUIDs** not indices so reorders/deletes don't corrupt sets
- UX: a modifier key toggles Selset-Mode; top soft-keys `0x18..0x1F` over each strip = slot 1..8. Tap = recall, hold (>1 s) = store
- Recall prunes missing tracks gracefully and surfaces count in the Value Line zone
- Inherits GUID-store infrastructure from 2.5a

### 2.5c — Show Sends / Show Receives

- Focus-variant (SSL 360° Send-Layer equivalent): on the focused track, strips 0..7 represent the first 8 sends; fader = send level, CUT = send mute, scribble = destination name, Channel Number Zone = send index, Page L/R pages through >8
- Receives via the same UI, toggled with a dedicated layer button
- Data: `GetTrackNumSends(tr, 0)` / `GetTrackSendInfo_Value` / `GetTrackSendName`
- Deferred optional variant: a routing-map overlay that highlights send destinations across banks with SEL-LED blink. Decide after (a) ships.

### 2.5d — Generic FX-parameter mapping (user-programmable Plug-in Mixer)

- Learn-mode: modifier + move any UF8 V-Pot or touch any soft-button → binding captured from `GetTouchedOrFocusedFX()` + `GetLastTouchedFX()`
- Binding schema in project extState: `slot.<strip>.<page>.<control> → {fxGuid, paramIdx, displayFormat}`. Guid-keyed so FX-slot reorders don't break mappings
- Value display via `TrackFX_GetFormattedParamValue` — plugin-provided formatting ("−3.5 dB", "42 %"), no per-plugin formatter code needed
- Multiple pages per strip; soft-buttons bindable as toggle or momentary
- Relationship to UC1: UC1 keeps its hardcoded SSL-plugin tables (physical layout is built for Bus Comp / Channel Strip). Generic mapping lives on UF8, where the physical controls are already generic

**Milestone complete when:** A user can (a) navigate a 200-track folder session on UF8 without leaving the surface, (b) save/recall 8 selection sets per project, (c) mix sends from UF8, and (d) bind any third-party plugin parameter to any V-Pot or soft-button with learn-mode and see the plugin's formatted value on the scribble strip.

## Phase 2.6 — Plugin Mixer Window

**Goal:** On-screen mixer view that mirrors SSL 360°'s Plugin Mixer — all SSL Channel Strip and Bus Compressor instances visible in a single docked window, fully interactive, themed to the user's REAPER theme. Closes the last gap where users still glance at SSL 360° instead of Rea-Sixty.

Plan: see `~/.claude/plans/splendid-snuggling-hejlsberg.md` (verdict: native REAPER extension with vendored Dear ImGui inside a dockable SWELL window — same in-process model as the rest of the extension; rejected standalone-app variant).

Reuses the existing `PluginMap` slot tables (CS2, 4K B/E/G, Bus Comp 2), `lookupPluginOnTrack`, and the Surface `Run()` tick. New code is purely UI + theme bridge.

### 2.6a — Skeleton + theme bridge

- Vendor `imgui` (docking branch) under `extension/vendor/imgui/`
- Pull `icontheme.h` from upstream REAPER SDK into `extension/vendor/reaper-sdk/sdk/`
- `MixerWindow.{cpp,h}` — SWELL window host, registers the action `Rea-Sixty: Toggle Plugin Mixer Window`, opens an OpenGL-backed ImGui context, docks via `DockWindowAddEx`
- `ThemeBridge.{cpp,h}` — pulls `GetColorThemeStruct` + falls back to indexed `GetColorTheme`; tick-driven hash check to repush on theme change
- Window opens, dock-state persists across REAPER restarts, theme switch in REAPER live-updates the empty window

### 2.6b — Channel Strip column

- `MixerLayout.{cpp,h}` — UC1-mirrored layout: Input → HPF/LPF → EQ HF/HMF/LMF/LF → Comp → Gate → Output → Fader+Audio-Meter
- Knob + button widgets backed by `TrackFX_*Normalized`
- GR meter via `TrackFX_GetNamedConfigParm(tr, fx, "GainReduction_dB", …)` — verifies SSL plugins expose the PreSonus VST3 GR extension to REAPER (user confirmed; in-code verification at this phase)
- Audio-meter via `Track_GetPeakInfo` + `Track_GetPeakHoldDB` (already imported)
- One track visible end-to-end before scaling out

### 2.6c — Multi-column + Bus Compressor rack

- 75% left: horizontally scrollable channel-strip columns, one per track that hosts a CS-family plugin
- 25% right: vertical Bus Compressor rack — one strip per BC instance found via `lookupPluginOnTrack(tr, Domain::BusComp)`
- Viewport-culling so a 200-track session reads only visible columns

### 2.6d — Polish

- Track-color headers (`GetTrackColor`), tooltips with formatted values, keyboard value entry
- Trademark audit: no hardcoded SSL palette / no SSL/360° strings in user-facing text
- Stress test at 100+ tracks; tune per-tick budget if `Run()` slows REAPER

**Milestone complete when:** A user with a 4K-G + CS2 + Bus Comp 2 mix can leave SSL 360° closed, drive every plugin parameter from the Rea-Sixty mixer window, see correct GR + audio meters, and the window auto-themes to whatever REAPER theme they have active (Reapertips by default in our setup, but no theme is bundled).

## Phase 2.7 — Settings Screen

**Goal:** All Rea-Sixty configuration editable without touching code, surfaced from the same docked window as Phase 2.6's Plugin Mixer. A top-level TabBar inside that window switches between **Mixer** and **Settings**; no separate window, no extra action.

Supersedes the earlier Phase 3 design (Electron / SwiftUI standalone). Now that we already host ReaImGui from C++ for the mixer, the settings UI is just additional tabs in the same context — zero extra runtime cost.

Source notes (to be consolidated into a single canonical spec during 2.7a):
- `docs/plan-settings-ui.md` — original tab structure and layout
- `docs/bindings.md` — concrete JSON binding format, Learn-mode flow, builtin action catalogue
- Memory `uf8-softkey-banks.md` — UF8 PM-mode CS6 / BC2 bank tables, kNoSlot positions

Settings tabs:

| Tab | Source of truth | Purpose |
|---|---|---|
| Device | plan-settings-ui §"Device" | LED brightness, scribble brightness, meter ballistic, SEL-follows-color |
| Bindings | bindings.md | Per-strip, transport, global buttons, soft-keys per layer, Learn mode |
| Soft-Key Banks | uf8-softkey-banks.md | CS6 / BC2 bank tables with kNoSlot positions wirable to raw VST3 / REAPER actions |
| Modes | ROADMAP 2.5 | Folder Mode, Show-Only-Selected, Send/Receive, Generic FX |
| Selection Sets | ROADMAP 2.5b | 8 GUID-keyed track-selection slots per project |
| About | — | Version, build hash, repo / ReaPack links |

### 2.7a — Spec consolidation + Device + About

- Merge plan-settings-ui.md and bindings.md into a single `docs/settings-spec.md`. Resolve disagreements (multi-tab vs flat scroll → multi-tab wins, that's the implementation).
- Implement Device tab against existing UF8/UC1 globals.
- Implement About tab — cheap.

### 2.7b — Bindings tab + JSON persistence

- Per-strip / transport / global-button / soft-key editor matching bindings.md §"Config UI Sketch".
- JSON read/write to `~/.../REAPER/rea_sixty/bindings.json` with mtime watch + reload.
- Learn-mode arming via ExtState handshake.

### 2.7c — Soft-Key Banks tab

- CS 6-bank + BC 2-bank grids reading authoritative tables from `softkey::` namespace.
- kNoSlot wiring picker: raw VST3 param browser (uses `TrackFX_GetParamName`) + REAPER action picker (uses `kbd_getTextFromCmd`).
- Per-position label + colour override.

### 2.7d — Modes + Selection Sets tabs

- Lands alongside the matching Phase 2.5 feature code, not before — these tabs configure features that don't exist yet.

### 2.7e — Polish

- Color-picker widget for button colours.
- Import / export JSON for sharing configs between users.
- Live preview to UF8 and UC1 — touched control highlights on the hardware.
- Reset-to-defaults action.

**Milestone complete when:** A non-developer user can remap any UF8/UC1 control via the Settings tab, save, reload across REAPER restarts, and see the change reflected on the hardware without restarting REAPER.

## Phase 4+ — Community

**Goal:** Project graduates from "Frank's studio tool" to "the open-source alternative for SSL controllers".

Candidate work:
- Windows port (libusb there, but USB backend differs in edge cases)
- Linux port (hidraw + libusb)
- DAW support beyond REAPER: anything MCU-compatible (Cubase, Studio One, Pro Tools via HUI, Logic, Bitwig)
- Firmware update path (since SSL will still ship firmware blobs; we may need to let SSL 360° do firmware updates, our tool handles everything else)
- Support for more SSL controllers (Big SiX, BiG SiX, SiX, other Nucleus variants)

## Non-goals (at least for now)

- Replacing the *individual plugin GUIs* — the plugins keep their own per-instance UIs (we don't redraw the EQ curve, the Bus Comp face plate, etc.). Phase 2.6 adds a *mixer-style overview* across instances; opening the plugin's own window is still the way to tweak details
- Full binary compatibility with SSL 360° config files — we ship our own simpler JSON format with a one-time import from SSL 360° XML
- Reverse-engineering any firmware — we only talk to the controllers through their existing USB protocols

## Working rhythm

- Each phase starts with a capture/decode session (Windows box, USBPcap)
- Decoded protocol lands in the matching spec (`docs/protocol-notes.md` for UF8, `docs/protocol-notes-uc1.md` for UC1; never let a capture go un-documented)
- Code in small layers with unit tests where the logic is pure (checksum, palette, frame parse)
- Extension/daemon rebuilds verified on Mac Studio and MacBook before anything lands on main
