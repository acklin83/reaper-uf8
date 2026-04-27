# Plan — Plugin Parameter Mirror

Status: draft, 2026-04-21. **UI model corrected 2026-04-27** — see
"UI model — same parameter across the bank" below for the controlling
spec; the Stages were rewritten to match.
Prereqs: `docs/plugin-ipc-notes.md` (architecture + path decision).
Goal: drive SSL plugin params on the UF8 scribble strips and V-Pots, using
REAPER's VST3 bridge — no SSL 360° IPC involved.

## UI model — same parameter across the bank

This is the SSL 360° Plug-in Mixer behaviour, confirmed against the UF8
User Guide Rev 11, p. 172 + 174:

> "all V-Pots are assigned to the **same selected parameter**, like
> looking across a mixing desk with a consistent layout of controls
> horizontally."

> "**Currently Selected Parameter across the V-Pots** — Displays the name
> of the channel strip parameter currently assigned across the V-Pots.
> **Value** — Displays the value of the currently assigned plug-in
> parameter on **that instance** of channel strip."

So the model is:

- A single **focused parameter** at a time — e.g. `HF Gain`.
- All 8 UF8 V-Pots are assigned to that same parameter; strip *N*
  controls `HF Gain` on track *N*'s plugin instance.
- All 8 UF8 strips show the **same** Parameter Label (`HF Gain`).
- Each strip's Value Line shows that strip's track's current value of
  the focused parameter.
- The selected channel on **UC1** mirrors the same focused parameter in
  zone 0x03 (Channel Strip readout) or zone 0x05 (Bus Comp readout),
  showing the selected track's value.

