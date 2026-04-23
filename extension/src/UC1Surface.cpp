#include "UC1Surface.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "reaper_plugin_functions.h"

namespace uc1 {

namespace {

// Label for the zone-0x03/0x05 readout based on which knob was touched.
// Kept short here — the surface clips/pads to 22 chars before sending.
// Matches the labels SSL 360° pushes in our captures (uc1_04 etc.).
const char* labelForKnob(uint8_t knobId, bool busCompContext)
{
    if (busCompContext) {
        switch (knobId) {
            case knob::kBCThreshold: return "Threshold";
            case knob::kBCMakeup:    return "Makeup";
            case knob::kBCAttack:    return "Attack";
            case knob::kBCRelease:   return "Release";
            case knob::kBCRatio:     return "Ratio";
            case knob::kBCMix:       return "Mix";
            case knob::kBCScHpf:     return "S/C HPF";
        }
    }
    switch (knobId) {
        // CS dedicated pots
        case knob::kCSLowPass:        return "Low Pass";
        case knob::kCSHighPass:       return "High Pass";
        case knob::kCSHfGain:         return "HF Gain";
        case knob::kCSHfFreq:         return "HF Frequency";
        case knob::kCSHmfGain:        return "HMF Gain";
        case knob::kCSHmfFreq:        return "HMF Frequency";
        case knob::kCSHmfQ:           return "HMF Q";
        case knob::kCSLmfGain:        return "LMF Gain";
        case knob::kCSLmfFreq:        return "LMF Frequency";
        case knob::kCSLmfQ:           return "LMF Q";
        case knob::kCSLfFreq:         return "LF Frequency";
        case knob::kCSLfGain:         return "LF Gain";
        // CS right-side
        case knob::kCSGateRelease:    return "Release";    // (Gate)
        case knob::kCSGateHold:       return "Hold";
        case knob::kCSGateThreshold:  return "Threshold";  // (Gate)
        case knob::kCSGateRange:      return "Range";
        case knob::kCSCompRelease:    return "Release";    // (Dyn)
        case knob::kCSCompThreshold:  return "Threshold";  // (Dyn)
        case knob::kCSCompRatio:      return "Ratio";      // (Dyn)
        // Repurposed V-Pots
        case knob::kCSInputTrim:      return "In Trim";
        case knob::kCSFaderLevel:     return "Fader Level";
    }
    return "";
}

// Build a zone-0x03/0x05 readout matching SSL 360°'s captured format:
//   [label][padding to 16 chars][value]
//
// Total length = 16 + value.size() (22 for 6-char values like "12.1dB",
// 23 for 7-char values like "-10.0dB" or "102.5Hz"). UC1's value-zone
// LCD slots start at position 16; right-justifying into a fixed 22-char
// field shifted 7-char values one left of that anchor, causing the
// split "1    02.5Hz" / "-        10.0dB" rendering on hardware.
std::string formatReadout(std::string_view label, std::string_view value)
{
    constexpr size_t kLabelPad = 16;
    std::string out;
    out.reserve(kLabelPad + value.size());

    // std::min is macro-shadowed by WDL/swell — wrap to suppress expansion.
    const size_t lmax = (std::min)(label.size(), kLabelPad);
    out.append(label.data(), lmax);
    if (lmax < kLabelPad) {
        out.append(kLabelPad - lmax, ' ');
    }
    out.append(value.data(), value.size());
    return out;
}

// Which LED cell belongs to this button? Returns {0,0} if there's no
// dedicated LED (Fine, Polarity without display, etc.).
led::Cell cellForButton(uint8_t buttonId)
{
    switch (buttonId) {
        case button::kHfBell:      return led::kHfBell;
        case button::kEqType:      return led::kEqType;
        case button::kEqIn:        return led::kEqIn;
        case button::kLfBell:      return led::kLfBell;
        case button::kBusCompIn:   return led::kBusCompIn;
        case button::kFastAttComp: return led::kFastAttComp;
        case button::kPeak:        return led::kPeak;
        case button::kDynIn:       return led::kDynIn;
        case button::kExpand:      return led::kExpand;
        case button::kFastAttGate: return led::kFastAttGate;
        case button::kPolarity:    return led::kPolarity;
        case button::kScListen:    return led::kScListen;
        case button::kSolo:        return led::kSolo;
        case button::kSoloClear:   return led::kSoloClear;
        case button::kCut:         return led::kCut;
        case button::kChannelIn:   return led::kChannelIn;
        case button::kFine:        return led::kFine;
    }
    return {0, 0};
}

} // namespace

UC1Surface::UC1Surface() = default;

void UC1Surface::attach(UC1Device& device)
{
    device_ = &device;
    device_->setKnobHandler([this](const KnobEvent& ev) {
        std::lock_guard<std::mutex> lk(queueMu_);
        knobQueue_.push_back(ev);
    });
    device_->setButtonHandler([this](const ButtonEvent& ev) {
        std::lock_guard<std::mutex> lk(queueMu_);
        buttonQueue_.push_back(ev);
    });
}

void UC1Surface::setFocusedTrack(void* track)
{
    if (focusedTrack_ == track) return;
    focusedTrack_ = track;
    refresh();
}

int UC1Surface::poll()
{
    std::deque<KnobEvent>   knobs;
    std::deque<ButtonEvent> buttons;
    {
        std::lock_guard<std::mutex> lk(queueMu_);
        knobs.swap(knobQueue_);
        buttons.swap(buttonQueue_);
    }

    int handled = 0;
    for (const auto& e : knobs)   { handleKnob_(e);   ++handled; }
    for (const auto& e : buttons) { handleButton_(e); ++handled; }
    return handled;
}

double UC1Surface::clickToDelta_(int8_t delta) const
{
    // Each encoder click = ~1/64 of a full param sweep. Fine mode = 1/4
    // of that (= 1/256 per click, effectively 256 clicks to traverse
    // the full normalized range). 1/64 at default roughly matches SSL
    // 360°'s perceived feel on the Bus Comp Threshold (40 dB range
    // covered in ~200 encoder clicks = 0.2 dB/click vs. our 0.625 dB
    // per click — slightly snappier but responsive).
    constexpr double kStepPerClick = 1.0 / 64.0;
    double d = delta * kStepPerClick;
    if (fineMode_.load(std::memory_order_relaxed)) d *= 0.25;
    return d;
}

void UC1Surface::handleKnob_(const KnobEvent& ev)
{
    // Diag-first: log every knob event before any suppression, so
    // unmapped IDs (Attack TBD, any knob we haven't attributed) show
    // up in the console. Per-ID budget of 3 keeps volume sane.
    static int kPerIdRemaining[0x20] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                         3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
    const bool logThis = (ev.id < 0x20 && kPerIdRemaining[ev.id] > 0);
    if (logThis) --kPerIdRemaining[ev.id];

    if (!focusedTrack_) {
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 knob 0x%02x delta=%d  (no focused track)\n",
                ev.id, (int)ev.delta);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsSuppressed;
        return;
    }

