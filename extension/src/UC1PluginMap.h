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
    // GR-meter override per slot — populated only when the binding came
    // from a learned (user-mapped) plugin AND the user designated a
    // VST3 parameter as the gain-reduction readout in Settings → FX
    // Learn. -1 means "fall back to TrackFX_GetNamedConfigParm
    // (GainReduction_dB)" (built-in plug-ins, or learned plug-ins with
    // no GR pick yet). Offset is added to the formatted-value parse so
    // a plug-in that reports negative-going GR can be shifted into
    // positive dB for the 0..max display.
    int                   busCompGrParam   = -1;
    double                busCompGrOffsetDb = 0.0;
    int                   channelGrParam   = -1;
    double                channelGrOffsetDb = 0.0;
};

// Walk TrackFX_GetCount on a track and return both the Bus Comp and
// Channel Strip bindings (if any). The Nth-match per category is
// determined by bcInstanceIndex / csInstanceIndex — default 0 picks
// the first match, the Shift+Channel-Encoder cycle bumps it for the
// next render.
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

// ---- Multi-instance picker -----------------------------------------------
// A track may host more than one BC and/or CS plug-in (e.g. built-in
// BC 2 followed by a user-learned BC). Each domain has its own active
// instance index; default 0 picks the first match in track-FX order.
// State is GUID-keyed and in-memory only — reset per session, not
// persisted.
int  bcInstanceIndex(void* track);
int  csInstanceIndex(void* track);
void setBcInstanceIndex(void* track, int idx);
void setCsInstanceIndex(void* track, int idx);
int  bcInstanceCount(void* track);
int  csInstanceCount(void* track);
// Step the active instance by `delta`, wrapping around the count.
// no-op when count <= 1.
void cycleInstance(void* track, ControlDomain dom, int delta);
// Map an FX index on a track back to its instance position within the
// domain. Returns -1 when the FX isn't a recognised CS/BC binding.
// Used by chaseLastTouchedFx so a click in REAPER's plug-in GUI
// snaps the active instance to whichever copy the user just touched.
int instanceIndexForFx(void* track, int fxIdx);

} // namespace uc1
