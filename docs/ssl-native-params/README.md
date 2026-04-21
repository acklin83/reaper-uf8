# SSL native plugin parameter dumps

Each file in this directory is the output of `analysis/dump_fx_params.lua`
against one track hosting exactly one SSL plugin. They are the source
material for `extension/data/sslnative/<plugin>.json` — the UF8 V-Pot ↔
VST3 parameter mapping files.

Procedure (one plugin at a time):

1. In REAPER, create a fresh empty project.
2. Insert one track. Add exactly one SSL plugin on it (e.g. "SSL Native
   Channel Strip 2"). No other FX — we want a clean flat parameter
   list.
3. Select that track.
4. Actions → "Load ReaScript…" → pick
   `analysis/dump_fx_params.lua` → Run.
5. Check this folder — the dump appears as `<fx_name>.md`.
6. Commit the dump.

Repeat for each SSL 360°-integrated plugin:

- SSL Native Channel Strip 2
- SSL 4K B
- SSL 4K E
- SSL 4K G
- SSL Native Bus Compressor 2

(The X-series and other Native SSL plugins are not 360°-integrated,
so they are not UF8 Plug-in-Mixer targets and we don't need to dump them.)

Once the dumps are in, the next step is hand-building the corresponding
`extension/data/sslnative/<plugin>.json` from each dump + the SSL 360°
Plugin Mixer layout reference.
