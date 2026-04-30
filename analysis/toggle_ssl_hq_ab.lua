-- toggle_ssl_hq_ab.lua
--
-- Prototype for chunk-replace toggling of SSL plug-in's HQ Mode and
-- A/B compare on the selected track's first SSL Native CS / 4K plug-in.
-- Both states live inside the base64-encoded VST3 chunk:
--
--   HQ Mode = <PARAM_NON_AUTO id="HighQuality" value="0.0|1.0"/>
--             (per-slot — appears in both <A> and <B> blocks)
--   A/B     = <SSL_PLUGIN_STATE ... StateASelected="0|1" ...>
--             (top-level attribute)
--
-- Both diffs are length-preserving so chunk size stays identical and we
-- don't have to touch any binary length headers in the VST3 wrapper.
--
-- Usage: bind one of these script-actions to a REAPER hotkey:
--   - toggle_ssl_hq.lua   (calls run("hq"))
--   - toggle_ssl_ab.lua   (calls run("ab"))
-- For now this script reads ARG = first argv (set via reaper.SetExtState
-- before Action: Run script… or hard-code below for testing).

----------------------------------------------------------------
-- pure-Lua base64 (we don't depend on SWS for portability)
----------------------------------------------------------------
local b64alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local b64dec_lookup = {}
for i = 1, #b64alpha do b64dec_lookup[b64alpha:sub(i, i)] = i - 1 end

local function b64decode(s)
  s = s:gsub("%s", ""):gsub("=+$", "")
  local out = {}
  local buf, bits = 0, 0
  for i = 1, #s do
    local v = b64dec_lookup[s:sub(i, i)]
    if v then
      buf = buf * 64 + v
      bits = bits + 6
      if bits >= 8 then
        bits = bits - 8
        local byte = math.floor(buf / 2 ^ bits) % 256
        out[#out + 1] = string.char(byte)
        buf = buf % 2 ^ bits
      end
    end
  end
  return table.concat(out)
end

local function b64encode(s)
  local out = {}
  local buf, bits = 0, 0
  for i = 1, #s do
    buf = buf * 256 + s:byte(i)
    bits = bits + 8
    while bits >= 6 do
      bits = bits - 6
      local v = math.floor(buf / 2 ^ bits) % 64
      out[#out + 1] = b64alpha:sub(v + 1, v + 1)
      buf = buf % 2 ^ bits
    end
  end
  if bits > 0 then
    local v = (buf * 2 ^ (6 - bits)) % 64
    out[#out + 1] = b64alpha:sub(v + 1, v + 1)
  end
  local enc = table.concat(out)
  local pad = (4 - (#enc % 4)) % 4
  return enc .. string.rep("=", pad)
end

----------------------------------------------------------------
-- chunk patching
----------------------------------------------------------------

-- Operate on the FIRST <VST "VST3: SSL ..."> block found in `chunk`.
-- Returns the patched chunk + a status string (or nil + error msg).
local function patchSslVstBlock(chunk, op)
  -- Locate the SSL VST block
  local headStart = chunk:find('<VST "VST3: SSL ', 1, true)
  if not headStart then return nil, "no SSL VST3 plug-in on track" end

  -- Header line ends at the next \n
  local headEnd = chunk:find("\n", headStart, true)
  if not headEnd then return nil, "malformed VST header" end

  -- VST block body runs until a line containing only ">". Find the
  -- "\n>\n" sentinel.
  local bodyEnd = chunk:find("\n>\n", headEnd, true)
  if not bodyEnd then return nil, "no VST close marker" end

  local headerLine = chunk:sub(headStart, headEnd - 1)
  local body       = chunk:sub(headEnd + 1, bodyEnd - 1) -- between header\n and \n>
  local prefix     = chunk:sub(1, headStart - 1)
  local suffix     = chunk:sub(bodyEnd)                  -- starts with "\n>\n..."

  -- REAPER stores the VST chunk as MULTIPLE base64 blocks, one per line,
  -- each with its own padding (line endings often `==`). Concatenating
  -- before decoding stops at the first `=` — must decode line-by-line.
  local lines = {}
  for line in body:gmatch("[^\n]+") do
    if line:match("%S") then table.insert(lines, line) end
  end
  local binParts, lineByteLens = {}, {}
  for _, ln in ipairs(lines) do
    local b = b64decode(ln)
    table.insert(binParts, b)
    table.insert(lineByteLens, #b)
  end
  local bin = table.concat(binParts)

  -- Find the XML payload boundaries inside the binary blob
  local xmlStart = bin:find("<?xml", 1, true)
  if not xmlStart then return nil, "no XML in plug-in state" end
  local xmlEnd = bin:find("</SSL_PLUGIN_STATE>", xmlStart, true)
  if not xmlEnd then return nil, "no SSL_PLUGIN_STATE close tag" end
  xmlEnd = xmlEnd + #"</SSL_PLUGIN_STATE>" - 1

  local xml = bin:sub(xmlStart, xmlEnd)
  local newXml, msg

  if op == "ab" then
    -- Top-level StateASelected="0" ↔ StateASelected="1"
    local cur = xml:match('StateASelected="(%d)"')
    if not cur then return nil, "StateASelected attr not found" end
    local nxt = (cur == "1") and "0" or "1"
    newXml = xml:gsub('StateASelected="' .. cur .. '"',
                      'StateASelected="' .. nxt .. '"', 1)
    msg = string.format("A/B: %s → %s", cur, nxt)

  elseif op == "hq" then
    -- Patch only the active slot's HighQuality. Active slot determined
    -- by StateASelected: "1" → <A>, "0" → <B>. SSL XML structure:
    --   <SSL_PLUGIN_STATE ...><A ...>...</A><B ...>...</B>...
    local active = xml:match('StateASelected="(%d)"')
    if not active then return nil, "StateASelected attr not found" end
    local slotTag = (active == "1") and "A" or "B"

    -- Find slot block
    local slotStart, slotBodyStart = xml:find("<" .. slotTag .. "[^>]*>", 1)
    if not slotStart then return nil, "no <" .. slotTag .. "> slot" end
    local slotEnd = xml:find("</" .. slotTag .. ">", slotBodyStart, true)
    if not slotEnd then return nil, "no </" .. slotTag .. "> close" end

    local slotBody = xml:sub(slotBodyStart + 1, slotEnd - 1)
    local cur = slotBody:match(
      '<PARAM_NON_AUTO id="HighQuality" value="(%d+%.?%d*)"/?>')
    if not cur then return nil, "no HighQuality in slot " .. slotTag end
    local nxt = (tonumber(cur) or 0) > 0.5 and "0.0" or "1.0"
    local newSlotBody = slotBody:gsub(
      '<PARAM_NON_AUTO id="HighQuality" value="' .. cur .. '"/>',
      '<PARAM_NON_AUTO id="HighQuality" value="' .. nxt .. '"/>',
      1)
    if newSlotBody == slotBody then
      return nil, "HighQuality replace failed (regex mismatch)"
    end
    newXml = xml:sub(1, slotBodyStart) .. newSlotBody .. xml:sub(slotEnd)
    msg = string.format("HQ slot %s: %s → %s", slotTag, cur, nxt)

  else
    return nil, "unknown op: " .. tostring(op)
  end

  -- length must be preserved (sanity)
  if #newXml ~= #xml then
    return nil, string.format("length drift: %d → %d", #xml, #newXml)
  end

  -- Splice patched XML back into binary
  local newBin = bin:sub(1, xmlStart - 1) .. newXml .. bin:sub(xmlEnd + 1)

  -- Re-encode preserving original per-line block boundaries: decode each
  -- line gave us its byte-length; cut newBin at those lengths and encode
  -- each slice independently so REAPER's chunk parser sees the same
  -- multi-block layout it wrote.
  local outLines = {}
  local cursor = 1
  for _, n in ipairs(lineByteLens) do
    local slice = newBin:sub(cursor, cursor + n - 1)
    table.insert(outLines, b64encode(slice))
    cursor = cursor + n
  end
  local newBody = table.concat(outLines, "\n")

  -- Reassemble chunk
  local newChunk = prefix .. headerLine .. "\n" .. newBody .. suffix
  return newChunk, msg
end

----------------------------------------------------------------
-- entry point
----------------------------------------------------------------

local op = reaper.GetExtState("ReaSixty", "test_chunk_op")
if not op or #op == 0 then op = "hq" end -- default for direct-run testing

local tr = reaper.GetSelectedTrack(0, 0)
if not tr then
  reaper.ShowMessageBox("Select a track with the SSL plug-in.",
                        "toggle_ssl_hq_ab", 0)
  return
end

local _, chunk = reaper.GetTrackStateChunk(tr, "", false)
local patched, msg = patchSslVstBlock(chunk, op)

if not patched then
  reaper.ShowConsoleMsg("toggle failed: " .. (msg or "unknown") .. "\n")
  return
end

reaper.PreventUIRefresh(1)
local ok = reaper.SetTrackStateChunk(tr, patched, false)
reaper.PreventUIRefresh(-1)

reaper.ShowConsoleMsg(string.format("[%s] op=%s ok=%s — %s\n",
  os.date("%H:%M:%S"), op, tostring(ok), msg or ""))
