#include "PluginChunkPatch.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "reaper_plugin_functions.h"

namespace uf8 {
namespace {

// ---- base64 ----------------------------------------------------------

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr int8_t b64DecodeValue(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string b64Decode(std::string_view s) {
    std::string out;
    out.reserve((s.size() * 3) / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int8_t v = b64DecodeValue(c);
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
            buf &= (1u << bits) - 1u;
        }
    }
    return out;
}

std::string b64Encode(std::string_view s) {
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        buf = (buf << 8) | static_cast<uint8_t>(c);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kB64Alphabet[(buf >> bits) & 0x3F]);
            buf &= (1u << bits) - 1u;
        }
    }
    if (bits > 0) {
        out.push_back(kB64Alphabet[(buf << (6 - bits)) & 0x3F]);
    }
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ---- XML payload patches --------------------------------------------

using PatchFn = bool (*)(std::string& xml);

// Flip <PARAM_NON_AUTO id="HighQuality" value="X.0"/> in the active A/B
// slot. Active slot determined by StateASelected attribute. No-op (false)
// if the plug-in's XML schema doesn't have HighQuality (BC2 doesn't —
// BC2 exposes oversampling as a normal automatable param instead).
bool patchHighQuality(std::string& xml) {
    constexpr std::string_view kSasMarker = "StateASelected=\"";
    const size_t sasPos = xml.find(kSasMarker);
    if (sasPos == std::string::npos) return false;
    const size_t activeIdx = sasPos + kSasMarker.size();
    if (activeIdx >= xml.size()) return false;
    const char active = xml[activeIdx];
    if (active != '0' && active != '1') return false;
    const char slotTag = (active == '1') ? 'A' : 'B';

    // Slot opens with "<A " or "<A>" / "<B " or "<B>"; reject false
    // matches like <AbsoluteSomething>.
    const std::string slotOpen = std::string("<") + slotTag;
    const size_t slotStart = xml.find(slotOpen, activeIdx);
    if (slotStart == std::string::npos) return false;
    const char afterTag = (slotStart + 2 < xml.size())
        ? xml[slotStart + 2] : '\0';
    if (afterTag != ' ' && afterTag != '>') return false;
    const size_t slotBodyStart = xml.find('>', slotStart);
    if (slotBodyStart == std::string::npos) return false;
    const std::string slotClose = std::string("</") + slotTag + ">";
    const size_t slotEnd = xml.find(slotClose, slotBodyStart);
    if (slotEnd == std::string::npos) return false;

    constexpr std::string_view kHqPattern =
        "<PARAM_NON_AUTO id=\"HighQuality\" value=\"";
    const size_t hqPos = xml.find(kHqPattern, slotBodyStart);
    if (hqPos == std::string::npos || hqPos > slotEnd) return false;
    const size_t valStart = hqPos + kHqPattern.size();
    const size_t valEnd = xml.find('"', valStart);
    if (valEnd == std::string::npos) return false;

    const std::string cur = xml.substr(valStart, valEnd - valStart);
    const std::string nxt = (cur == "1.0") ? "0.0" : "1.0";
    if (cur.size() != nxt.size()) return false;
    xml.replace(valStart, cur.size(), nxt);
    return true;
}

// Flip the StateASelected attribute (single-digit "0" / "1"). Works on
// every SSL plug-in with an A/B compare (CS-family + BC).
bool patchStateASelected(std::string& xml) {
    constexpr std::string_view kSasMarker = "StateASelected=\"";
    const size_t sasPos = xml.find(kSasMarker);
    if (sasPos == std::string::npos) return false;
    const size_t idx = sasPos + kSasMarker.size();
    if (idx >= xml.size()) return false;
    const char cur = xml[idx];
    if (cur != '0' && cur != '1') return false;
    xml[idx] = (cur == '1') ? '0' : '1';
    return true;
}

// ---- chunk walker ---------------------------------------------------

