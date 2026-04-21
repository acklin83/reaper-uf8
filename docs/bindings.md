# Bindings — Button, V-Pot, Softkey and Layer Configuration

Design doc for the user-configurable mapping system. Paired with
`architecture-decision.md` — that document says *we are a standalone
`csurf_inst`*; this one says *how users tell it what each control
should do*.

## Locked-In Decisions

1. **Runtime: REAPER extension only.** No standalone daemon, no IPC.
   UF8 is only meaningful when REAPER is running.
2. **Config UI: ReaScript + ReaImGui.** Native REAPER look, fast to
   iterate, no SWELL/C++-GUI burden. Ships as a `.lua` + companion
   `.json` schema file under `<REAPER resource>/Scripts/rea_sixty/`.
3. **Learn Mode: yes, v1.** MIDI-Learn-style — user arms a control
   on UF8, triggers a REAPER action, binding recorded. Matches an
   interaction pattern REAPER users already know.
4. **Builtin actions: core first.** Bank nav, layer switch, V-Pot
   mode, flip, track-context. Bonus features (UC1 monitor control,
   SSL-Plug-in-specific bindings) are Phase 2, unlocked by a
   `builtin` registry that's trivial to extend.

## Config File

**Location**
- macOS:  `~/Library/Application Support/REAPER/rea_sixty/bindings.json`
- Windows: `%APPDATA%\REAPER\rea_sixty\bindings.json`
- Linux:  `~/.config/REAPER/rea_sixty/bindings.json`

**Lifecycle**
- If missing at startup: write default template, load.
- Extension watches mtime; reload on change (no REAPER restart).
- Learn Mode and the ReaImGui UI both edit this file.
- Schema version bumps trigger migration on load; old file archived as `bindings.json.v<N>.bak`.

**Top-level structure (JSON, v1)**

```json
{
  "schema_version": 1,
  "active_layer": "DAW",
  "layers": {
    "DAW": { "vpot_mode": "pan", "soft_keys": { ... } },
    "FX":  { "vpot_mode": "selected_fx_param", "soft_keys": { ... } }
  },
  "per_strip": {
    "select": { "type": "track_target", "target": "select" },
    "mute":   { "type": "track_target", "target": "mute" },
    "solo":   { "type": "track_target", "target": "solo" },
    "rec":    { "type": "track_target", "target": "rec_arm" },
    "vpot_press": { "type": "builtin", "action": "vpot_reset" }
  },
  "transport": {
    "play":   { "type": "reaper_action", "id": 40044 },
    "stop":   { "type": "reaper_action", "id": 40667 },
    "record": { "type": "reaper_action", "id": 1013 },
    "rewind": { "type": "reaper_action", "id": 40042 },
    "ffwd":   { "type": "reaper_action", "id": 40043 }
  },
  "global_buttons": {
    "bank_left":  { "type": "builtin", "action": "bank_left" },
    "bank_right": { "type": "builtin", "action": "bank_right" },
    "ch_left":    { "type": "builtin", "action": "bank_ch_left" },
    "ch_right":   { "type": "builtin", "action": "bank_ch_right" },
    "flip":       { "type": "builtin", "action": "flip_fader_vpot" },
    "layer_a":    { "type": "builtin", "action": "layer_cycle" }
  }
}
```

Layers apply to `soft_keys` and `vpot_mode` only. `per_strip` and
`transport` are layer-independent (always the same behaviour).

## Binding Types

### `reaper_action`
Dispatches a REAPER action via `Main_OnCommand(id, 0)`.
```json
{ "type": "reaper_action", "id": 40044 }
{ "type": "reaper_action", "name_id": "_SWS_SAVEVIEW" }
```
Use `id` for built-in actions (stable numeric), `name_id` for
SWS/ReaPack/user-macros (string form via
`NamedCommandLookup()`). Hold/release semantics: press triggers once;
for momentary actions REAPER's own momentary flag applies.

### `builtin`
Built-in hardware or surface behaviour that isn't a REAPER action.
See catalogue below.
```json
{ "type": "builtin", "action": "bank_right" }
```

### `track_target`
Strip-local standard target — only valid inside `per_strip`.
```json
{ "type": "track_target", "target": "mute" }
```
Targets: `select`, `mute`, `solo`, `rec_arm`, `phase`, `monitor`,
`fx_bypass`, `automation_mode`.

### `fx_param`
Direct binding of a V-Pot (or softkey for toggle params) to an FX
parameter on a specified track.
```json
{
  "type": "fx_param",
  "track": "selected",     // "selected" | "track_index:N" | "per_strip"
  "fx": "index:0",         // "index:N" | "name:SSL Channel Strip 2"
  "param": "index:3",      // "index:N" | "name:In Trim"
  "step": 0.01,            // V-Pot tick size (0..1 normalised)
  "display": "percent"     // "percent" | "db" | "raw" | "custom:<fmt>"
}
```
If `track` is `per_strip`, the binding is templated across all eight
strips (slot N → track currently in strip N).

## Builtin Action Catalogue (v1)

