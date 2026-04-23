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

// Pad or truncate a "<label><value>" pair to the 22-char zone width,
// left-justifying label and right-justifying value with spaces between.
std::string formatReadout(std::string_view label, std::string_view value)
{
    constexpr size_t kWidth = 22;
    std::string out(kWidth, ' ');
    // std::min is macro-shadowed by WDL/swell — wrap to suppress expansion.
    const size_t lmax = (std::min)(label.size(), kWidth);
    std::memcpy(out.data(), label.data(), lmax);
    if (value.size() >= kWidth - lmax) {
        // Value runs into the label — truncate value from the left-hand
        // side so the tail (usually "dB", "Hz", units) stays visible.
        const size_t take = kWidth - lmax;
        std::memcpy(out.data() + lmax, value.data() + (value.size() - take), take);
    } else {
        std::memcpy(out.data() + (kWidth - value.size()), value.data(), value.size());
    }
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
    // Each encoder click = ~1/32 of a full param sweep. Fine mode = 1/4.
    constexpr double kStepPerClick = 1.0 / 32.0;
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

void UC1Surface::pushKnobReadout_(uint8_t knobId, void* trackRaw, int fxIdx,
                                  int vst3Param, uint8_t zone,
                                  std::string_view label)
{
    if (!device_) return;

    MediaTrack* tr = static_cast<MediaTrack*>(trackRaw);
    char formatted[64];
    formatted[0] = '\0';
    // TrackFX_FormatParamValueNormalized gives us "-12.1 dB", "45.0 %",
    // "250 Hz" etc. — exactly what SSL 360° shows on the UC1.
    const double cur = TrackFX_GetParamNormalized(tr, fxIdx, vst3Param);
    TrackFX_FormatParamValueNormalized(tr, fxIdx, vst3Param, cur,
                                       formatted, sizeof(formatted));
    std::string value{formatted};
    // REAPER returns "X.X dB" with space; SSL 360°'s readout is "X.XdB"
    // compact. Strip the space before unit — optional cosmetic polish.
    // Keep it simple for now.

    auto readout = formatReadout(label, value);
    device_->send(buildDisplayText(zone, readout, 22));
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
