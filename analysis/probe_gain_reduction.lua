-- probe_gain_reduction.lua
--
-- ReaScript. For every FX on the selected track, probes a list of
-- candidate named-config-parms (PreSonus GR-meter standard + likely
-- SSL/legacy variants) and reports which ones the plug-in answers to.
-- Output goes to the REAPER console.
--
-- Use:
--   1. Select a track hosting one of: SSL Native CS2, BC2, 4K E, 4K G, 4K B.
--   2. Drive some audio through it so the comp is actually reducing gain.
--   3. Actions → Load ReaScript → run this file → check console.
--
-- We're hunting for a working GR readback on 4K E/G/B; CS2 is the known-
-- good baseline (must report "GainReduction_dB" -> nonzero string).

local CANDIDATES = {
  "GainReduction_dB",   -- PreSonus standard, confirmed for CS2/BC2
  "GainReduction",
  "Gain_Reduction_dB",
  "GR_dB",
  "GR",
  "gainReduction",
  "gain_reduction",
  "gainReductionDb",
  "Meter_GR",
  "Meter:GR",
  "meter.gr",
  "MeterGR",
  "CompGR",
  "Comp_GR_dB",
}

local tr = reaper.GetSelectedTrack(0, 0)
if not tr then
  reaper.ShowMessageBox("Select the track hosting the plugin under test.",
                        "probe_gain_reduction", 0)
  return
end

reaper.ShowConsoleMsg("\n=== probe_gain_reduction ===\n")
local n = reaper.TrackFX_GetCount(tr)
for fx = 0, n - 1 do
  local _, fxName = reaper.TrackFX_GetFXName(tr, fx, "")
  reaper.ShowConsoleMsg(string.format("\n[%d] %s\n", fx, fxName))

  local hits = 0
  for _, name in ipairs(CANDIDATES) do
    local ok, val = reaper.TrackFX_GetNamedConfigParm(tr, fx, name)
    if ok then
      reaper.ShowConsoleMsg(string.format("  OK  %-22s = %q\n", name, val or ""))
      hits = hits + 1
    end
  end
  if hits == 0 then
    reaper.ShowConsoleMsg("  (no candidates answered)\n")
  end
end
reaper.ShowConsoleMsg("=== done ===\n")