    auto bindings = lookupBindingsOnTrack(focusedTrack_);
    if (!bindings.busCompMap && !bindings.channelMap) {
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 knob 0x%02x delta=%d  (track has no BC/CS plugin)\n",
                ev.id, (int)ev.delta);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsSuppressed;
        return;
    }

    // Pick Bus Comp when available and the knob is a V-Pot; otherwise
    // fall back to Channel Strip.
    const PluginBindings* map = nullptr;
    int fxIdx = -1;
    bool busCompContext = false;
    uint8_t zone = zone::kChannelStripReadout;

    const ControlDomain domain = classifyKnob(ev.id);
    if (domain == ControlDomain::BusComp && bindings.busCompMap) {
        map           = bindings.busCompMap;
        fxIdx         = bindings.busCompFxIdx;
        busCompContext = true;
        zone          = zone::kBusCompReadout;
    } else if (bindings.channelMap) {
        map   = bindings.channelMap;
        fxIdx = bindings.channelFxIdx;
    } else if (bindings.busCompMap) {
        // No CS plugin — even a CS-area knob won't have anywhere to go.
        // Fall through as suppressed.
    }

    if (!map) {
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 knob 0x%02x delta=%d  (no matching domain binding)\n",
                ev.id, (int)ev.delta);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsSuppressed; return;
    }

    const int vst3Param = map->knobParam[ev.id];
    if (vst3Param == kParamNone) {
        if (logThis) {
            char line[128];
            std::snprintf(line, sizeof(line),
                "UC1 knob 0x%02x delta=%d  unmapped in %s  (add to UC1PluginMap)\n",
                ev.id, (int)ev.delta, map->shortName);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsSuppressed;
        return;
    }

    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
    double cur = TrackFX_GetParamNormalized(tr, fxIdx, vst3Param);
    double next = cur + clickToDelta_(ev.delta) * (map->inverted[ev.id] ? -1.0 : 1.0);
    next = std::clamp(next, 0.0, 1.0);
    TrackFX_SetParamNormalized(tr, fxIdx, vst3Param, next);

    pushKnobReadout_(ev.id, tr, fxIdx, vst3Param, zone, labelForKnob(ev.id, busCompContext));

    if (logThis) {
        char pname[64] = {0};
        TrackFX_GetParamName(tr, fxIdx, vst3Param, pname, sizeof(pname));
        char line[160];
        std::snprintf(line, sizeof(line),
            "UC1 knob 0x%02x '%s' delta=%d → VST3 param %d '%s' val=%.3f\n",
            ev.id, labelForKnob(ev.id, busCompContext),
            (int)ev.delta, vst3Param, pname, next);
        ShowConsoleMsg(line);
    }

    ++stats_.knobEventsHandled;
}

