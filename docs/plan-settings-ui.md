# Plan — Settings UI

Status: draft, 2026-04-24.
Prereqs: Phase 1 shipped (Phase 2 in progress, Phase 2.5 in design).
Goal: a configuration UI that (a) visually mirrors SSL 360°'s mapping
screens so existing users have muscle memory, (b) exposes Rea-Sixty's
additional modes (folders, selection sets, send layer, generic FX map),
and (c) lets any UF8 button other than `SEL` / `CUT` / `SOLO` be freely
bound.

## Framework: Dear ImGui

Reasons over Electron / Tauri / native toolkits:
- Zero runtime dependency at the user end — one extra dylib next to the
  existing extension, nothing to install separately
- Cross-platform by construction (macOS / Windows / Linux), single source
- We already ship a C++ toolchain and link libusb; ImGui is a header +
  source drop, no new techstack
- Launchable via a REAPER named action (`REAPER: rea-sixty — Open
  settings…`) — no separate process, no IPC
- The 360° look is a theme job (dark palette + right font), not a
  rewrite

Rejected alternatives:
- **Electron / Tauri**: ~100 MB bundle, separate process, IPC glue — too
  much for the scope
- **Native SwiftUI + WinUI**: 3× the work, no polish gain that matters
  here
- **WDL / REAPER-native dialogs**: too constrained for a 360°-style
  canvas

## Window layout

Top tab bar:

```
[Device] [Mappings] [Layers] [Modes] [Selection Sets] [About]
```

### Tab: Device

- Connected devices with status dots (UF8 #1, UC1 #1, …)
- LED brightness slider (writes the global-brightness frame decoded in
  the upcoming brightness capture)
- Scribble brightness slider
- Meter ballistic selector (PPM / VU / RMS) — UI only until Phase 1
  meter forwarding is implemented
- `SEL follows track color` toggle — writes the SEL-color frame once
  that capture is done

### Tab: Mappings (primary screen)

Dropdown for layer + bank at the top. Canvas renders a physical-layout
diagram of UF8 with clickable slots:

- 8× top soft-keys (one row above the 8 strips)
- 8× V-Pot push (per strip)
- Left-side buttons: Bank ◀ ▶, Page ◀ ▶, Flip, Layer, the automation
  row (Read / Write / Trim / Touch / Latch), Pan, Fine, Focus, 360,
  Channel-encoder push
- Transport row: Play / Stop / Rec / FF / Rew / Loop

Fixed, non-bindable:
- Fader (always track volume or plugin param depending on layer)
- V-Pot rotation (layer-determined: Pan / Send / FX-param)
- SEL / CUT / SOLO per strip (hardcoded track semantics)

Clicking a slot opens a right-side **Binding Inspector** panel:

```
Label:        [ free text              ]
Color:        [palette swatches]
Brightness:   ────●──── 80%
Action:       ( ) REAPER action
              (•) Named command
              ( ) Keystroke
              ( ) Internal
Command:      [ text or autocomplete   ]  [Browse…]
Behaviour:    (•) Toggle  ( ) Momentary
                                 [Cancel] [Apply]
```

### Tab: Layers

Per-layer defaults and descriptions. `DAW` layer is the reference; other
layers inherit from DAW unless a slot is overridden. Users can add/rename
custom layers within a small per-device limit.

### Tab: Modes (Phase 2.5 features)

```
Folder Mode
  [✓] Enabled
  Long-press duration:  ────●──── 500 ms
  Expand default:   (•) one level  ( ) recursive

Show Only Selected
  [✓] Bank resolver respects saved sets
  Auto-save on selection change: [ ]

Send / Receive Layer
  Enabled as layer: [✓] Send  [ ] Receive (v2)

Generic FX Mapping
  Learn-mode modifier: [Focus ▼] + control touch
  Display format:  (•) Plugin-formatted  ( ) Raw 0..1
```

### Tab: Selection Sets

Eight slots; each shows name + track GUIDs, with an editor for pruning
missing tracks manually and a "save current selection" button. Helpful
for debugging when a track disappears.

## Bindable controls — enumerated

Anything not in the non-bindable list is bindable per `(layer, bank,
slot)`.

| Group | Count | Notes |
|---|---|---|
| Top soft-keys (`0x18..0x1F`) | 8 | per-bank, per-layer |
| V-Pot push | 8 | rotation is layer-determined, push is free |
| Bank / Page | 4 | ◀ ▶ × 2 |
| Automation | 5 | Read / Write / Trim / Touch / Latch |
| Transport | 6 | Play / Stop / Rec / FF / Rew / Loop |
| Mode keys | ~6 | Flip / Layer / 360 / Pan / Fine / Focus |
| Channel-encoder push | 1 | |
| **Σ bindable / bank / layer** | **~38** | |

6 layers × 8 banks × 38 = large config space, but "inherit from DAW" keeps
default configs small.

## Action catalog

Dropdown with autocomplete:

1. **REAPER named actions** — all commands via `kbd_getTextFromCmd`,
   searchable
2. **Keystroke** — arbitrary key combination, forwarded to REAPER / OS
3. **Layer switch** — `Goto Layer: DAW / Plugin / EQ / Instrument /
   Send / Pan`
4. **Selection Set** — `Recall set 1..8`, `Store set 1..8` (Phase 2.5b)
5. **Folder Mode** — `Folders: on/off`, `Expand/collapse focused`
   (Phase 2.5a)
6. **FX navigation** — `Next FX`, `Prev FX`, `Focus FX 1..8`
7. **Transport** — bindable like any other action; Play/Stop/Rec/FF/Rew
   are defaults but not hardcoded
8. **Learn-mode** — `Enter FX learn for touched control` (Phase 2.5d)

## Persistence

- **Global config**
  - macOS: `~/Library/Application Support/Rea-Sixty/config.json`
  - Windows: `%APPDATA%\Rea-Sixty\config.json`
  - Linux: `$XDG_CONFIG_HOME/rea-sixty/config.json`
  - Contents: device settings, layer/button bindings, action catalogs
- **Per project** (REAPER extState, namespace `rea_sixty`)
  - Selection sets (track GUIDs)
  - Folder expand state
  - Generic FX-param bindings (FX guid is project-scoped)
- **Import / export**: JSON, for sharing between users. No SSL 360° XML
  import in MVP (see ROADMAP non-goals).

## Build order

1. **MVP** — Tabs `Device` + `Mappings`. Binding inspector complete;
   persistence wired. ~2–3 sessions.
2. **Phase 2.5 integration** — Tabs `Modes` + `Selection Sets`, landing
   alongside the feature code in Phase 2.5a/b/c/d. Ships incrementally.
3. **Polish** — color picker widget, live preview to the device, undo /
   redo in the mapping editor, JSON import/export. ~1 session.

## Non-goals

- No in-UI firmware update (SSL still handles firmware — see ROADMAP
  Phase 4)
- No MIDI-learn for fader-rotation (those are layer-semantic, not
  free-assign)
- No per-strip `SEL / CUT / SOLO` overrides (those stay hardcoded to
  preserve Mackie-style muscle memory and keep bank navigation sane)
