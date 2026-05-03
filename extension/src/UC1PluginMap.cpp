#include "UC1PluginMap.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "reaper_plugin_functions.h"

#include "FocusedParam.h"        // uf8::Domain
#include "UserPluginCatalog.h"   // uf8::user_plugins

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
    b.knobParam[knob::kBCAttack]    = 4;
    b.knobParam[knob::kBCRelease]   = 5;
    b.knobParam[knob::kBCRatio]     = 6;
    b.knobParam[knob::kBCScHpf]     = 7;
    b.knobParam[knob::kBCMix]       = 8;
    // BC IN button = the plug-in's own "Comp Bypass" param at vst3 idx 10
    // (not REAPER's TrackFX_Enabled). Inverted semantic: param=1 means
    // bypassed → IN is OFF, so the LED state is the inverse of the read.
    b.bypassParam = 10;
    return b;
}

// ---- SSL 360 Link Bus Compressor (the wrapper variant) --------------------
//
// Same UC1 layout as Native BC 2 but vst3 indices line up 1:1 with the
// SSL 360 Link strip (CompBypass at 0; Threshold/Makeup/Attack/Release/
// Ratio/HPF/Mix at 1..7). See
// docs/ssl-native-params/VST3__SSL_360_Link_Bus_Compressor_(SSL).md.

PluginBindings makeSsl360LinkBcBindings()
{
    auto b = makeEmpty("SSL 360 Link Bus Compressor", "L-BC");
    b.knobParam[knob::kBCThreshold] = 1;
    b.knobParam[knob::kBCMakeup]    = 2;
    b.knobParam[knob::kBCAttack]    = 3;
    b.knobParam[knob::kBCRelease]   = 4;
    b.knobParam[knob::kBCRatio]     = 5;
    b.knobParam[knob::kBCScHpf]     = 6;
    b.knobParam[knob::kBCMix]       = 7;
    // BC IN button → "CompBypass" at vst3 0 on the wrapper.
    b.bypassParam = 0;
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

// Apply the "knob goes the wrong way" inversions the user reported after
// hardware testing 2026-04-24: Low Pass and the two Mid-Q knobs run
// physically CCW = VST3 value up, so we flip the sign at dispatch time.
static void applyCsInversions(PluginBindings& b)
{
    b.inverted[knob::kCSLowPass]        = true;
    b.inverted[knob::kCSHmfQ]           = true;
    b.inverted[knob::kCSLmfQ]           = true;
    b.inverted[knob::kCSCompThreshold]  = true;
    b.inverted[knob::kCSGateThreshold]  = true;
}

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

    // CS IN button (UC1 ChannelIn) → plug-in's own "Bypass" param at
    // vst3 idx 0 (NOT REAPER's TrackFX_Enabled). Inverted semantic.
    b.bypassParam = 0;

    applyCsInversions(b);
    return b;
}

// ---- SSL 360 Link (CS wrapper variant) -------------------------------------
//
// vst3 indices match the SSL 360 Link strip 1:1 (linkIdx == vst3). VST3
// value semantic matches the 4K-series wrappers (NOT Native CS 2) for
// the threshold/Q knobs — user confirmed 2026-05-01 that applying the
// CS-2 inversions here flips Comp/Gate Threshold and HMF/LMF Q. So we
// skip applyCsInversions. See
// docs/ssl-native-params/VST3__SSL_360_Link_(SSL).md.