void UC1Surface::handleButton_(const ButtonEvent& ev)
{
    // Fine is a pure modifier — tracked at the surface, no plugin work.
    if (ev.id == button::kFine) {
        fineMode_.store(ev.pressed, std::memory_order_relaxed);
        pushButtonLed_(ev.id, ev.pressed);
        ++stats_.buttonEventsHandled;
        return;
    }

    // Track-level buttons (Solo / Cut / Solo Clear) are not plugin
    // params; let main.cpp wire those in when the track-state router
    // exists. For now we consume the event so callers can snoop stats.
    if (ev.id == button::kSolo     ||
        ev.id == button::kCut      ||
        ev.id == button::kSoloClear)
    {
        ++stats_.buttonEventsSuppressed;
        return;
    }

    if (!focusedTrack_ || !ev.pressed) {
        // Only act on the press edge — UC1 sends a release right after.
        if (ev.pressed) ++stats_.buttonEventsSuppressed;
        return;
    }

    auto bindings = lookupBindingsOnTrack(focusedTrack_);
    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);

    // Channel IN / Bus Comp IN toggle the plugin's bypass state.
    if (ev.id == button::kChannelIn && bindings.channelMap) {
        const bool wasEnabled = TrackFX_GetEnabled(tr, bindings.channelFxIdx);
        TrackFX_SetEnabled(tr, bindings.channelFxIdx, !wasEnabled);
        pushButtonLed_(ev.id, !wasEnabled);
        ++stats_.buttonEventsHandled;
        return;
    }
    if (ev.id == button::kBusCompIn && bindings.busCompMap) {
        const bool wasEnabled = TrackFX_GetEnabled(tr, bindings.busCompFxIdx);
        TrackFX_SetEnabled(tr, bindings.busCompFxIdx, !wasEnabled);
        pushButtonLed_(ev.id, !wasEnabled);
        ++stats_.buttonEventsHandled;
        return;
    }

    // Plugin-param toggles (EQ In, Dyn In, Fast Attack, etc.). These
    // live on the Channel Strip plugin.
    if (!bindings.channelMap) { ++stats_.buttonEventsSuppressed; return; }
    const int vst3Param = bindings.channelMap->buttonParam[ev.id];
    if (vst3Param == kParamNone) { ++stats_.buttonEventsSuppressed; return; }

    const double cur = TrackFX_GetParamNormalized(tr, bindings.channelFxIdx, vst3Param);
    const double next = (cur < 0.5) ? 1.0 : 0.0;
    TrackFX_SetParamNormalized(tr, bindings.channelFxIdx, vst3Param, next);
    pushButtonLed_(ev.id, next > 0.5);
    ++stats_.buttonEventsHandled;
}

