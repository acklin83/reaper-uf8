#pragma once
//
// UC1PluginMap — UC1 hardware control IDs → VST3 parameter indices per
// supported SSL plugin.
//
// UC1's surface is a fixed layout, so unlike UF8's pageable slots each
// UC1 knob / button maps to exactly one plugin parameter. Two plugin
// contexts matter:
//
//   * Bus Comp 2 — drives the top-center 7 V-Pots (knob IDs 0x0E..0x16)
//     and the Bus Comp IN button (0x0C).
//   * Channel Strip (CS 2 / 4K E / 4K G / 4K B) — drives the dedicated
//     EQ knobs (0x00..0x0B) and the Dyn/Gate knobs (0x17..0x1D), plus
//     the Channel-section buttons and plugin-bypass (Channel IN 0x1E).
//     When NO Bus Comp 2 is loaded SSL 360° repurposes the V-Pots 0x0C
//     and 0x16 to drive Channel-Strip Input Trim / Fader Level.
//
// A track can have both — UC1 drives whichever plugin each knob/button
// belongs to. `Bindings` is what the surface looks up at the top of
// every event handler.
//

#include <cstdint>
#include <string_view>

#include "UC1Protocol.h"

namespace uc1 {

// Sentinel "this control isn't mapped for this plugin" — skip the event.
constexpr int kParamNone = -1;

// Per-plugin mapping: indexed by UC1 control ID, returns VST3 param idx.
// Static tables live in UC1PluginMap.cpp.
struct PluginBindings {
    const char* match;             // substring of TrackFX_GetFXName
    const char* shortName;         // 4-char display ("CS 2", "BC 2", "4K E"…)

    // knob ID → VST3 param index. -1 = unmapped.
    // Covers all 30 UC1 knob IDs (0x00..0x1D). Index into this array is
    // the raw knob_id byte from a FF 24 event.
    int knobParam[0x20];

    // button ID → VST3 param index. -1 = unmapped.
    // Covers all 32 UC1 button IDs (0x00..0x1F). Index is the raw button
    // byte from a FF 22 event.
    int buttonParam[0x20];

    // VST3 index of the plugin's "bypass" parameter, if any. Used by
    // the Channel IN / Bus Comp IN buttons to bypass the whole plugin.
    // -1 = no mapped bypass (plugin bypass via TrackFX_SetEnabled).
    int bypassParam;

    // Sign-flip per knob. For params where physical CW should decrease
    // the VST3 value (rare; most SSL params are CW=up). Indexed by knob
    // ID like knobParam.
    bool inverted[0x20];
};

// Resolved lookup for a given track state: two plugin contexts plus
// each plugin's FX index in the track's chain.
struct UC1Bindings {
    const PluginBindings* busCompMap    = nullptr;  // Bus Compressor 2 on track (or null)
    int                   busCompFxIdx  = -1;
    const PluginBindings* channelMap    = nullptr;  // Channel Strip plugin on track (or null)
    int                   channelFxIdx  = -1;
};

// Walk TrackFX_GetCount on a track and return both the Bus Comp and
// Channel Strip bindings (if any). First-hit per category.
UC1Bindings lookupBindingsOnTrack(void* track /*MediaTrack**/);

// Lookup by raw FX name (substring). Exposed for tests.
const PluginBindings* lookupBindingsByName(std::string_view fxName);

// Kind of a control — helps the surface decide which plugin slot to
// route a knob to when both Bus Comp and Channel Strip are on the track.
enum class ControlDomain {
    BusComp,        // top V-Pots + Bus Comp IN button
    ChannelStrip,   // everything else on the UC1
};

// Classify a knob ID — the UC1's top V-Pots (0x0E..0x16) go to Bus Comp
// when present, else fall back to Channel Strip repurposing.
ControlDomain classifyKnob(uint8_t knobId);

// Classify a button — Bus Comp IN (0x0C) is the sole Bus Comp button,
// everything else belongs to Channel Strip.
ControlDomain classifyButton(uint8_t buttonId);

} // namespace uc1