PluginBindings makeSsl360LinkBindings()
{
    auto b = makeEmpty("SSL 360 Link", "Link");

    b.knobParam[knob::kCSLowPass]   = 6;
    b.knobParam[knob::kCSHighPass]  = 7;
    b.knobParam[knob::kCSHfGain]    = 9;
    b.knobParam[knob::kCSHfFreq]    = 10;
    b.knobParam[knob::kCSHmfGain]   = 11;
    b.knobParam[knob::kCSHmfFreq]   = 12;
    b.knobParam[knob::kCSHmfQ]      = 13;
    b.knobParam[knob::kCSLmfGain]   = 16;
    b.knobParam[knob::kCSLmfFreq]   = 17;
    b.knobParam[knob::kCSLmfQ]      = 18;
    b.knobParam[knob::kCSLfFreq]    = 19;
    b.knobParam[knob::kCSLfGain]    = 20;

    // V-Pot repurposing when no Bus Comp present — wrapper has Input Trim
    // at vst3 4 and Fader Level at vst3 1. Per the 2026-04-30 user
    // instruction we route Plugin-mode fader to "Fader Level" (1), not
    // the "Linkable Fader Level" alias (35).
    b.knobParam[knob::kCSInputTrim]  = 4;
    b.knobParam[knob::kCSFaderLevel] = 1;

    b.knobParam[knob::kCSCompThreshold] = 27;
    b.knobParam[knob::kCSCompRatio]     = 26;
    b.knobParam[knob::kCSCompRelease]   = 28;
    b.knobParam[knob::kCSGateThreshold] = 30;
    b.knobParam[knob::kCSGateRange]     = 29;
    b.knobParam[knob::kCSGateRelease]   = 31;
    b.knobParam[knob::kCSGateHold]      = 32;

    b.buttonParam[button::kHfBell]      = 8;   // HighEqBell
    b.buttonParam[button::kEqType]      = 14;
    b.buttonParam[button::kEqIn]        = 15;
    b.buttonParam[button::kLfBell]      = 21;  // LowEqBell
    b.buttonParam[button::kFastAttComp] = 24;  // CompFastAttack
    b.buttonParam[button::kPeak]        = 25;  // CompPeak
    b.buttonParam[button::kDynIn]       = 22;
    b.buttonParam[button::kExpand]      = 33;  // GateExpander
    b.buttonParam[button::kFastAttGate] = 34;  // GateAttack
    b.buttonParam[button::kScListen]    = 36;  // Listen

    // CS IN → wrapper's own Bypass at vst3 0 (matches Native CS 2 idiom).
    b.bypassParam = 0;
    // No applyCsInversions — see comment above.
    return b;
}

// ---- 4K E ------------------------------------------------------------------
// VST3 param indices from kCs2Slots' "4K E" counterpart (PluginMap.cpp).
// Same UC1 knob/button layout; differences: EQ Type is "EQ Colour", no
// Comp Peak, no Gate Hold.
PluginBindings make4kEBindings()
{
    auto b = makeEmpty("4K E", "4K E");
    b.knobParam[knob::kCSLowPass]       = 15;
    b.knobParam[knob::kCSHighPass]      = 14;
    b.knobParam[knob::kCSHfGain]        = 31;
    b.knobParam[knob::kCSHfFreq]        = 30;
    b.knobParam[knob::kCSHmfGain]       = 28;
    b.knobParam[knob::kCSHmfFreq]       = 27;
    b.knobParam[knob::kCSHmfQ]          = 29;
    b.knobParam[knob::kCSLmfGain]       = 25;
    b.knobParam[knob::kCSLmfFreq]       = 24;
    b.knobParam[knob::kCSLmfQ]          = 26;
    b.knobParam[knob::kCSLfFreq]        = 21;
    b.knobParam[knob::kCSLfGain]        = 22;
    b.knobParam[knob::kCSInputTrim]     =  2;
    b.knobParam[knob::kCSFaderLevel]    =  6;
    b.knobParam[knob::kCSCompThreshold] = 36;
    b.knobParam[knob::kCSCompRatio]     = 35;
    b.knobParam[knob::kCSCompRelease]   = 37;
    b.knobParam[knob::kCSGateThreshold] = 43;
    b.knobParam[knob::kCSGateRange]     = 42;
    b.knobParam[knob::kCSGateRelease]   = 44;
    // Gate Hold not present on 4K E
    b.buttonParam[button::kHfBell]      = 32;
    b.buttonParam[button::kEqType]      = 19;  // EQ Colour
    b.buttonParam[button::kEqIn]        = 18;
    b.buttonParam[button::kLfBell]      = 23;
    b.buttonParam[button::kFastAttComp] = 39;
    // Comp Peak not present on 4K E
    b.buttonParam[button::kDynIn]       = 33;
    b.buttonParam[button::kExpand]      = 46;
    b.buttonParam[button::kFastAttGate] = 45;
    b.buttonParam[button::kScListen]    = 47;
    // CS IN → plug-in Bypass at vst3 idx 0 (same across CS variants).
    b.bypassParam = 0;
    // 4K E has opposite VST3-value semantic from CS 2 for several knobs
    // (LP, HMF/LMF Q, Comp/Gate Threshold). User confirmed 2026-04-24:
    // applying the CS-2 inversions here makes the pot motion flip.
    return b;
}