namespace {

// Strip a single space between the numeric part and the unit suffix.
// REAPER's TrackFX_FormatParamValueNormalized returns "12.1 dB",
// "102.5 Hz", "50.0 %", "0.12 s" — SSL 360°'s zone 0x05 format is the
// same without the separator space ("12.1dB", "102.5Hz").
std::string compactUnit(std::string_view s)
{
    std::string r{s};
    static constexpr std::string_view units[] = {
        " dB", " Hz", " kHz", " ms", " s", " %", " :1",
    };
    for (auto u : units) {
        auto p = r.rfind(u);
        if (p == std::string::npos) continue;
        // Only strip the separator space if the char immediately
        // before it is digit/decimal — i.e. a numeric value. For
        // "OFF Hz" the char before the space is 'F', not numeric, so
        // we leave the whole string alone (display will show "OFF").
        if (p == 0) break;
        const char prev = r[p - 1];
        const bool isNumeric = (prev >= '0' && prev <= '9') || prev == '.';
        if (!isNumeric) break;
        r.erase(p, 1);
        break;
    }
    return r;
}

// For non-numeric values like "OFF", "AUTO", "N/A" — drop the unit
// token entirely. SSL 360° shows just "OFF" without " Hz" in that
// case (uc1_07 captured "S/C HPF         OFF Hz" but the OFF plays
// cleaner without the trailing unit).
std::string stripUnitIfNonNumeric(std::string_view s)
{
    static constexpr std::string_view units[] = {
        " dB", " Hz", " kHz", " ms", " s", " %", " :1",
    };
    for (auto u : units) {
        auto p = s.rfind(u);
        if (p == std::string::npos) continue;
        if (p == 0) return std::string{s};
        const char prev = s[p - 1];
        const bool isNumeric = (prev >= '0' && prev <= '9') || prev == '.';
        if (!isNumeric) return std::string{s.substr(0, p)};
    }
    return std::string{s};
}

// Right-pad / left-pad a value string to a fixed width. UC1's numeric
// LCD has physically-spaced digit slots with a gap between the
// leftmost sign/overflow slot and the main digit column. SSL 360°
// sends values in a fixed position so chars always land in expected
// slots; anything shorter gets leading spaces, anything longer
// "overflows" into the sign slot (bad — shows e.g. "1    02.5Hz"
// instead of "102.5Hz"). Forcing 7 chars matches the captured format
// for the widest Bus Comp values ("-20.0dB", "-10.0dB", "102.5Hz").
std::string padValueFixed(std::string_view s, size_t width = 7)
{
    if (s.size() >= width) return std::string{s};
    std::string r(width - s.size(), ' ');
    r.append(s);
    return r;
}

} // namespace

void UC1Surface::pushKnobReadout_(uint8_t knobId, void* trackRaw, int fxIdx,
                                  int vst3Param, uint8_t zone,
                                  std::string_view label)
{
    if (!device_) return;

    MediaTrack* tr = static_cast<MediaTrack*>(trackRaw);
    char formatted[64];
    formatted[0] = '\0';
    const double cur = TrackFX_GetParamNormalized(tr, fxIdx, vst3Param);
    TrackFX_FormatParamValueNormalized(tr, fxIdx, vst3Param, cur,
                                       formatted, sizeof(formatted));
    // UC1 expects compact "X.XdB" / "X.XHz" / "X%". Non-numeric values
    // ("OFF", "AUTO", "N/A") drop the unit entirely. No padding — the
    // readout builder anchors the value at position 16 regardless of
    // length, which is what SSL 360° does in captures.
    std::string value = stripUnitIfNonNumeric(compactUnit(formatted));

    auto readout = formatReadout(label, value);

    // Diag — log the final 22-char string for the first N events per
    // knob ID. Use '·' placeholder for space so we can count columns
    // exactly. Helps diagnose value-alignment problems in zone 0x05/03.
    static int kReadoutDebugRemaining[0x20] = {
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    };
    if (knobId < 0x20 && kReadoutDebugRemaining[knobId] > 0) {
        --kReadoutDebugRemaining[knobId];
        std::string vis = readout;
        for (auto& c : vis) if (c == ' ') c = '.';
        char line[64];
        std::snprintf(line, sizeof(line),
            "UC1 push zone=0x%02x raw='%s' → '%s'\n",
            zone, formatted, vis.c_str());
        ShowConsoleMsg(line);
    }

    // Variable-width readout — 22 for 6-char values, 23 for 7-char.
    // UC1 accepts either; what matters is that value digits land at
    // position 16 onwards.
    device_->send(buildDisplayText(zone, readout, readout.size()));
}

void UC1Surface::pushButtonLed_(uint8_t buttonId, bool on)
{
    if (!device_) return;
    auto cell = cellForButton(buttonId);
    if (cell.bank == 0 && cell.cell == 0) return;  // unmapped
    device_->send(buildLedWrite(cell.bank, cell.cell,
                                on ? led::kStateOn : led::kStateOff));
}

