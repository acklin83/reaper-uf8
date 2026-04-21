#pragma once
//
// PluginMap — cross-references SSL plugins to their UF8-addressable
// parameters.
//
// In SSL 360°'s Plug-in Mixer mode, each UF8 strip is bound to one
// REAPER track's SSL plugin. The V-Pot and the scribble strip show ONE
// of that plugin's parameters at a time. Global Page ←/→ buttons shift
// all strips simultaneously to the next parameter in the plugin's
// slot list — strips with different plugins each advance in their own
// list.
//
// So per plugin we store a flat, ordered list of slots. strip[s] renders
// slots[pageIdx] from the plugin on track[s]. Slot metadata (id, legend,
// name) comes from SSL 360 Link's virtual-strip numbering, so the same
// page index yields comparable params across 4K B / 4K E / 4K G / CS2.
//
// Tables are compiled in. A JSON / .factory loader for third-party
// plugins is a Phase-2 concern.
//

#include <cstdint>
#include <span>
#include <string_view>

namespace uf8 {

struct LinkSlot {
    int         linkIdx;      // SSL 360 Link virtual-strip slot (stable across plugins)
    const char* id;           // "InputTrim"
    const char* name;         // long label for Value Line ("Input Trim")
    const char* legend;       // short 3-4 char scribble legend ("IN")
    int         vst3Param;    // VST3 parameter index on *this* plugin
    bool        inverted;     // rotate CCW to increase when true
};

struct PluginMap {
    const char*              match;         // case-sensitive substring of TrackFX_GetFXName
    const char*              displayShort;  // 4-char Channel Strip Type zone label ("CS 2", …)
    std::span<const LinkSlot> slots;        // ordered — pageIdx indexes into this directly
};

// Lookup the first matching plugin map on a track. Walks TrackFX_GetCount
// in order (first hit wins). Returns { nullptr, -1 } when no compatible
// plugin is present.
struct PluginMatch {
    const PluginMap* map;
    int              fxIndex;
};

PluginMatch lookupPluginOnTrack(void* track /*MediaTrack**/);

// Lookup by raw FX name (substring match). Exposed for tests.
const PluginMap* lookupPluginMapByName(std::string_view fxName);

// All compiled-in maps — for debugging / tests.
std::span<const PluginMap> allPluginMaps();

} // namespace uf8