// ---- 4K G ------------------------------------------------------------------
// Full-featured G-series strip. No Gate Hold, no Comp Peak.
PluginBindings make4kGBindings()
{
    auto b = makeEmpty("4K G", "4K G");
    b.knobParam[knob::kCSLowPass]       = 21;
    b.knobParam[knob::kCSHighPass]      = 20;
    b.knobParam[knob::kCSHfGain]        = 36;
    b.knobParam[knob::kCSHfFreq]        = 35;
    b.knobParam[knob::kCSHmfGain]       = 33;
    b.knobParam[knob::kCSHmfFreq]       = 32;
    b.knobParam[knob::kCSHmfQ]          = 34;
    b.knobParam[knob::kCSLmfGain]       = 29;
    b.knobParam[knob::kCSLmfFreq]       = 28;
    b.knobParam[knob::kCSLmfQ]          = 30;
    b.knobParam[knob::kCSLfFreq]        = 24;
    b.knobParam[knob::kCSLfGain]        = 25;
    b.knobParam[knob::kCSInputTrim]     =  6;
    b.knobParam[knob::kCSFaderLevel]    = 12;
    b.knobParam[knob::kCSCompThreshold] = 40;
    b.knobParam[knob::kCSCompRatio]     = 39;
    b.knobParam[knob::kCSCompRelease]   = 41;
    b.knobParam[knob::kCSGateThreshold] = 47;
    b.knobParam[knob::kCSGateRange]     = 46;
    b.knobParam[knob::kCSGateRelease]   = 48;
    b.buttonParam[button::kHfBell]      = 37;
    b.buttonParam[button::kEqType]      = 23;  // EQ Colour
    b.buttonParam[button::kEqIn]        = 22;
    b.buttonParam[button::kLfBell]      = 26;
    b.buttonParam[button::kFastAttComp] = 43;
    b.buttonParam[button::kDynIn]       = 38;
    b.buttonParam[button::kExpand]      = 50;
    b.buttonParam[button::kFastAttGate] = 49;
    b.buttonParam[button::kScListen]    = 51;
    b.bypassParam = 0;  // CS IN → plug-in Bypass at vst3 idx 0
    // 4K G skips CS-2 inversions (same VST3 semantic as 4K E — see above).
    return b;
}

// ---- 4K B ------------------------------------------------------------------
// Simpler B-series: no EQ Type, no Fast-Attack (Comp/Gate), no Gate Hold,
// no Comp Peak.
PluginBindings make4kBBindings()
{
    auto b = makeEmpty("4K B", "4K B");
    b.knobParam[knob::kCSLowPass]       = 11;
    b.knobParam[knob::kCSHighPass]      = 10;
    b.knobParam[knob::kCSHfGain]        = 26;
    b.knobParam[knob::kCSHfFreq]        = 25;
    b.knobParam[knob::kCSHmfGain]       = 23;
    b.knobParam[knob::kCSHmfFreq]       = 22;
    b.knobParam[knob::kCSHmfQ]          = 24;
    b.knobParam[knob::kCSLmfGain]       = 20;
    b.knobParam[knob::kCSLmfFreq]       = 19;
    b.knobParam[knob::kCSLmfQ]          = 21;
    b.knobParam[knob::kCSLfFreq]        = 16;
    b.knobParam[knob::kCSLfGain]        = 17;
    b.knobParam[knob::kCSInputTrim]     =  2;
    b.knobParam[knob::kCSFaderLevel]    =  6;
    b.knobParam[knob::kCSCompThreshold] = 31;
    b.knobParam[knob::kCSCompRatio]     = 30;
    b.knobParam[knob::kCSCompRelease]   = 32;
    b.knobParam[knob::kCSGateThreshold] = 35;
    b.knobParam[knob::kCSGateRange]     = 34;
    b.knobParam[knob::kCSGateRelease]   = 36;
    b.buttonParam[button::kHfBell]      = 27;
    // No EQ Type on 4K B
    b.buttonParam[button::kEqIn]        = 14;
    b.buttonParam[button::kLfBell]      = 18;
    b.buttonParam[button::kDynIn]       = 28;
    b.buttonParam[button::kExpand]      = 37;
    b.buttonParam[button::kScListen]    = 41;
    b.bypassParam = 0;  // CS IN → plug-in Bypass at vst3 idx 0
    // 4K B skips CS-2 inversions (same VST3 semantic as 4K E — see above).
    return b;
}

