#include "UC1PluginMap.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "reaper_plugin_functions.h"

namespace uc1 {

namespace {

// Helper to build a PluginBindings initialiser with most entries defaulted
// to kParamNone. We only want to spell out the knobs/buttons the plugin
// actually supports.
constexpr PluginBindings makeEmpty(const char* match, const char* shortName)
{
    PluginBindings b{};
    b.match     = match;
    b.shortName = shortName;
    for (auto& v : b.knobParam)    v = kParamNone;
    for (auto& v : b.buttonParam)  v = kParamNone;
    for (auto& v : b.inverted)     v = false;
    b.bypassParam = kParamNone;
    return b;
}

// ---- Bus Compressor 2 ------------------------------------------------------
//
// VST3 param indices from the existing PluginMap's kBusComp2Slots.
// Only the top-center V-Pots and the Bus Comp IN button are relevant here
// — Bus Comp 2 has no EQ, no filters, no dedicated CS knobs.

PluginBindings makeBusComp2Bindings()
{
    auto b = makeEmpty("Bus Compressor 2", "BC 2");
    b.knobParam[knob::kBCThreshold] = 2;
    b.knobParam[knob::kBCMakeup]    = 3;
    // Attack's UC1 knob ID isn't confirmed yet (0x10 is the likely
    // candidate — we never captured a clean sweep with display evidence).
    // Leave it kParamNone for now; add when uc1_XX pins it down.
    b.knobParam[knob::kBCRelease]   = 5;
    b.knobParam[knob::kBCRatio]     = 6;
    b.knobParam[knob::kBCScHpf]     = 7;
    b.knobParam[knob::kBCMix]       = 8;
    // The Bus Comp IN button toggles the plugin's own Bypass, not a
    // dedicated param — surface handles that via TrackFX_SetEnabled.
    return b;
}

// ---- Native SSL Channel Strip 2 --------------------------------------------
//
// VST3 param indices from kCs2Slots in PluginMap.cpp. All UC1 knobs that
// drive Channel Strip params are covered here.
//
// Note: button IDs for Solo, Cut, Solo Clear, Channel IN, Fine don't
// correspond to plugin params — those live on the track or are surface
// modifiers. Left kParamNone here; surface handles them.

PluginBindings makeChannelStrip2Bindings()
{
    auto b = makeEmpty("Channel Strip 2", "CS 2");

    // Dedicated CS knobs (left side: filters + EQ)
    b.knobParam[knob::kCSLowPass]   = 4;
    b.knobParam[knob::kCSHighPass]  = 5;
    b.knobParam[knob::kCSHfGain]    = 19;
    b.knobParam[knob::kCSHfFreq]    = 18;
    b.knobParam[knob::kCSHmfGain]   = 15;
    b.knobParam[knob::kCSHmfFreq]   = 14;
    b.knobParam[knob::kCSHmfQ]      = 16;
    b.knobParam[knob::kCSLmfGain]   = 12;
    b.knobParam[knob::kCSLmfFreq]   = 11;
    b.knobParam[knob::kCSLmfQ]      = 13;
    b.knobParam[knob::kCSLfFreq]    =  9;
    b.knobParam[knob::kCSLfGain]    = 10;

    // V-Pot repurposing when no Bus Comp 2 is present.
    b.knobParam[knob::kCSInputTrim]  = 2;    // Input Trim
    b.knobParam[knob::kCSFaderLevel] = 38;   // Linkable Fader Level

    // Right side: dynamics + gate
    b.knobParam[knob::kCSCompThreshold] = 24;
    b.knobParam[knob::kCSCompRatio]     = 23;
    b.knobParam[knob::kCSCompRelease]   = 25;
    b.knobParam[knob::kCSGateThreshold] = 30;
    b.knobParam[knob::kCSGateRange]     = 31;
    b.knobParam[knob::kCSGateRelease]   = 32;
    b.knobParam[knob::kCSGateHold]      = 29;

    // Buttons — plugin-local toggles
    b.buttonParam[button::kHfBell]      = 17;  // HighEqBell
    b.buttonParam[button::kEqType]      =  7;  // EqType
    b.buttonParam[button::kEqIn]        =  6;  // EqIn
    b.buttonParam[button::kLfBell]      =  8;  // LowEqBell
    b.buttonParam[button::kFastAttComp] = 22;  // CompFastAttack
    b.buttonParam[button::kPeak]        = 21;  // CompPeak
    b.buttonParam[button::kDynIn]       = 20;  // DynamicsIn
    b.buttonParam[button::kExpand]      = 27;  // GateExpander
    b.buttonParam[button::kFastAttGate] = 28;  // GateAttack (Gate F.Attack)
    b.buttonParam[button::kScListen]    = 37;  // Listen

    // Channel IN toggles plugin bypass — handled via TrackFX_SetEnabled.
    // Polarity, Solo, Cut, Solo Clear, Fine aren't plugin params.

    return b;
}

// ---- 4K E / 4K G / 4K B placeholders --------------------------------------
//
// Param indices exist in the UF8 PluginMap (kCs2Slots counterparts). When
// we wire these up we'll mirror the CS 2 binding above but with the
// matching indices from k4kESlots / k4kGSlots / k4kBSlots. Leaving as
// empty binders for now keeps them discoverable by match-string even if
// knob/button routing returns kParamNone — surface just ignores events.
PluginBindings make4kEBindings() { return makeEmpty("4K E", "4K E"); }
PluginBindings make4kGBindings() { return makeEmpty("4K G", "4K G"); }
PluginBindings make4kBBindings() { return makeEmpty("4K B", "4K B"); }

// Registry. Order: most-specific substring first (same convention as the
// UF8 PluginMap). BC 2's match string wouldn't collide with any of the
// Channel Strip variants so ordering there isn't critical, but the 4K
// series all contain "SSL" so putting the longer "4K G" before "4K E"
// before "4K B" keeps lookupBindingsByName unambiguous.
const PluginBindings& csReg() { static auto v = makeChannelStrip2Bindings(); return v; }
const PluginBindings& bcReg() { static auto v = makeBusComp2Bindings();      return v; }
const PluginBindings& e4Reg() { static auto v = make4kEBindings();           return v; }
const PluginBindings& g4Reg() { static auto v = make4kGBindings();           return v; }
const PluginBindings& b4Reg() { static auto v = make4kBBindings();           return v; }

const PluginBindings* kChannelStripCandidates[] = {
    &csReg(), &g4Reg(), &e4Reg(), &b4Reg(),
};

} // namespace

const PluginBindings* lookupBindingsByName(std::string_view fxName)
{
    if (fxName.find(bcReg().match) != std::string_view::npos) return &bcReg();
    for (const auto* b : kChannelStripCandidates) {
        if (fxName.find(b->match) != std::string_view::npos) return b;
    }
    return nullptr;
}

ControlDomain classifyKnob(uint8_t knobId)
{
    // Top V-Pots live in 0x0C..0x16 and repurpose per plugin. The two
    // we've confirmed driving CS params (0x0C Input Trim, 0x16 Fader
    // Level) are only handled that way when no Bus Comp is on the
    // track. When a Bus Comp IS present SSL 360° puts those V-Pots on
    // Bus Comp params (Bus Comp 2 has no input-trim, so 0x0C might sit
    // unused in that context — needs a confirming capture).
    //
    // Implementation: the surface checks whether Bus Comp bindings
    // exist; if yes, route V-Pot IDs to Bus Comp. If no and CS
    // bindings exist, fall back to Channel Strip. This classify()
    // returns the *default* which is Bus Comp for the V-Pot range.
    if (knobId >= 0x0C && knobId <= 0x16) return ControlDomain::BusComp;
    return ControlDomain::ChannelStrip;
}

ControlDomain classifyButton(uint8_t buttonId)
{
    if (buttonId == button::kBusCompIn) return ControlDomain::BusComp;
    return ControlDomain::ChannelStrip;
}

UC1Bindings lookupBindingsOnTrack(void* trackRaw)
{
    UC1Bindings result;
    MediaTrack* tr = static_cast<MediaTrack*>(trackRaw);
    if (!tr) return result;

    const int n = TrackFX_GetCount(tr);
    char buf[256];
    for (int i = 0; i < n; ++i) {
        if (!TrackFX_GetFXName(tr, i, buf, sizeof(buf))) continue;
        std::string_view name{buf};
        const PluginBindings* b = lookupBindingsByName(name);
        if (!b) continue;

        // Bus Comp 2 is identified by its shortName == "BC 2"; everything
        // else in the registry is a Channel Strip variant.
        const bool isBusComp = (std::strcmp(b->shortName, "BC 2") == 0);

        if (isBusComp) {
            if (!result.busCompMap) {
                result.busCompMap   = b;
                result.busCompFxIdx = i;
            }
        } else {
            if (!result.channelMap) {
                result.channelMap   = b;
                result.channelFxIdx = i;
            }
        }

        // Bail when both slots found.
        if (result.busCompMap && result.channelMap) break;
    }
    return result;
}

} // namespace uc1