void UC1Surface::refresh()
{
    if (!device_) return;

    auto bindings = focusedTrack_ ? lookupBindingsOnTrack(focusedTrack_) : UC1Bindings{};

    // Track name push — zones 0x02 (CS slot) and 0x04 (BC slot). Per
    // SSL UC1 User Guide p.17 each slot shows the DAW track name of
    // the track that has THAT specific plugin inserted. If a plugin
    // isn't present on the focused track, its slot reads "No Plug-ins".
    // Empty string → device keeps the last state; we explicitly reset
    // the slot to all-zeros when empty so the stale track name from a
    // previous focus doesn't linger.
    auto resolveTrackName = [&]() -> std::string {
        if (!focusedTrack_) return {};
        MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
        char nameBuf[128] = {0};
        if (GetSetMediaTrackInfo_String(tr, "P_NAME", nameBuf, false)
            && nameBuf[0] != '\0')
        {
            return nameBuf;
        }
        int idx = static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER"));
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "Track %d", idx);
        return fallback;
    };

    // CS slot always shows the focused track's name — Rea-Sixty uses
    // the Channel Strip display as the general "current track" view,
    // whether or not an SSL Channel Strip 2 plugin is on the track.
    // BC slot stays plugin-specific since its knobs are only
    // meaningful when Bus Comp 2 is actually present.
    std::string csName = resolveTrackName();
    std::string bcName = bindings.busCompMap ? resolveTrackName() : std::string{};

    // Diag — one-shot log of what we're pushing so the user can
    // correlate with what the UC1 actually displays.
    {
        static int kDiagRemaining = 8;
        if (kDiagRemaining > 0) {
            --kDiagRemaining;
            char line[128];
            std::snprintf(line, sizeof(line),
                "UC1 refresh  cs='%s' bc='%s'\n",
                csName.c_str(), bcName.c_str());
            ShowConsoleMsg(line);
        }
    }

    device_->send(buildChannelStripContext(csName));
    device_->send(buildBusCompContext(bcName));

    // Plugin-name tag (zone 0x10) — shows which CS plugin variant is
    // currently driving the Channel Strip section. Bus Comp 2 isn't
    // reflected here in captures; when neither plugin is present we
    // leave zone 0x10 blank so SSL-style "No Plug-ins" status lives
    // in zone 0x04 (which we don't populate yet).
    const char* nameTag =
        bindings.channelMap ? bindings.channelMap->shortName :
        bindings.busCompMap ? bindings.busCompMap->shortName :
        "    ";
    device_->send(buildDisplayText(zone::kPluginNameTag, nameTag, 4));

    // Push each button's LED to mirror its current plugin-param state.
    // We walk the full button-ID range and ask the appropriate binding
    // for each one; kParamNone or no binding → LED off.
    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
    auto ledForParam = [&](const PluginBindings* map, int fxIdx, uint8_t btnId) {
        if (!map || !tr) return false;
        const int p = map->buttonParam[btnId];
        if (p == kParamNone) return false;
        const double v = TrackFX_GetParamNormalized(tr, fxIdx, p);
        return v >= 0.5;
    };

    for (uint8_t btn = 0; btn < 0x20; ++btn) {
        const auto cell = cellForButton(btn);
        if (cell.bank == 0 && cell.cell == 0) continue;  // not an LED

        bool on = false;
        switch (classifyButton(btn)) {
            case ControlDomain::BusComp:
                if (bindings.busCompMap && tr) {
                    on = TrackFX_GetEnabled(tr, bindings.busCompFxIdx);
                }
                break;
            case ControlDomain::ChannelStrip:
                if (btn == button::kChannelIn) {
                    on = bindings.channelMap && tr
                         && TrackFX_GetEnabled(tr, bindings.channelFxIdx);
                } else {
                    on = ledForParam(bindings.channelMap,
                                     bindings.channelFxIdx, btn);
                }
                break;
        }

        // Fine tracks the surface's own modifier state, not a plugin param.
        if (btn == button::kFine) on = fineMode_.load(std::memory_order_relaxed);

        // Solo / Cut / Solo Clear still route through REAPER track-state
        // rather than plugin params — skip them here; the track-state
        // hook will push LEDs once wired up.
        if (btn == button::kSolo || btn == button::kCut
            || btn == button::kSoloClear) continue;

        device_->send(buildLedWrite(cell.bank, cell.cell,
                                    on ? led::kStateOn : led::kStateOff));
    }

    // Zero the Bus Comp GR readout so stale values from the last track
    // don't linger until the JSFX probe next ticks.
    if (bindings.busCompMap) {
        device_->send(buildZeroGr());
    }
}

void UC1Surface::pushGainReduction(float dB)
{
    if (!device_) return;
    // UC1Device streams GR at 50 Hz on its own; we just update the
    // cached value. No per-call send needed.
    device_->setGainReduction(dB);
}

void UC1Surface::pushVu(uint8_t meter, uint8_t level)
{
    if (!device_) return;
    device_->send(buildVuMeter(meter, level));
}

} // namespace uc1