// Registry. Order: most-specific substring first (same convention as the
// UF8 PluginMap). BC 2's match string wouldn't collide with any of the
// Channel Strip variants so ordering there isn't critical, but the 4K
// series all contain "SSL" so putting the longer "4K G" before "4K E"
// before "4K B" keeps lookupBindingsByName unambiguous.
const PluginBindings& csReg()   { static auto v = makeChannelStrip2Bindings();   return v; }
const PluginBindings& bcReg()   { static auto v = makeBusComp2Bindings();        return v; }
const PluginBindings& linkReg() { static auto v = makeSsl360LinkBindings();      return v; }
const PluginBindings& linkBcReg(){static auto v = makeSsl360LinkBcBindings();    return v; }
const PluginBindings& e4Reg()   { static auto v = make4kEBindings();             return v; }
const PluginBindings& g4Reg()   { static auto v = make4kGBindings();             return v; }
const PluginBindings& b4Reg()   { static auto v = make4kBBindings();             return v; }

const PluginBindings* kChannelStripCandidates[] = {
    &csReg(), &g4Reg(), &e4Reg(), &b4Reg(), &linkReg(),
};

// BC variants — order matters for substring matching: "SSL 360 Link Bus
// Compressor" must come before "Bus Compressor 2" to win on its own
// name. (And linkBcReg's match string is more specific than bcReg's.)
const PluginBindings* kBusCompCandidates[] = {
    &linkBcReg(), &bcReg(),
};

// ---- User-map → UC1 binding bridge --------------------------------------
//
// When the FX-Learn editor produces a UserPluginMap (per-linkIdx
// vst3Param + inverted), UC1 needs the same data in its per-knob-id
// PluginBindings layout so the BC encoder / dispatch path treat the
// user's plugin like any built-in. We synthesize one PluginBindings per
// user map, cached by match string, invalidated via user_plugins's
// generation counter.

constexpr uint8_t kNoUc1 = 0xFF;

// Maps an SSL 360 Link CS slot index to the UC1 control(s) that drive it.
// linkIdx not in this table → not reachable from UC1 (fader / pan /
// width / output / extras / QA1..6 / SAT.* / GRP). Knob and button can
// both be filled when an SSL slot is exposed by both (none today).
struct LinkToUc1 {
    int     linkIdx;
    uint8_t knobId;
    uint8_t buttonId;
};

constexpr LinkToUc1 kCsLinkToUc1[] = {
    {  4, knob::kCSInputTrim,    kNoUc1               },
    {  5, kNoUc1,                button::kPolarity    },
    {  6, knob::kCSLowPass,      kNoUc1               },
    {  7, knob::kCSHighPass,     kNoUc1               },
    {  8, kNoUc1,                button::kHfBell      },
    {  9, knob::kCSHfGain,       kNoUc1               },
    { 10, knob::kCSHfFreq,       kNoUc1               },
    { 11, knob::kCSHmfGain,      kNoUc1               },
    { 12, knob::kCSHmfFreq,      kNoUc1               },
    { 13, knob::kCSHmfQ,         kNoUc1               },
    { 14, kNoUc1,                button::kEqType      },
    { 15, kNoUc1,                button::kEqIn        },
    { 16, knob::kCSLmfGain,      kNoUc1               },
    { 17, knob::kCSLmfFreq,      kNoUc1               },
    { 18, knob::kCSLmfQ,         kNoUc1               },
    { 19, knob::kCSLfFreq,       kNoUc1               },
    { 20, knob::kCSLfGain,       kNoUc1               },
    { 21, kNoUc1,                button::kLfBell      },
    { 22, kNoUc1,                button::kDynIn       },
    { 24, kNoUc1,                button::kFastAttComp },
    { 25, kNoUc1,                button::kPeak        },
    { 26, knob::kCSCompRatio,    kNoUc1               },
    { 27, knob::kCSCompThreshold,kNoUc1               },
    { 28, knob::kCSCompRelease,  kNoUc1               },
    { 29, knob::kCSGateRange,    kNoUc1               },
    { 30, knob::kCSGateThreshold,kNoUc1               },
    { 31, knob::kCSGateRelease,  kNoUc1               },
    { 32, knob::kCSGateHold,     kNoUc1               },
    { 33, kNoUc1,                button::kExpand      },
    { 34, kNoUc1,                button::kFastAttGate },
    { 36, kNoUc1,                button::kScListen    },
};

