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
#include <optional>
#include <span>
#include <string_view>

#include "FocusedParam.h"

namespace uf8 {

// linkIdx convention:
//   0..46  — SSL 360 Link's own VST3 param indices (authoritative virtual
//            strip). See docs/ssl-native-params/VST3__SSL_360_Link_(SSL).md.
//  100+    — extension-defined synthetic IDs for params NOT in SSL 360
//            Link (External S/C, routing toggles, preamp section, Width
//            Mode, Auto Make-up, etc.). Stable across plug-in tables so
//            cross-plugin focused-param chase keeps working. Reference
//            these by name from soft-key bank tables — never inline.
namespace ext {
    constexpr int ExternalSC      = 100;
    constexpr int FiltersToInput  = 101;
    constexpr int DynamicsPreEq   = 102;
    constexpr int FiltersToSC     = 103;
    constexpr int EqToSC          = 104;
    constexpr int FiltersIn       = 108;
    constexpr int WidthMode       = 109;
    constexpr int WidthFreq       = 110;
    constexpr int AutoMakeup      = 111;
    constexpr int AutoMakeupOff   = 112;
    // Hardware-style preamp params on the 4K-series plug-ins. CS2 has no
    // equivalents (modern strip is post-preamp). Variant coverage:
    //   Pre, MicDrive             — 4K B / 4K E / 4K G
    //   ImpedanceIn, Impedance    — 4K G only
    //
    // MicDrive resolves to the native "Mic" gain param (vst3 4 on 4K B/E,
    // vst3 10 on 4K G). The SSL hardware label "Mic/Drive" reflects that
    // the same gain knob serves both Mic preamp and Drive modes; the
    // mode select is exposed as the separate Pre toggle (and on UC1 will
    // surface as a sub-menu — see plugin-bound UC1 work).
    constexpr int Pre             = 113;
    constexpr int MicDrive        = 114;
    constexpr int ImpedanceIn     = 115;
    constexpr int Impedance       = 116;
    // Synthetic toggles that don't map to any VST3 parameter — handled
    // directly by main.cpp's render + V-Pot push paths. Soft-key press
    // sets focus to one of these like any normal slot; per-strip render
    // reads the state from REAPER (TrackPhase) or the SSL plug-in chunk
    // (PluginAB / PluginHQ), and V-Pot push toggles it on that strip's
    // track. No PluginMap entry for these — findSlotByLinkIdx returns
    // nullptr, callers special-case the linkIdx instead.
    constexpr int TrackPhase      = 117;  // REAPER B_PHASE
    constexpr int PluginAB        = 118;  // SSL StateASelected
    constexpr int PluginHQ        = 119;  // SSL HighQuality (PARAM_NON_AUTO)
}

struct LinkSlot {
    int         linkIdx;      // SSL 360 Link virtual-strip slot (stable across plugins)
    const char* id;           // "InputTrim"
    const char* name;         // long label for Value Line ("Input Trim")
    const char* legend;       // short 3-4 char scribble legend ("IN")
    int         vst3Param;    // VST3 parameter index on *this* plugin
    bool        inverted;     // rotate CCW to increase when true

    // Reset target for V-Pot push. nullopt → default to 0.5 (matches the
    // pre-Stage-7 hardcoded behaviour). Populate per-slot to match each
    // SSL plug-in's "neutral" position — 0 dB for gains, lowest cutoff
    // for filters, etc. Tables left at nullopt for now; populate as we
    // confirm each slot's natural reset point against SSL 360°.
    std::optional<double> deflt = std::nullopt;
};

struct PluginMap {
    const char*               match;         // case-sensitive substring of TrackFX_GetFXName
    const char*               displayShort;  // 4-char Channel Strip Type zone label ("CS 2", …)
    Domain                    domain;        // family classification — drives focused-param routing
    std::span<const LinkSlot> slots;         // ordered — focused.slotIdx indexes into this directly
};

// Lookup the first matching plugin map on a track. Walks TrackFX_GetCount
// in order (first hit wins). Returns { nullptr, -1 } when no compatible
// plugin is present.
struct PluginMatch {
    const PluginMap* map;
    int              fxIndex;
};

PluginMatch lookupPluginOnTrack(void* track /*MediaTrack**/);

// Domain-aware lookup. Walks the track's FX chain and returns the first
// plugin whose PluginMap.domain matches `domain`. A track can host both
// CS-family and BC plugins simultaneously (Channel Strip 2 + Bus
// Compressor 2), and the focused-param projection on UF8 needs to route
// to the correct one. `Domain::None` is treated as "no match" (returns
// {nullptr, -1}).
PluginMatch lookupPluginOnTrack(void* track, Domain domain);

// Lookup by raw FX name (substring match). Exposed for tests.
const PluginMap* lookupPluginMapByName(std::string_view fxName);

// All compiled-in maps — for debugging / tests.
std::span<const PluginMap> allPluginMaps();

// Find the SSL 360 Link slot index (linkIdx) whose vst3Param matches.
// Used by cross-device focus sync: when UC1 (or the plugin GUI) writes
// a param, we map "this plugin's VST3 param N" back to the linkIdx so
// the focused-param state can be projected onto UF8 strips that may
// host *different* plugin variants with their own slot orderings.
//
// Returns -1 when no slot maps to that VST3 param. O(N) linear scan;
// slot lists are tiny (≤32) so a hash map would be overkill.
//
// IMPORTANT: returns the LinkSlot.linkIdx field — the SSL 360 Link
// virtual-strip slot number that's stable across plugin variants —
// NOT the array index into PluginMap.slots. Callers store this in
// FocusedParam.slotIdx and look up the slot via findSlotByLinkIdx
// against whichever plugin happens to be on the rendering track.
int slotIdxForVst3Param(const PluginMap& map, int vst3Param);

// Find a slot in `map` by its SSL 360 Link slot index (linkIdx).
// Returns nullptr when the plugin doesn't expose a slot with that
// linkIdx (e.g. CompPeak's linkIdx=25 has no entry on 4K E/G/B).
const LinkSlot* findSlotByLinkIdx(const PluginMap& map, int linkIdx);

} // namespace uf8
