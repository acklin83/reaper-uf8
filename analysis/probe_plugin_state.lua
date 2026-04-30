-- probe_plugin_state.lua
--
-- ReaScript probe for plugin-internal toggles that aren't exposed as
-- VST3 parameters (HQ Mode, A/B compare on SSL Native CS2 / 4K B/E/G).
--
-- Strategy: dump everything REAPER can see about the plugin into a
-- timestamped file, so we can run the probe twice (state X, state Y),
-- diff the files, and identify the bit that changed.
--
-- Captures per FX:
--   * VST3 ident (TrackFX_GetFXName, TrackFX_GetFXIdent)
--   * Per-param name + ident + normalized value (looking for hidden
--     non-default-valued params)
--   * NamedConfigParm values for a list of speculative keys covering
--     PreSonus-VST3 conventions, VST3 introspection, and REAPER's own
--     vst3: namespace
--   * Full track-state chunk (base64-encoded plug-in state lives here —
--     diff post-probes to detect state changes)
--
-- Usage:
--   1. Select a track with a single SSL Native Channel Strip 2 (or 4K B/E/G).
--   2. Set HQ Mode OFF on the plugin GUI (default state).
--   3. Run this script → writes /tmp/reasixty_probe_NN.txt (auto-inc).
--   4. Toggle HQ Mode ON in plugin GUI.
--   5. Run again → next file.
--   6. Repeat with A/B (off/on).
--   7. Send the resulting files to Claude Code for diff.

local tr = reaper.GetSelectedTrack(0, 0)
if not tr then
  reaper.ShowMessageBox("No track selected — pick a track with the SSL plugin first.",
                        "probe_plugin_state", 0)
  return
end

local fxCount = reaper.TrackFX_GetCount(tr)
if fxCount == 0 then
  reaper.ShowMessageBox("Selected track has no FX.", "probe_plugin_state", 0)
  return
end

-- Auto-increment output file under /tmp.
local function nextPath()
  for n = 1, 99 do
    local p = string.format("/tmp/reasixty_probe_%02d.txt", n)
    local f = io.open(p, "r")
    if not f then return p end
    f:close()
  end
  return string.format("/tmp/reasixty_probe_%d.txt", os.time())
end

local path = nextPath()
local f = io.open(path, "w")
if not f then
  reaper.ShowConsoleMsg("Could not open " .. path .. " for writing\n")
  return
end

f:write(string.format("# probe @ %s\n\n", os.date("%Y-%m-%d %H:%M:%S")))

-- Speculative NamedConfigParm keys. Hits will be reported, misses silent.
-- Covers: VST3 introspection (vst3:), PreSonus convention (process/, hq*),
-- generic plugin meta (compare, ab, program), SSL-style guesses.
local probeKeys = {
  -- VST3 introspection (REAPER's own keys)
  "vst3:UnitID:0", "vst3:UnitID:1", "vst3:UnitID:2",
  "vst3:ProgramListID", "vst3:ProgramListID:0",
  "vst3:NumPrograms", "vst3:CurrentProgram",
  -- Quality / oversampling
  "process/highqualitymode", "process/highquality", "process/oversampling",
  "highqualitymode", "highquality", "hqmode", "HQ", "hq", "HighQuality",
  "Oversampling", "oversampling", "quality", "Quality",
  -- A/B compare
  "compare", "Compare", "ab", "AB", "a_b", "A_B",
  "programA", "programB", "compareSlot", "abSlot",
  "abCompare", "ABCompare",
  -- SSL-internal guesses
  "ssl:hq", "ssl:ab", "ssl:quality",
  -- PreSonus extension namespace
  "presonus:hq", "presonus:quality",
  -- Linkable / plugin-wrapper
  "Linkable", "ProcessHQ",
}

for fx = 0, fxCount - 1 do
  local _, fxName = reaper.TrackFX_GetFXName(tr, fx, "")
  local fxIdent  = ""
  -- TrackFX_GetFXIdent isn't always available; guard with pcall.
  local ok, ident = pcall(function()
    local _, s = reaper.TrackFX_GetNamedConfigParm(tr, fx, "fx_ident")
    return s
  end)
  if ok then fxIdent = ident or "" end

  f:write(string.format("## FX[%d] %s\n", fx, fxName))
  if #fxIdent > 0 then
    f:write(string.format("ident: %s\n", fxIdent))
  end

  -- Per-param dump: include name + ident-string + raw value. Hidden
  -- params (not in TrackFX_GetParamName but still readable) would show
  -- here if their idx falls in [0, GetNumParams).
  local n = reaper.TrackFX_GetNumParams(tr, fx)
  f:write(string.format("params: %d\n", n))

  -- Probe NamedConfigParm keys.
  f:write("\nnamed-config-parm hits:\n")
  for _, k in ipairs(probeKeys) do
    local got, val = reaper.TrackFX_GetNamedConfigParm(tr, fx, k)
    if got and val and #val > 0 then
      f:write(string.format("  %-32s = %q\n", k, val))
    end
  end

  -- Probe per-param ident strings. Some VST3 plugins expose internal
  -- IDs that hint at HQ / compare slots even when the human name doesn't.
  f:write("\nparam idents (any non-empty):\n")
  for i = 0, n - 1 do
    local _, pname = reaper.TrackFX_GetParamName(tr, fx, i, "")
    local got, ident = reaper.TrackFX_GetNamedConfigParm(tr, fx, "param." .. i .. ".ident")
    if got and ident and #ident > 0 then
      f:write(string.format("  [%d] %-30s ident=%s\n", i, pname, ident))
    end
  end

  f:write("\n")
end

-- Full track-state chunk — plugin state lives encoded in the <VST ...>
-- block. base64-decode and diff to detect any internal flag flips.
local _, chunk = reaper.GetTrackStateChunk(tr, "", false)
f:write("# full track chunk:\n")
f:write(chunk)
f:write("\n")

f:close()

reaper.ShowConsoleMsg(string.format("probe wrote: %s\n", path))
reaper.ShowMessageBox("Probe written to:\n" .. path
                      .. "\n\nNow toggle HQ / A/B in the plugin GUI and re-run.",
                      "probe_plugin_state", 0)