// Maps an SSL 360 Link BC slot index to the UC1 control. BC has only the
// top V-Pots + the IN button.
constexpr LinkToUc1 kBcLinkToUc1[] = {
    {  0, kNoUc1,                button::kBusCompIn   }, // bypass / IN
    {  1, knob::kBCThreshold,    kNoUc1               },
    {  2, knob::kBCMakeup,       kNoUc1               },
    {  3, knob::kBCAttack,       kNoUc1               },
    {  4, knob::kBCRelease,      kNoUc1               },
    {  5, knob::kBCRatio,        kNoUc1               },
    {  6, knob::kBCScHpf,        kNoUc1               },
    {  7, knob::kBCMix,          kNoUc1               },
};

// Fill a PluginBindings from a UserPluginMap. Caller owns the storage of
// `out` — match/shortName must already point to stable strings (we don't
// touch them here).
void synthesizeUserBinding_(const uf8::UserPluginMap& um, PluginBindings& out)
{
    for (auto& v : out.knobParam)   v = kParamNone;
    for (auto& v : out.buttonParam) v = kParamNone;
    for (auto& v : out.inverted)    v = false;
    out.bypassParam = kParamNone;

    const LinkToUc1* table     = nullptr;
    int              tableSize = 0;
    if (um.domain == uf8::Domain::BusComp) {
        table     = kBcLinkToUc1;
        tableSize = sizeof(kBcLinkToUc1) / sizeof(kBcLinkToUc1[0]);
    } else if (um.domain == uf8::Domain::ChannelStrip) {
        table     = kCsLinkToUc1;
        tableSize = sizeof(kCsLinkToUc1) / sizeof(kCsLinkToUc1[0]);
    } else {
        return;
    }

    for (const auto& slot : um.slots) {
        if (slot.vst3Param < 0) continue;
        // SSL Link slot 0 = plug-in bypass on both CS and BC. Persist
        // the bound vst3Param into bypassParam so the IN button toggles
        // the plug-in's own bypass param rather than REAPER's
        // TrackFX_Enabled (matches what built-in BC 2 does).
        if (slot.linkIdx == 0) {
            out.bypassParam = slot.vst3Param;
        }
        for (int i = 0; i < tableSize; ++i) {
            if (table[i].linkIdx != slot.linkIdx) continue;
            if (table[i].knobId != kNoUc1) {
                out.knobParam[table[i].knobId] = slot.vst3Param;
                out.inverted[table[i].knobId]  = slot.inverted;
            }
            if (table[i].buttonId != kNoUc1) {
                out.buttonParam[table[i].buttonId] = slot.vst3Param;
            }
            break;
        }
    }
}

// Per-user-map cache entry. `match` and `shortName` strings own their
// storage so PluginBindings::match / shortName can point into them.
struct UserBindingEntry {
    std::string    matchOwned;
    std::string    shortNameOwned;
    PluginBindings bindings{};
    uf8::Domain    domain = uf8::Domain::None;
};

std::mutex                                       g_userCacheMutex;
std::vector<std::unique_ptr<UserBindingEntry>>   g_userCache;
int                                              g_userCacheGeneration = -1;

// Rebuild the user-binding cache from current user_plugins state. Caller
// must hold g_userCacheMutex.
void rebuildUserCache_locked_()
{
    g_userCache.clear();
    const auto& cat = uf8::user_plugins::get();
    for (const auto& um : cat.maps) {
        if (um.domain != uf8::Domain::BusComp &&
            um.domain != uf8::Domain::ChannelStrip)
        {
            continue;
        }
        if (um.match.empty()) continue;
        auto e = std::make_unique<UserBindingEntry>();
        e->matchOwned     = um.match;
        e->shortNameOwned = um.displayShort.empty()
            ? std::string("USR")
            : um.displayShort;
        e->domain         = um.domain;
        e->bindings.match     = e->matchOwned.c_str();
        e->bindings.shortName = e->shortNameOwned.c_str();
        synthesizeUserBinding_(um, e->bindings);
        g_userCache.push_back(std::move(e));
    }
    g_userCacheGeneration =
        uf8::user_plugins::generation();
}

