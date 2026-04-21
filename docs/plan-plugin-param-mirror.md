# Plan — Plugin Parameter Mirror

Status: draft, 2026-04-21.
Prereqs: `docs/plugin-ipc-notes.md` (architecture + path decision).
Goal: drive SSL plugin params on the UF8 scribble strips and V-Pots, using
REAPER's VST3 bridge — no SSL 360° IPC involved.

## What "done" looks like

A user with `SSL Native Channel Strip 2` on 8 tracks, no SSL 360° running:

- UF8 LCDs show `CS 2` in the Channel Strip Type zone (already works).
- Each strip's Parameter Label shows the short legend for the current page
  (`LPF`, `HPF`, `HF`, `LMF`, …). Driven from our mapping table.
- Each strip's Value Line shows the parameter's formatted value
  (`HF Freq    8.00kHz`) — sourced via `TrackFX_FormatParamValueNormalized`.
- Each strip's V-Pot rotation edits the corresponding VST3 parameter.
  Clockwise = increase, CCW = decrease, respecting `LinkInverted` for
  mappings that invert.
- V-Pot push: reset the param to its default (or to a sensible per-map
  "neutral" — e.g. HF Gain → 0 dB).
- Page ← / → buttons cycle through pages of 8 params per strip. UF8 LCDs
  update on page change.
- Non-SSL tracks fall back to the current pan/volume behaviour.

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
  ]
}
```

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

No V-Pot writes yet. Just display.

Code:

- `extension/src/PluginMap.h/.cpp`
  - `struct LinkSlot { int idx; string id; string name; string legend; int vst3Param; bool inverted; optional<double> deflt; }`
  - `struct PluginMap { string match; string displayShort; vector<LinkSlot> slots; vector<vector<int>> pages; }`
  - `class PluginMapRegistry`
    - `loadBuiltins()` — read bundled JSONs (embedded as `.inc` or read
      from `~/Library/Application Support/REAPER/UserPlugins/reaper-uf8/mappings/`).
    - `loadSsl360LinkFactories()` — glob `/Library/Application Support/SSL/SSLPlugins/SSL360Link/*.factory` and parse XML into `PluginMap`.
    - `lookupForTrack(MediaTrack*)` → `(const PluginMap*, int fxIndex)` or
      nullptr. Reuses the existing `sslPluginShortName` matching logic.
- Extend `main.cpp::pushZonesForVisibleSlots`:
  - Per visible slot, resolve `(plugin, fx, currentPage)`.
  - For strip s: lookup `pageSlots[currentPage][s]` → `LinkSlot`.
  - Read `TrackFX_GetParamNormalized(tr, fx, slot.vst3Param)`.
  - Read `TrackFX_FormatParamValueNormalized(tr, fx, slot.vst3Param, val, buf, bufsz)`.
  - Push Parameter Label = `slot.legend` (cache-on-change).
  - Push Value Line = `composeValueLine(slot.name, formattedValue)`.
- Bank-change / page-change invalidates the per-slot caches.
- Fallback path: if `lookupForTrack` returns nullptr, keep the current
  track-name + Vol display.

Tests:

- Unit: `PluginMap` JSON round-trip (parse → serialize → parse).
- Unit: `.factory` XML parser — a couple of small fixture files covering
  the main `<LINK_PARAM>` / `<HOSTED_PARAM>` / `LinkInverted` variants.
- Manual: open REAPER with CS2 on track 1. Confirm the 8 strips show
  correct legends + live-updating values when the plugin GUI is moved.

### Stage 2 — V-Pot writes (~½ session)

Add input path.

- Queue new `PendingInput` kind: `PluginParamDelta { strip, signedDetents }`.
- `onUf8Input` V-Pot rotation: branch on `lookupForTrack(slotTrack[strip])`.
  If SSL plugin present, queue `PluginParamDelta`; else keep `PanDelta`.
- Drain: compute `new = clamp(old + detents * stepScale * (inverted ? -1 : 1), 0..1)`.
  `stepScale` default 1/128, configurable later.
- V-Pot push with SSL plugin present: `TrackFX_SetParamNormalized` to
  `slot.deflt` if set, else `0.5`.

Edge: touch / Fine-mode scaling — multiply `stepScale` by 0.25 while
`g_shiftHeld`.

### Stage 3 — Paging (~½ session)

- Track `int g_pageIdx` atomic, default 0.
- Page ← (0x52) / Page → (0x53) buttons: cycle
  `0 ≤ g_pageIdx < pluginMap.pages.size()` (clamped, no wrap).
- Page change invalidates per-slot caches so Stage-1 dedup re-pushes.

Optional: also mirror `g_pageIdx` to the UF8 Position zone command once
blocker #4 is decoded.

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

## UI model gap — discovered 2026-04-21

What we implemented (Stages 1-3): **per-strip own-plugin model**. Each
UF8 strip shows slots[pageIdx] of its own track's plugin. Page ← / →
advances all strips simultaneously in their respective slot lists.

What SSL 360° actually does in PM mode: **focused-plugin model**. One
plugin is focused (selected track's first SSL plugin). All 8 UF8 strips
show 8 DIFFERENT params of that ONE plugin. The 5 right-side Soft Keys
(IDs 0x69–0x6D) select which 8-param "page" (INPUT / FILTER / EQ LOW /
EQ HIGH / DYN / SEND per cap15_pm_param_cycle.md).

Partial data for native plugins from cap15 (layout for 4K G — IMPEDANCE
only exists there):

- **INPUT:** Bypass / IN TRIM / — / PRE / MIC·DRIVE / — / IMPEDANCE IN / IMPEDANCE
- **FILTER:** WIDTH / — / — / A·B / HIGH PASS / LOW PASS / EQ / EQ TYPE
- **EQ LOW:** LF FREQ / LF GAIN / LF TYPE / — / LMF FREQ / LMF GAIN / LMF Q / —
- **EQ HIGH:** HMF FREQ / HMF GAIN / HMF Q / — / HF FREQ / HF GAIN / HF TYPE / —
- **DYN + SEND:** not captured (TODO — Windows + USBPcap pass with SSL 360° running).

Action items:

1. Capture `cap20_pm_dyn_send.pcap` — cycle through all 5 soft keys in
   SSL 360° PM mode with CS2 on track 1, then 4K E, then BC2. Use the
   `FF 66 <len> 04 <strip> <text>` command stream to read per-strip
   Parameter Label for each page.
2. Extend `PluginMap` with a `Page[] pages` member (each Page holds 8
   linkParamIdx values); drop the flat-slot-list that Stage 1 used.
3. Wire Soft Keys 0x69–0x6D to `g_pageIdx = 0..4` (or whatever range the
   plugin defines).
4. Add a mode toggle (CLAUDE.md settings backlog): "SSL classic PM" vs
   "multi-track-paged" (the Stage-1 behaviour).

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

- On a 4-track REAPER project with CS2 on each track:
  - 30 Hz display updates without audible latency spike or dropped
    timer ticks (check via extension build on Mac Studio).
  - V-Pot rotation on UF8 strip 1 moves CS2's HF Gain knob on track 1.
  - Plugin GUI drag of HF Gain on track 1 updates UF8 strip 1 Value Line
    within one 30 Hz tick (~33 ms).
  - Page ← / → cycles through at least 3 pages of 8 params.
- No regression in the current track-name + Vol fallback for non-SSL
  tracks.
- Unit tests pass.