| Action | Purpose |
| --- | --- |
| `bank_left` / `bank_right` | Shift visible 8 by 8 tracks |
| `bank_ch_left` / `bank_ch_right` | Shift by 1 track |
| `bank_home` | Jump to track 1 |
| `layer_cycle` | Cycle active layer |
| `layer_set:<name>` | Jump to named layer |
| `vpot_mode_pan` / `vpot_mode_send` / `vpot_mode_fx_param` | Override current layer's V-Pot mode transiently |
| `vpot_reset` | Reset V-Pot target to default (pan=0, fx_param=default) |
| `flip_fader_vpot` | Swap fader and V-Pot targets |
| `mute_clear_all` / `solo_clear_all` | Clear all mutes / solos |
| `select_exclusive_strip:<n>` | Select only the track in strip n |
| `display_mode_name` / `display_mode_time` / `display_mode_dB` | Change upper/lower scribble content globally |

Actions that need a parameter take it after `:` (e.g.
`layer_set:FX`). Catalogue lives in `extension/src/Builtins.cpp`;
adding one is a one-function change plus a name-registry entry.

## Default Layout (ships with extension)

- **Per-strip buttons** — Select / Mute / Solo / RecArm on the obvious
  buttons; V-Pot press = `vpot_reset`.
- **Transport** — standard REAPER Play/Stop/Rec/RW/FF action IDs.
- **Bank nav** — UF8 bank/channel buttons as expected.
- **Softkeys layer `DAW`** — Save (`40026`), Undo (`40029`), Redo
  (`40030`), Metronome (`40364`), Loop (`1068`), Click (`40634`),
  Markers Next/Prev (`40173`/`40172`).
- **Softkeys layer `FX`** — FX Chain for selected (`40271`), FX
  Bypass selected (`8`), FX param-learn placeholders.
- **Layer A button** — `layer_cycle` (DAW ↔ FX).
- V-Pot mode in `DAW` = `pan`; in `FX` = `selected_fx_param` (first
  plug-in, first parameter, until user re-binds).

Users override individually in the UI; defaults remain as fallback.

## Learn Mode

**Workflow**
1. User opens the ReaScript config UI, clicks "Learn".
2. UI asks: "Press the UF8 control to bind."
3. Extension flags the next incoming UF8 event and reports it back
   to the UI (control-id + strip context).
4. UI asks: "Trigger the REAPER action to bind to it."
5. UI hooks `actions_changed` / uses `Main_OnCommand` monitoring to
   capture the next action triggered.
6. UI writes the binding into `bindings.json`, extension reloads.

**Communication path** ReaScript ↔ extension goes via:
- ExtState namespace `rea_sixty` (`SetExtState` / `GetExtState` +
  polling) for simple key-value flags like "learn armed".
- A small ring-buffer of recent UF8 events written by the
  extension into a shared-memory segment (or ExtState with rolling
  keys) for the UI to pick up.

v1 uses ExtState-only for simplicity; shared-memory upgrade only if
latency becomes annoying.

**Escape hatch** Any incoming REAPER action with keycode ESC cancels
Learn Mode.

## Config UI Sketch (ReaImGui)

```
┌─ Rea-Sixty Bindings ────────────────────────────────┐
│ Active layer: [DAW ▾]   [+ Add Layer]  [Learn ⌕]    │
├─────────────────────────────────────────────────────┤
│ Per-strip buttons                                   │
│   Select   [track_target: select        ▾]   [Edit] │
│   Mute     [track_target: mute          ▾]   [Edit] │
│   Solo     [track_target: solo          ▾]   [Edit] │
│   Rec      [track_target: rec_arm       ▾]   [Edit] │
│ V-Pot press: [builtin: vpot_reset       ▾]   [Edit] │
├─────────────────────────────────────────────────────┤
│ Layer DAW softkeys                                  │
│   F1   Save         [_learn_] [clear]               │
│   F2   Undo         [_learn_] [clear]               │
│   ...                                               │
├─────────────────────────────────────────────────────┤
│ Transport                                           │
│   Play   [reaper_action: 40044 (Play)    ] [learn]  │
│   ...                                               │
├─────────────────────────────────────────────────────┤
│  [Reset to defaults]  [Import...]  [Export...]      │
└─────────────────────────────────────────────────────┘
```

Single-window, no tabs; virtual scroll for softkeys if the list grows.

## Phase 2 (explicitly deferred)

- Monitor section control (requires UC1, separate protocol)
- Multiple V-Pot FX-param bindings simultaneously
- Per-track-template button layouts
- Cross-platform key-chord sequences (hold + press)
- Sync with REAPER Control Surface Offset or multi-device sessions

## Open Questions

1. **How does REAPER's action-triggered-recently hook work?** Need
   to confirm `CSurf_OnXyzChange` provides a path for the UI to
   observe "user just ran action X" without racing. Worst case, UI
   polls `GetLastExtraParameter` on a known sentinel.
2. **V-Pot ring LED feedback for fx_param bindings.** For `pan` it's
   obvious (position around centre); for arbitrary FX params we
   need a display-mode choice per binding (bar / fill / dot).
3. **Soft-key LED state for toggle actions.** REAPER can report an
   action's toggle state via `GetToggleCommandState`; confirm that
   works for `reaper_action` bindings so button LEDs reflect reality.