// Refresh cache if user_plugins changed since last build. Returns with
// g_userCacheMutex held; caller is responsible for unlocking before any
// further user_plugins:: read to avoid lock-order issues.
void refreshUserCache_()
{
    if (uf8::user_plugins::generation() == g_userCacheGeneration) return;
    std::lock_guard<std::mutex> lk(g_userCacheMutex);
    if (uf8::user_plugins::generation() == g_userCacheGeneration) return;
    rebuildUserCache_locked_();
}

// True when this binding's plug-in is a Bus Compressor variant. The
// older code identified BC by `shortName == "BC 2"`, which broke as
// soon as the SSL 360 Link wrapper came in with its own shortName.
bool isBusCompBinding(const PluginBindings* b)
{
    if (!b) return false;
    for (const auto* c : kBusCompCandidates) if (c == b) return true;
    // User-synthesized BC bindings: walk the cache and check ownership.
    std::lock_guard<std::mutex> lk(g_userCacheMutex);
    for (const auto& e : g_userCache) {
        if (&e->bindings == b) return e->domain == uf8::Domain::BusComp;
    }
    return false;
}

} // namespace

const PluginBindings* lookupBindingsByName(std::string_view fxName)
{
    // Built-ins win first (mirrors uf8::lookupPluginMapByName ordering)
    // — UserPluginCatalog::save guarantees no user map can shadow a
    // built-in match string anyway.
    for (const auto* b : kBusCompCandidates) {
        if (fxName.find(b->match) != std::string_view::npos) return b;
    }
    for (const auto* b : kChannelStripCandidates) {
        if (fxName.find(b->match) != std::string_view::npos) return b;
    }

    // User catalog fallback — synthesize a UC1 PluginBindings on first
    // touch, cache it, return the stable pointer.
    refreshUserCache_();
    std::lock_guard<std::mutex> lk(g_userCacheMutex);
    for (const auto& e : g_userCache) {
        if (fxName.find(e->matchOwned) != std::string_view::npos) {
            return &e->bindings;
        }
    }
    return nullptr;
}

ControlDomain classifyKnob(uint8_t knobId)
{
    // Top V-Pots live in 0x0C..0x16. By physical layout on the SSL UC1:
    //   0x0C  Input Gain  → sits above the Input VU strip (CS-side)
    //   0x0E..0x14  BC pots (Ratio/ScHpf/Atk/Rel/Thresh/Makeup/Mix)
    //   0x16  Output Gain → sits above the Output VU strip (CS-side)
    // The two end knobs (Input/Output Gain) are wired by default to
    // Channel Strip Input Trim + Fader Level — even when a Bus Comp
    // plug-in is on the track. BC2 has no equivalent params for them.
    if (knobId == knob::kCSInputTrim || knobId == knob::kCSFaderLevel)
        return ControlDomain::ChannelStrip;
    if (knobId >= 0x0E && knobId <= 0x14) return ControlDomain::BusComp;
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

    // Crash 2026-04-29 (SetSurfaceSolo during LoadProjectFromContext):
    // REAPER frees old project's MediaTrack* before notifying surfaces,
    // so a cached focusedTrack pointer becomes dangling. ValidatePtr2
    // returns false for freed tracks; bail out instead of crashing in
    // TrackFX_GetCount.
    if (!ValidatePtr2(nullptr, tr, "MediaTrack*")) return result;

    const int n = TrackFX_GetCount(tr);
    char buf[256];
    for (int i = 0; i < n; ++i) {
        if (!TrackFX_GetFXName(tr, i, buf, sizeof(buf))) continue;
        std::string_view name{buf};
        const PluginBindings* b = lookupBindingsByName(name);
        if (!b) continue;

        // BC variant identification — kBusCompCandidates lists every
        // binding that should populate `result.busCompMap`. The older
        // shortName == "BC 2" check broke on the SSL 360 Link Bus
        // Compressor wrapper which carries shortName "L-BC".
        const bool isBusComp = isBusCompBinding(b);

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