// In-place mutation of a single VST block within `chunk`, where the
// block boundaries are [headStart .. blockEnd) (blockEnd points to the
// first char of the closing "\n>\n"). Decodes per-line, attempts the
// patch, re-encodes preserving original line byte boundaries, and
// substitutes the new body. Returns true if the patch was applied
// (chunk size unchanged on success).
bool patchVstBlock(std::string& chunk,
                   size_t headStart,
                   size_t headEnd,
                   size_t bodyEnd,
                   PatchFn patch)
{
    std::vector<size_t> lineByteLens;
    std::string bin;
    {
        size_t pos = headEnd + 1;
        while (pos < bodyEnd) {
            size_t eol = chunk.find('\n', pos);
            if (eol == std::string::npos || eol > bodyEnd) eol = bodyEnd;
            std::string_view line(chunk.data() + pos, eol - pos);
            bool hasContent = false;
            for (char c : line) {
                if (c != ' ' && c != '\t' && c != '\r') { hasContent = true; break; }
            }
            if (hasContent) {
                std::string decoded = b64Decode(line);
                lineByteLens.push_back(decoded.size());
                bin += decoded;
            }
            pos = eol + 1;
        }
    }

    constexpr std::string_view kXmlStartMarker = "<?xml";
    constexpr std::string_view kXmlEndMarker   = "</SSL_PLUGIN_STATE>";
    const size_t xs = bin.find(kXmlStartMarker);
    if (xs == std::string::npos) return false;
    size_t xe = bin.find(kXmlEndMarker, xs);
    if (xe == std::string::npos) return false;
    xe += kXmlEndMarker.size();

    std::string xml = bin.substr(xs, xe - xs);
    const size_t origLen = xml.size();
    if (!patch(xml)) return false;
    if (xml.size() != origLen) return false;

    std::string newBin = bin.substr(0, xs) + xml + bin.substr(xe);

    std::string newBody;
    newBody.reserve(bodyEnd - headEnd);
    size_t cursor = 0;
    for (size_t n : lineByteLens) {
        if (!newBody.empty()) newBody.push_back('\n');
        newBody += b64Encode(std::string_view(newBin.data() + cursor, n));
        cursor += n;
    }

    // Substitute body in place. Old body was [headEnd+1 .. bodyEnd).
    chunk.replace(headEnd + 1, bodyEnd - (headEnd + 1), newBody);
    return true;
}

// Walk all <VST "VST3: SSL ..."> blocks in `chunk`, apply `patch` to
// each, return count of successful patches. Mutates `chunk` in place.
int forEachSslVstBlock(std::string& chunk, PatchFn patch) {
    constexpr std::string_view kVstMarker = "<VST \"VST3: SSL ";
    int patched = 0;
    size_t searchFrom = 0;
    while (true) {
        const size_t headStart = chunk.find(kVstMarker, searchFrom);
        if (headStart == std::string::npos) break;
        const size_t headEnd = chunk.find('\n', headStart);
        if (headEnd == std::string::npos) break;
        const size_t bodyEnd = chunk.find("\n>\n", headEnd);
        if (bodyEnd == std::string::npos) break;

        const size_t origBodyLen = bodyEnd - (headEnd + 1);
        if (patchVstBlock(chunk, headStart, headEnd, bodyEnd, patch)) {
            ++patched;
        }
        // patchVstBlock preserves base-64 line content but the new body
        // length may differ (different line counts → different newline
        // count). Recompute the offset for the next iteration from the
        // current replaced block by walking past its (possibly new)
        // close marker.
        const size_t nextStart = chunk.find("\n>\n", headEnd);
        if (nextStart == std::string::npos) break;
        searchFrom = nextStart + 3;
        (void)origBodyLen;
    }
    return patched;
}

// ---- public driver --------------------------------------------------

int applyToAllSsl(MediaTrack* tr, PatchFn patch) {
    if (!tr) return 0;
    constexpr int kChunkBufSize = 1 << 20;  // 1 MB — heaviest chunks I've
                                            // seen are < 16 KB.
    std::vector<char> buf(kChunkBufSize, 0);
    if (!GetTrackStateChunk(tr, buf.data(), kChunkBufSize, false)) return 0;

    std::string chunk(buf.data());
    const int patched = forEachSslVstBlock(chunk, patch);
    if (patched == 0) return 0;

    return SetTrackStateChunk(tr, chunk.c_str(), false) ? patched : 0;
}

}  // namespace

int togglePluginHQ(MediaTrack* tr) { return applyToAllSsl(tr, patchHighQuality); }
int togglePluginAB(MediaTrack* tr) { return applyToAllSsl(tr, patchStateASelected); }

}  // namespace uf8
