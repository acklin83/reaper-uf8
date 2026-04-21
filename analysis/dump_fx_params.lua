-- dump_fx_params.lua
--
-- ReaScript. Dumps every VST3 parameter of every FX on the currently
-- selected track to a markdown file in docs/ssl-native-params/.
--
-- Usage:
--   1. In REAPER, select one track that hosts exactly the plugin you
--      want to map (e.g. a fresh track with only "SSL Native Channel
--      Strip 2" inserted).
--   2. Actions → "Load ReaScript..." → pick this file → Run.
--   3. A markdown dump lands at
--      <project>/docs/ssl-native-params/<fx_name>.md
--      relative to this script. The file is suitable as the source
--      material for building extension/data/sslnative/<plugin>.json.
--
-- The dump records, per parameter:
--   - REAPER flat index (what we pass to TrackFX_GetParamNormalized)
--   - plugin-reported name (TrackFX_GetParamName)
--   - current normalized value (0..1)
--   - formatted string (TrackFX_FormatParamValueNormalized) at
--     normalized = 0.0, 0.5, 1.0, and the current position — gives us
--     the unit + scale at a glance

local function sanitize(s)
  s = s:gsub("[/:]", "_")
  s = s:gsub("%s+", "_")
  return s
end

local function scriptDir()
  local info = debug.getinfo(1, "S").source
  if info:sub(1, 1) == "@" then info = info:sub(2) end
  return info:match("(.*/)")
end

local function fmt(tr, fx, idx, norm)
  local ok, s = reaper.TrackFX_FormatParamValueNormalized(tr, fx, idx, norm, "")
  if ok and s and #s > 0 then return s end
  return ""
end

local tr = reaper.GetSelectedTrack(0, 0)
if not tr then
  reaper.ShowMessageBox("No track selected — select the track hosting the "
                        .. "plugin you want to dump, then re-run.",
                        "dump_fx_params", 0)
  return
end

local fxCount = reaper.TrackFX_GetCount(tr)
if fxCount == 0 then
  reaper.ShowMessageBox("Selected track has no FX.", "dump_fx_params", 0)
  return
end

-- If several FX are on this track, dump each into its own file.
local outDir = scriptDir() .. "../docs/ssl-native-params/"
-- mkdir -p
reaper.RecursiveCreateDirectory(outDir, 0)

local summary = {}

for fx = 0, fxCount - 1 do
  local _, fxName = reaper.TrackFX_GetFXName(tr, fx, "")
  local n = reaper.TrackFX_GetNumParams(tr, fx)

  local fname = sanitize(fxName) .. ".md"
  local path = outDir .. fname
  local f = io.open(path, "w")
  if not f then
    reaper.ShowConsoleMsg("Could not open " .. path .. " for writing\n")
    goto continue
  end

  f:write(string.format("# %s\n\n", fxName))
  f:write(string.format("Dumped via dump_fx_params.lua. %d parameter(s).\n\n",
                        n))
  f:write("| idx | name | cur | @0.0 | @0.5 | @1.0 |\n")
  f:write("|----:|------|----:|------|------|------|\n")

  for i = 0, n - 1 do
    local _, paramName = reaper.TrackFX_GetParamName(tr, fx, i, "")
    local cur = reaper.TrackFX_GetParamNormalized(tr, fx, i)
    local at0   = fmt(tr, fx, i, 0.0)
    local at05  = fmt(tr, fx, i, 0.5)
    local at1   = fmt(tr, fx, i, 1.0)
    local atCur = fmt(tr, fx, i, cur)

    local function esc(s) return (s or ""):gsub("|", "\\|") end

    f:write(string.format("| %d | %s | %s (%.4f) | %s | %s | %s |\n",
      i, esc(paramName),
      esc(atCur), cur,
      esc(at0), esc(at05), esc(at1)))
  end

  f:close()
  table.insert(summary, string.format("- [%s](%s) — %d params",
                                      fxName, fname, n))

  ::continue::
end

reaper.ShowConsoleMsg(string.format(
  "dump_fx_params: wrote %d file(s) to %s\n", #summary, outDir))
for _, line in ipairs(summary) do
  reaper.ShowConsoleMsg(line .. "\n")
end