Selection sources for the focused parameter (per p. 170, "Selected
parameter follows UC1/Plug-in Mixer"):

1. **UC1 knob turn** (`FF 24 02 <knob_id> <delta>`) — `knob_id` →
   parameter via the UC1 knob map in `protocol-notes-uc1.md`.
2. **UF8 top soft-key press** (IDs `0x18..0x1F`, one per strip) — the 8
   top soft-keys are a **menu of 8 selectable params** for the current
   page. Pressing soft-key on strip *k* sets the focused parameter to
   `page.params[k]`. The next page is reached via the page selector (5
   right-side keys; exact IDs to be confirmed by `cap20_pm_dyn_send`).
3. **REAPER plugin GUI move** — periodic poll detects which param the
   user just touched in the plugin window and sets it as focused. Lower
   priority; can be deferred.

## What "done" looks like

A user with `SSL Native Channel Strip 2` on 8 tracks, no SSL 360° running:

- UF8 LCDs show `CS 2` in the Channel Strip Type zone (already works).
- All 8 strips' Parameter Label shows the same focused-param legend
  (e.g. `HF Gain` on every strip).
- Each strip's Value Line shows that strip's track's value of the
  focused param (`HF Gain    +2.0dB`, `HF Gain    -1.5dB`, …) — sourced
  via `TrackFX_FormatParamValueNormalized` per track.
- Each strip's V-Pot rotation edits the focused parameter on **that
  strip's track**. Clockwise = increase, CCW = decrease, respecting
  `LinkInverted` for mappings that invert.
- V-Pot push: reset the focused param on that strip's track to its
  default (or a sensible per-map "neutral" — e.g. `HF Gain → 0 dB`).
- UF8 top soft-keys (`0x18..0x1F`) switch the focused parameter to one
  of the 8 params on the active page.
- UC1 knob turn changes the focused parameter and the selected track's
  value; UF8 picks this up and re-pushes label + per-strip values.
- Non-SSL tracks fall back to the current pan/volume behaviour for
  rotation, and show blank for the focused-param Value Line.

## Out of scope (explicit)

- SSL plugin-specific meters (Gain Reduction etc.) — needs a separate
  capture pass (blocker #3 in the memory file).
- Top-zone soft-key labels (blocker #5).
- Plug-in Mixer Position indicator (blocker #4).
- User-editable mappings at runtime (Phase-2 Config UI).
- Plug-in chains > 1 SSL plugin on one track — pick first detected.

## Mapping data format

Single on-disk format regardless of source. Load-time merge:

```json
{
  "plugin_id": "VST3: SSL Native Channel Strip 2",
  "match": "Channel Strip 2",
  "display_short": "CS 2",
  "link_slots": [
    {
      "idx": 4,  "id": "InputTrim",  "name": "Input Trim",  "legend": "IN",
      "vst3_param": 34,  "inverted": false, "default": 0.5
    },
    {
      "idx": 6,  "id": "LowPassFreq", "name": "Low Pass Filter", "legend": "LPF",
      "vst3_param": 18,  "inverted": false
    }
  ],
  "pages": [
    {"name": "Main",  "slots": [4, 6, 7, 8, 9, 10, 15, 16]},
    {"name": "EQ",    "slots": [8, 9, 10, 11, 12, 13, 22, 20]}
  ],
  "uc1_knob_map": {
    "0x02": 12, "0x03": 11, "0x04": 14, "0x05": 13
  }
}
```

`pages[].slots` is the **menu of 8 params** that the 8 UF8 top soft-keys
(`0x18..0x1F`) select on that page — not 8 simultaneous V-Pot
assignments. Only one slot is the focused param at a time.

`uc1_knob_map` maps a UC1 knob ID (the `FF 24 02 <id>` event) to a
`link_slots[].idx` so that turning a UC1 knob can update the focused
param. Keys come from `docs/protocol-notes-uc1.md` "Dedicated Channel
Strip pots" / "Bus Comp" tables.

Sources:

- **Third-party plugins** → load SSL's XMLs from
  `/Library/Application Support/SSL/SSLPlugins/SSL360Link/*.factory`.
  Convert to the JSON structure at startup (don't commit the XMLs — read
  from the user's install). The `<LINK_PARAM>` + `<HOSTED_PARAM>` mapping
  drops in 1:1. Pages come from SSL 360° Link's default page layout —
  hard-coded in our loader until we decide to derive them from the
  `.factoryprefs` file.
- **Native SSL plugins** → we ship hand-built JSONs under
  `extension/data/sslnative/` (checked in). One per plugin.
  Step 0 below covers how to build these.
- **User overrides** → `~/Library/Application Support/reaper-uf8/mappings/*.json`
  takes precedence over both above. Optional, not shipped.

## Stages

### Stage 0 — Parameter enumeration (one-time, manual)

Not code in the extension — we just need the data to build maps.

1. Write `analysis/dump_fx_params.lua` — a ReaScript that iterates
   `TrackFX_GetCount(tr) * TrackFX_GetNumParams` for the selected track
   and writes `idx | name | cur_normalized | formatted_value` to the
   REAPER console.
2. In REAPER, load one track per native SSL plugin (CS2, 4K B, 4K E,
   4K G, Bus Comp 2 — the five 360°-integrated plugins; the X-series is
   not 360° Link-aware). Run the script on each.
3. Save each dump to `docs/ssl-native-params/<plugin>.md`.
4. Draft `extension/data/sslnative/<plugin>.json` by hand, referencing
   the SSL 360° Plugin Mixer layout for page ordering.

### Stage 1 — Read-only mirror (~1 session)

No writes yet. Just display.

Focused-param state (global):

```cpp
struct FocusedParam {
  const PluginMap* map;   // governing plugin family (CS 2, BC 2, …)
  int slotIdx;            // index into map->slots
};
std::atomic<FocusedParam> g_focusedParam;
```

Default on startup / track change with no prior selection: first slot of
the active page (e.g. `LF Gain` on page 0 of CS 2).

Code:

- `extension/src/PluginMap.h/.cpp`
  - `struct LinkSlot { int idx; string id; string name; string legend; int vst3Param; bool inverted; optional<double> deflt; }`
  - `struct PluginMap { string match; string displayShort; vector<LinkSlot> slots; vector<vector<int>> pages; map<int, int> uc1KnobMap; }`
  - `class PluginMapRegistry`
    - `loadBuiltins()` — read bundled JSONs (embedded as `.inc` or read
      from `~/Library/Application Support/REAPER/UserPlugins/reaper-uf8/mappings/`).
    - `loadSsl360LinkFactories()` — glob `/Library/Application Support/SSL/SSLPlugins/SSL360Link/*.factory` and parse XML into `PluginMap`.
    - `lookupForTrack(MediaTrack*)` → `(const PluginMap*, int fxIndex)` or
      nullptr. Reuses the existing `sslPluginShortName` matching logic.
- Extend `main.cpp::pushZonesForVisibleSlots`:
  - Read `(map, slotIdx) = g_focusedParam`. If null, fall back to the
    current track-name + Vol display.
  - For each visible strip *s* (0..7):
    - `tr = slotTrack[s]`; `(_, fx) = lookupForTrack(tr)`. If nullptr,
      blank the Value Line for that strip.
    - `val = TrackFX_GetParamNormalized(tr, fx, map->slots[slotIdx].vst3Param)`.
    - `formatted = TrackFX_FormatParamValueNormalized(...)`.
    - Push Parameter Label = `map->slots[slotIdx].legend` (same on all
      8 strips — cached, only re-pushed when `slotIdx` changes).
    - Push Value Line per strip = `composeValueLine(slot.name, formatted)`.
  - On UC1: push the selected-track value to zone 0x03 (CS) or 0x05 (BC)
    using the same focused param.
- Caches: per-strip `lastFormatted` for the Value Line dedup; one global
  `lastLabel` for the (broadcast) Parameter Label.
- Triggers that invalidate: `g_focusedParam` change, bank change,
  selected-track change, plugin add/remove on a visible track.

Tests:

- Unit: `PluginMap` JSON round-trip (parse → serialize → parse).
- Unit: `.factory` XML parser — a couple of small fixture files covering
  the main `<LINK_PARAM>` / `<HOSTED_PARAM>` / `LinkInverted` variants.
- Manual: open REAPER with CS2 on tracks 1–4. Force-set focused param =
  `HF Gain`. All 4 strips show the same `HF Gain` label; per-strip
  values update live as plugin GUI is moved on each track.

### Stage 2 — V-Pot writes (~½ session)

Add input path. V-Pot rotation drives the **focused param on that
strip's track** — same behaviour as the SSL 360° UF8 surface.

- Queue new `PendingInput` kind: `PluginParamDelta { strip, signedDetents }`.
- `onUf8Input` V-Pot rotation: if `g_focusedParam` is set and
  `lookupForTrack(slotTrack[strip])` returns the same plugin family,
  queue `PluginParamDelta`; else keep `PanDelta`.
- Drain: read current normalized value, compute
  `new = clamp(old + detents * stepScale * (inverted ? -1 : 1), 0..1)`,
  `TrackFX_SetParamNormalized` on `slotTrack[strip]`. `stepScale`
  default 1/128, configurable later.
- V-Pot push with SSL plugin present: `TrackFX_SetParamNormalized` to
  `slot.deflt` if set, else `0.5`, on that strip's track.

Edge: touch / Fine-mode scaling — multiply `stepScale` by 0.25 while
`g_shiftHeld`.

### Stage 3 — Focused-param dispatcher (~½ session)

Sources that write `g_focusedParam`:

1. **UF8 top soft-key** (IDs `0x18..0x1F`):
   `slotIdx = currentPage->slots[strip - 1]`.
2. **UC1 knob turn** (`FF 24 02 <id>`): if the focused track's plugin
   has `uc1KnobMap[id]`, set `slotIdx = uc1KnobMap[id]`.
3. **UF8 page selector** (5 right-side keys, IDs TBD by
   `cap20_pm_dyn_send`): change the active page; do **not** auto-change
   the focused slot — let the user pick a soft-key on the new page.
   (Optional: snap to the page's first non-empty slot.)
4. **REAPER plugin GUI move** (deferred): periodic poll —
   `TrackFX_GetLastTouchedFX` then map to a `slotIdx` if the touched
   param matches one of `map->slots[].vst3Param`.

Each write is followed by a forced re-render of UF8 label + per-strip
values + UC1 selected-track readout.

Optional: also mirror the page index to the UF8 Position zone command
once blocker #4 is decoded.

## File layout after this plan

```
extension/
  src/
    PluginMap.h          NEW
    PluginMap.cpp        NEW
    main.cpp             edit: hook PluginMap into pushZonesForVisibleSlots,
                               add PluginParamDelta handling + page state
  data/
    sslnative/
      channel_strip_2.json   NEW
      4k_b.json              NEW
      4k_e.json              NEW
      4k_g.json              NEW
      bus_comp_2.json        NEW
  test_plugin_map.cpp    NEW (unit: JSON + .factory parsing)
  vendor/
    tinyxml2/            NEW (or rapidxml — header-only, MIT)
analysis/
  dump_fx_params.lua     NEW
docs/
  ssl-native-params/
    channel_strip_2.md   NEW (param enumeration dump)
    …
```

## Page layouts — to capture

Initial cap15 reading of the 8 top soft-keys per page (4K G; IMPEDANCE
only exists there):

- **INPUT:** Bypass / IN TRIM / — / PRE / MIC·DRIVE / — / IMPEDANCE IN / IMPEDANCE
- **FILTER:** WIDTH / — / — / A·B / HIGH PASS / LOW PASS / EQ / EQ TYPE
- **EQ LOW:** LF FREQ / LF GAIN / LF TYPE / — / LMF FREQ / LMF GAIN / LMF Q / —
- **EQ HIGH:** HMF FREQ / HMF GAIN / HMF Q / — / HF FREQ / HF GAIN / HF TYPE / —
- **DYN + SEND:** not captured.

Each entry above is one of the 8 selectable params on that page — i.e.
the **menu** behind the 8 top soft-keys, not 8 simultaneous V-Pot
assignments.

Action items:

1. Capture `cap20_pm_dyn_send.pcap` — cycle through all 5 page-selector
   keys in SSL 360° PM mode with CS 2, then 4K E, then BC 2. Read the
   `FF 66 <len> 04 <strip> <text>` stream after each page change to
   confirm the 8-param menu per page.
2. Identify the page-selector button IDs (5 right-side keys, suspected
   `0x69..0x6D`) in the same capture.
3. Confirm via capture: when SSL 360° receives a UC1 knob turn for a
   param on a different page than the currently shown one, does it
   auto-switch the page? (Manual p. 170 says the param assignment
   follows; whether the page indicator follows too is unclear.)

## Risks

- **Native SSL plugin param indices shift on version bump.** SSL has done
  this before (Channel Strip 1 → 2). Mitigation: load maps by plugin name
  match, not by hash; ship a version field in the JSON so mismatches warn.
- **REAPER's VST3 parameter indices occasionally differ from Steinberg's
  "unit+param" indexing.** We drive with REAPER's flat index; verified on
  CS2 earlier sessions via the Lua dump. Keep the dump tool around.
- **No `FormatParamValueNormalized` for some older SDKs.** We already
  require REAPER v6+ which has it.

## Success criteria

- On a 4-track REAPER project with CS 2 on each track:
  - 30 Hz display updates without audible latency spike or dropped
    timer ticks (check via extension build on Mac Studio).
  - Pressing the UF8 top soft-key for `HF Gain` makes all 4 strips show
    `HF Gain` as Parameter Label, with each strip's track-specific
    value in its Value Line.
  - V-Pot rotation on UF8 strip 1 moves track 1's CS 2 `HF Gain`;
    rotation on strip 2 moves track 2's `HF Gain`. Each strip stays
    independent on the same param.
  - Plugin GUI drag of `HF Gain` on track 1 updates UF8 strip 1 Value
    Line within one 30 Hz tick (~33 ms); the other 3 strips stay
    unchanged.
  - Turning the UC1 `HF Gain` knob switches the focused param, all 4
    UF8 strips re-render to `HF Gain`, and UC1 zone 0x03 shows the
    selected track's `HF Gain` value.
  - Page-selector cycles through at least 3 pages of 8 selectable
    params.
- No regression in the current track-name + Vol fallback for non-SSL
  tracks.
- Unit tests pass.
