#include "PluginMap.h"

#include <algorithm>
#include <array>
#include <string>

#include "reaper_plugin_functions.h"

namespace uf8 {
namespace {

// ----- SSL 360 Link virtual channel-strip layout -----------------------------
//
// linkIdx / id / name / legend columns come from the "SSL 360 Link" VST3
// wrapper plugin, whose own VST3 param indices are the authoritative
// virtual-strip reference (and match every .factory XML in
// /Library/Application Support/SSL/SSLPlugins/SSL360Link/).
//
// Per-plugin tables below differ only in `vst3Param` / `inverted` — the
// metadata is duplicated rather than indirected, because the tables are
// tiny and the readability is worth it.
//
// Ordering of slots in each table is also the *page order* for that
// plugin: strip[s] renders slots[pageIdx] from the plugin on track[s].
// Pick an order that makes sense hands-on: essentials first, then EQ,
// then dynamics.

// Native SSL Channel Strip 2 (VST3: "SSL Native Channel Strip 2 (SSL)")
constexpr LinkSlot kCs2Slots[] = {
    { 35, "LinkableFaderLevel", "Fader",            "FADER", 38,  false },
    {  4, "InputTrim",          "Input Trim",       "IN",     2,  false },
    {  7, "HighPassFreq",       "High Pass Filter", "HPF",    5,  false },
    {  6, "LowPassFreq",        "Low Pass Filter",  "LPF",    4,  false },
    { 15, "EqIn",               "EQ In",            "EQ",     6,  false },
    { 22, "DynamicsIn",         "Dynamics In",      "DYN",   20,  false },
    { 37, "OutputTrim",         "Output Trim",      "OUT",   39,  false },
    { 36, "Listen",             "S/C Listen",       "LST",   37,  false },
    // EQ HF
    { 10, "HighEqFreq",         "HF Freq",          "FREQ",  18,  false },
    {  9, "HighEqGain",         "HF Gain",          "GAIN",  19,  false },
    {  8, "HighEqBell",         "HF Type",          "BELL",  17,  false },
    // EQ HMF
    { 12, "HighMidEqFreq",      "HMF Freq",         "FREQ",  14,  false },
    { 11, "HighMidEqGain",      "HMF Gain",         "GAIN",  15,  false },
    { 13, "HighMidEqQ",         "HMF Q",            "Q",     16,  false },
    // EQ Type
    { 14, "EqType",             "EQ Type",          "TYPE",   7,  false },
    // EQ LMF
    { 17, "LowMidEqFreq",       "LMF Freq",         "FREQ",  11,  false },
    { 16, "LowMidEqGain",       "LMF Gain",         "GAIN",  12,  false },
    { 18, "LowMidEqQ",          "LMF Q",            "Q",     13,  false },
    // EQ LF
    { 19, "LowEqFreq",          "LF Freq",          "FREQ",   9,  false },
    { 20, "LowEqGain",          "LF Gain",          "GAIN",  10,  false },
    { 21, "LowEqBell",          "LF Type",          "BELL",   8,  false },
    // Compressor
    { 27, "CompThreshold",      "Comp Threshold",   "THR",   24,  false },
    { 26, "CompRatio",          "Comp Ratio",       "RATIO", 23,  false },
    { 28, "CompRelease",        "Comp Release",     "REL",   25,  false },
    { 24, "CompFastAttack",     "Comp F.Attack",    "F.ATK", 22,  false },
    { 25, "CompPeak",           "Comp Peak",        "PEAK",  21,  false },
    { 23, "CompMix",            "Comp Mix",         "MIX",   26,  false },
    // Gate
    { 30, "GateThreshold",      "Gate Threshold",   "THR",   30,  false },
    { 29, "GateRange",          "Gate Range",       "RNG",   31,  false },
    { 31, "GateRelease",        "Gate Release",     "REL",   32,  false },
    { 32, "GateHold",           "Gate Hold",        "HOLD",  29,  false },
    { 34, "GateAttack",         "Gate F.Attack",    "F.ATK", 28,  false },
    { 33, "GateExpander",       "Gate/Expander",    "G/E",   27,  false },
};

// 4K E — shares all CS-family slots except CompPeak; adds EQ Colour in
// place of EQ Type. VST3 param indices from docs/ssl-native-params.
constexpr LinkSlot k4kESlots[] = {
    { 35, "LinkableFaderLevel", "Fader",            "FADER",  6,  false },
    {  4, "InputTrim",          "Input Trim",       "IN",     2,  false },
    {  7, "HighPassFreq",       "High Pass Filter", "HPF",   14,  false },
    {  6, "LowPassFreq",        "Low Pass Filter",  "LPF",   15,  false },
    { 15, "EqIn",               "EQ In",            "EQ",    18,  false },
    { 22, "DynamicsIn",         "Dynamics In",      "DYN",   33,  false },
    { 37, "OutputTrim",         "Output Trim",      "OUT",    8,  false },
    { 36, "Listen",             "S/C Listen",       "LST",   47,  false },
    { 10, "HighEqFreq",         "HF Freq",          "FREQ",  30,  false },
    {  9, "HighEqGain",         "HF Gain",          "GAIN",  31,  false },
    {  8, "HighEqBell",         "HF Type",          "BELL",  32,  false },
    { 12, "HighMidEqFreq",      "HMF Freq",         "FREQ",  27,  false },
    { 11, "HighMidEqGain",      "HMF Gain",         "GAIN",  28,  false },
    { 13, "HighMidEqQ",         "HMF Q",            "Q",     29,  false },
    { 14, "EqType",             "EQ Colour",        "COL",   19,  false },
    { 17, "LowMidEqFreq",       "LMF Freq",         "FREQ",  24,  false },
    { 16, "LowMidEqGain",       "LMF Gain",         "GAIN",  25,  false },
    { 18, "LowMidEqQ",          "LMF Q",            "Q",     26,  false },
    { 19, "LowEqFreq",          "LF Freq",          "FREQ",  21,  false },
    { 20, "LowEqGain",          "LF Gain",          "GAIN",  22,  false },
    { 21, "LowEqBell",          "LF Type",          "BELL",  23,  false },
    { 27, "CompThreshold",      "Comp Threshold",   "THR",   36,  false },
    { 26, "CompRatio",          "Comp Ratio",       "RATIO", 35,  false },
    { 28, "CompRelease",        "Comp Release",     "REL",   37,  false },
    { 24, "CompFastAttack",     "Comp F.Attack",    "F.ATK", 39,  false },
    { 23, "CompMix",            "Comp Mix",         "MIX",   38,  false },
    { 30, "GateThreshold",      "Gate Threshold",   "THR",   43,  false },
    { 29, "GateRange",          "Gate Range",       "RNG",   42,  false },
    { 31, "GateRelease",        "Gate Release",     "REL",   44,  false },
    { 34, "GateAttack",         "Gate F.Attack",    "F.ATK", 45,  false },
    { 33, "GateExpander",       "Gate/Expander",    "G/E",   46,  false },
};

// 4K B — no EQ Type toggle, no Fast-Attack / Gate-Hold / Gate-Attack /
// Comp-Peak (simpler 4000-series B-feature set).
constexpr LinkSlot k4kBSlots[] = {
    { 35, "LinkableFaderLevel", "Fader",            "FADER",  6,  false },
    {  4, "InputTrim",          "Input Trim",       "IN",     2,  false },
    {  7, "HighPassFreq",       "High Pass Filter", "HPF",   10,  false },
    {  6, "LowPassFreq",        "Low Pass Filter",  "LPF",   11,  false },
    { 15, "EqIn",               "EQ In",            "EQ",    14,  false },
    { 22, "DynamicsIn",         "Dynamics In",      "DYN",   28,  false },
    { 37, "OutputTrim",         "Output Trim",      "OUT",    7,  false },
    { 36, "Listen",             "S/C Listen",       "LST",   41,  false },
    { 10, "HighEqFreq",         "HF Freq",          "FREQ",  25,  false },
    {  9, "HighEqGain",         "HF Gain",          "GAIN",  26,  false },
    {  8, "HighEqBell",         "HF Type",          "BELL",  27,  false },
    { 12, "HighMidEqFreq",      "HMF Freq",         "FREQ",  22,  false },
    { 11, "HighMidEqGain",      "HMF Gain",         "GAIN",  23,  false },
    { 13, "HighMidEqQ",         "HMF Q",            "Q",     24,  false },
    { 17, "LowMidEqFreq",       "LMF Freq",         "FREQ",  19,  false },
    { 16, "LowMidEqGain",       "LMF Gain",         "GAIN",  20,  false },
    { 18, "LowMidEqQ",          "LMF Q",            "Q",     21,  false },
    { 19, "LowEqFreq",          "LF Freq",          "FREQ",  16,  false },
    { 20, "LowEqGain",          "LF Gain",          "GAIN",  17,  false },
    { 21, "LowEqBell",          "LF Type",          "BELL",  18,  false },
    { 27, "CompThreshold",      "Comp Threshold",   "THR",   31,  false },
    { 26, "CompRatio",          "Comp Ratio",       "RATIO", 30,  false },
    { 28, "CompRelease",        "Comp Release",     "REL",   32,  false },
    { 23, "CompMix",            "Comp Mix",         "MIX",   33,  false },
    { 30, "GateThreshold",      "Gate Threshold",   "THR",   35,  false },
    { 29, "GateRange",          "Gate Range",       "RNG",   34,  false },
    { 31, "GateRelease",        "Gate Release",     "REL",   36,  false },
    { 33, "GateExpander",       "Gate/Expander",    "G/E",   37,  false },
};

// 4K G — full-featured G-series strip.
constexpr LinkSlot k4kGSlots[] = {
    { 35, "LinkableFaderLevel", "Fader",            "FADER", 12,  false },
    {  4, "InputTrim",          "Input Trim",       "IN",     6,  false },
    {  7, "HighPassFreq",       "High Pass Filter", "HPF",   20,  false },
    {  6, "LowPassFreq",        "Low Pass Filter",  "LPF",   21,  false },
    { 15, "EqIn",               "EQ In",            "EQ",    22,  false },
    { 22, "DynamicsIn",         "Dynamics In",      "DYN",   38,  false },
    { 37, "OutputTrim",         "Output Trim",      "OUT",   14,  false },
    { 36, "Listen",             "S/C Listen",       "LST",   51,  false },
    { 10, "HighEqFreq",         "HF Freq",          "FREQ",  35,  false },
    {  9, "HighEqGain",         "HF Gain",          "GAIN",  36,  false },
    {  8, "HighEqBell",         "HF Type",          "BELL",  37,  false },
    { 12, "HighMidEqFreq",      "HMF Freq",         "FREQ",  32,  false },
    { 11, "HighMidEqGain",      "HMF Gain",         "GAIN",  33,  false },
    { 13, "HighMidEqQ",         "HMF Q",            "Q",     34,  false },
    { 14, "EqType",             "EQ Colour",        "COL",   23,  false },
    { 17, "LowMidEqFreq",       "LMF Freq",         "FREQ",  28,  false },
    { 16, "LowMidEqGain",       "LMF Gain",         "GAIN",  29,  false },
    { 18, "LowMidEqQ",          "LMF Q",            "Q",     30,  false },
    { 19, "LowEqFreq",          "LF Freq",          "FREQ",  24,  false },
    { 20, "LowEqGain",          "LF Gain",          "GAIN",  25,  false },
    { 21, "LowEqBell",          "LF Type",          "BELL",  26,  false },
    { 27, "CompThreshold",      "Comp Threshold",   "THR",   40,  false },
    { 26, "CompRatio",          "Comp Ratio",       "RATIO", 39,  false },
    { 28, "CompRelease",        "Comp Release",     "REL",   41,  false },
    { 24, "CompFastAttack",     "Comp F.Attack",    "F.ATK", 43,  false },
    { 23, "CompMix",            "Comp Mix",         "MIX",   42,  false },
    { 30, "GateThreshold",      "Gate Threshold",   "THR",   47,  false },
    { 29, "GateRange",          "Gate Range",       "RNG",   46,  false },
    { 31, "GateRelease",        "Gate Release",     "REL",   48,  false },
    { 34, "GateAttack",         "Gate F.Attack",    "F.ATK", 49,  false },
    { 33, "GateExpander",       "Gate/Expander",    "G/E",   50,  false },
};

// Bus Compressor 2 — virtual strip has 7 slots (idx 1..7).
constexpr LinkSlot kBusComp2Slots[] = {
    { 1, "Threshold",    "Threshold", "THR",   2, false },
    { 2, "MakeupGain",   "Makeup",    "MAKE",  3, false },
    { 5, "Ratio",        "Ratio",     "RATIO", 6, false },
    { 3, "Attack",       "Attack",    "ATK",   4, false },
    { 4, "Release",      "Release",   "REL",   5, false },
    { 6, "SidechainHPF", "S/C HPF",   "HPF",   7, false },
    { 7, "DryWetMix",    "Mix",       "MIX",   8, false },
};

// ----- registry ---------------------------------------------------------------
//
// Order matters: lookupPluginMapByName does first-hit substring matching,
// so more specific strings (e.g. "4K G") must come before substrings that
// would also match a broader name. Both TrackFX_GetFXName forms observed
// in captures — "VST3: SSL 4K G (SSL)" and "VST3: 4K E" — are unambiguous
// against these `match` strings.

constexpr PluginMap kMaps[] = {
    { "Channel Strip 2",    "CS 2",  Domain::ChannelStrip, kCs2Slots       },
    { "4K G",               "4K G",  Domain::ChannelStrip, k4kGSlots       },
    { "4K E",               "4K E",  Domain::ChannelStrip, k4kESlots       },
    { "4K B",               "4K B",  Domain::ChannelStrip, k4kBSlots       },
    { "Bus Compressor 2",   "BC 2",  Domain::BusComp,      kBusComp2Slots  },
};

} // namespace

const PluginMap* lookupPluginMapByName(std::string_view fxName)
{
    for (const auto& m : kMaps) {
        if (fxName.find(m.match) != std::string_view::npos) return &m;
    }
    return nullptr;
}

std::span<const PluginMap> allPluginMaps()
{
    return { kMaps, sizeof(kMaps) / sizeof(kMaps[0]) };
}

int slotIdxForVst3Param(const PluginMap& map, int vst3Param)
{
    for (size_t i = 0; i < map.slots.size(); ++i) {
        if (map.slots[i].vst3Param == vst3Param) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

PluginMatch lookupPluginOnTrack(void* trackOpaque)
{
    auto* tr = static_cast<MediaTrack*>(trackOpaque);
    if (!tr) return { nullptr, -1 };
    const int n = TrackFX_GetCount(tr);
    char buf[512];
    for (int fx = 0; fx < n; ++fx) {
        buf[0] = 0;
        TrackFX_GetFXName(tr, fx, buf, sizeof(buf));
        if (buf[0] == 0) continue;
        if (const PluginMap* m = lookupPluginMapByName(buf)) {
            return { m, fx };
        }
    }
    return { nullptr, -1 };
}

PluginMatch lookupPluginOnTrack(void* trackOpaque, Domain domain)
{
    if (domain == Domain::None) return { nullptr, -1 };
    auto* tr = static_cast<MediaTrack*>(trackOpaque);
    if (!tr) return { nullptr, -1 };
    const int n = TrackFX_GetCount(tr);
    char buf[512];
    for (int fx = 0; fx < n; ++fx) {
        buf[0] = 0;
        TrackFX_GetFXName(tr, fx, buf, sizeof(buf));
        if (buf[0] == 0) continue;
        const PluginMap* m = lookupPluginMapByName(buf);
        if (m && m->domain == domain) return { m, fx };
    }
    return { nullptr, -1 };
}

} // namespace uf8
