#include "UC1Surface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "reaper_plugin_functions.h"
#include "Palette.h"  // uf8::quantize for UC1 focused-track colour

// Defined in main.cpp — scroll REAPER's MCP so the just-selected track
// is visible (and, on UF8, rebank the 8-strip window around it). Shared
// with UF8's SEL/CHANNEL-encoder paths so UC1 encoders feel identical.
void reasixty_followSelectedInMixer(MediaTrack* tr);

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

    // Helper for fine-tick encoders (CHANNEL, BC): accumulate deltas
    // across events so N ticks = 1 physical click. Direction change
    // OR a >100 ms gap since the last event clears residual ticks,
    // so occasional short-tick clicks don't silently accumulate drift.
    auto stepFromAccumulator = [&ev](int& acc, auto& lastT, int ticksPerStep) -> int {
        auto now = std::chrono::steady_clock::now();
        if (lastT.time_since_epoch().count() != 0
            && now - lastT > std::chrono::milliseconds(100)) {
            acc = 0;
        }
        lastT = now;
        if ((ev.delta > 0 && acc < 0) || (ev.delta < 0 && acc > 0)) acc = 0;
        acc += ev.delta;
        int step = acc / ticksPerStep;
        acc -= step * ticksPerStep;
        return step;
    };

    // CHANNEL encoder — scroll through ALL REAPER tracks. ~4 ticks/click.
    if (ev.id == knob::kChannelEncoder) {
        static int acc = 0;
        static std::chrono::steady_clock::time_point lastT{};
        int step = stepFromAccumulator(acc, lastT, 4);
        if (step == 0) { ++stats_.knobEventsHandled; return; }
        const int n = CountTracks(nullptr);
        if (n <= 0) return;
        int cur = -1;
        if (focusedTrack_) {
            cur = static_cast<int>(GetMediaTrackInfo_Value(
                static_cast<MediaTrack*>(focusedTrack_), "IP_TRACKNUMBER")) - 1;
        }
        int next = cur + step;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
        MediaTrack* tr = GetTrack(nullptr, next);
        if (tr) {
            SetOnlyTrackSelected(tr);
            reasixty_followSelectedInMixer(tr);
            setFocusedTrack(tr);
        }
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 CHANNEL delta=%d step=%d → track %d of %d\n",
                (int)ev.delta, step, next + 1, n);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsHandled;
        return;
    }

    // BC encoder — jump to the next/prev track (relative to the
    // currently focused track) that has a plugin targeted by the
    // Bus-Comp section. "Next" = first BC track whose project index
    // is greater than the focused track's; "prev" = first BC track
    // whose project index is smaller. Works whether or not the
    // focused track itself has a BC plugin.
    if (ev.id == knob::kBcEncoder) {
        static int acc = 0;
        static std::chrono::steady_clock::time_point lastT{};
        int step = stepFromAccumulator(acc, lastT, 3);
        if (step == 0) { ++stats_.knobEventsHandled; return; }
        const int n = CountTracks(nullptr);
        if (n <= 0) return;

        int curIdx = -1;
        if (focusedTrack_) {
            curIdx = static_cast<int>(GetMediaTrackInfo_Value(
                static_cast<MediaTrack*>(focusedTrack_), "IP_TRACKNUMBER")) - 1;
        }

        const bool forward = step > 0;
        const int stepsAbs = forward ? step : -step;
        int probe = curIdx;
        int found = -1;
        for (int k = 0; k < stepsAbs; ++k) {
            int next = -1;
            if (forward) {
                for (int i = probe + 1; i < n; ++i) {
                    auto b = lookupBindingsOnTrack(GetTrack(nullptr, i));
                    if (b.busCompMap) { next = i; break; }
                }
            } else {
                for (int i = probe - 1; i >= 0; --i) {
                    auto b = lookupBindingsOnTrack(GetTrack(nullptr, i));
                    if (b.busCompMap) { next = i; break; }
                }
            }
            if (next < 0) break;  // no more BC tracks in this direction
            found = next;
            probe = next;
        }

        if (found < 0) {
            if (logThis) ShowConsoleMsg("UC1 BC encoder: no BC track in that direction\n");
            ++stats_.knobEventsHandled;
            return;
        }

        MediaTrack* tr = GetTrack(nullptr, found);
        if (tr) {
            SetOnlyTrackSelected(tr);
            reasixty_followSelectedInMixer(tr);
            setFocusedTrack(tr);
        }
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 BC delta=%d step=%d → track %d\n",
                (int)ev.delta, step, found + 1);
            ShowConsoleMsg(line);
        }
        ++stats_.knobEventsHandled;
        return;
    }

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
    // Pass the visual position (flipped when the pot is inverted) so
    // the LED ring goes CW when the pot goes CW — independent of which
    // way the VST3 param value moves.
    const double visual = map->inverted[ev.id] ? (1.0 - next) : next;
    pushKnobRing_(ev.id, visual);

    if (logThis) {
        char pname[64] = {0};
        TrackFX_GetParamName(tr, fxIdx, vst3Param, pname, sizeof(pname));
        char line[192];
        std::snprintf(line, sizeof(line),
            "UC1 knob 0x%02x '%s' plug=%s inv=%d delta=%d → param %d '%s' val=%.3f\n",
            ev.id, labelForKnob(ev.id, busCompContext),
            map->shortName, map->inverted[ev.id] ? 1 : 0,
            (int)ev.delta, vst3Param, pname, next);
        ShowConsoleMsg(line);
    }

    ++stats_.knobEventsHandled;
}

void UC1Surface::handleButton_(const ButtonEvent& ev)
{
    // Fine is a latching toggle — press flips the mode, release is a
    // no-op. LED reflects the latched state. User preference
    // 2026-04-23: momentary "hold to fine-tune" was awkward for long
    // parameter sweeps; toggle lets the user engage Fine once and
    // adjust as many knobs as they want.
    if (ev.id == button::kFine) {
        if (ev.pressed) {
            const bool next = !fineMode_.load(std::memory_order_relaxed);
            fineMode_.store(next, std::memory_order_relaxed);
            pushButtonLed_(ev.id, next);
        }
        ++stats_.buttonEventsHandled;
        return;
    }

    // Track-level buttons: act on the press edge; release is a no-op.
    // Solo/Cut target the focused track; Solo Clear is a global unsolo.
    // We push the LED inline (pushButtonLed_, same path Fine uses) to
    // avoid depending on REAPER's SetSurface* callback firing back on
    // the initiating surface. One-shot diag so we can see in the
    // console what state each press computed.
    auto anySolo = []() -> bool {
        const int n = CountTracks(nullptr);
        for (int i = 0; i < n; ++i) {
            if (GetMediaTrackInfo_Value(GetTrack(nullptr, i), "I_SOLO") > 0.5) return true;
        }
        return false;
    };
    static int kDiagSoloCut = 12;
    if (ev.id == button::kSolo) {
        if (ev.pressed && focusedTrack_) {
            MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
            CSurf_OnSoloChange(tr, -1);
            const bool on = GetMediaTrackInfo_Value(tr, "I_SOLO") > 0.5;
            pushButtonLed_(button::kSolo, on);
            pushButtonLed_(button::kSoloClear, anySolo());
            if (kDiagSoloCut > 0) {
                --kDiagSoloCut;
                char line[80];
                std::snprintf(line, sizeof(line),
                    "UC1 Solo press → solo=%d anySolo=%d\n", on, anySolo());
                ShowConsoleMsg(line);
            }
            ++stats_.buttonEventsHandled;
        }
        return;
    }
    if (ev.id == button::kCut) {
        if (ev.pressed && focusedTrack_) {
            MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
            CSurf_OnMuteChange(tr, -1);
            const bool on = GetMediaTrackInfo_Value(tr, "B_MUTE") > 0.5;
            pushButtonLed_(button::kCut, on);
            if (kDiagSoloCut > 0) {
                --kDiagSoloCut;
                char line[80];
                std::snprintf(line, sizeof(line),
                    "UC1 Cut press → mute=%d\n", on);
                ShowConsoleMsg(line);
            }
            ++stats_.buttonEventsHandled;
        }
        return;
    }
    if (ev.id == button::kSoloClear) {
        if (ev.pressed) {
            // REAPER action 40340 = "Track: Unsolo all tracks".
            Main_OnCommand(40340, 0);
            pushButtonLed_(button::kSoloClear, anySolo());
            // Every strip's solo LED could have just been turned off,
            // but since we only light Solo for the focused track here,
            // a single refresh on the focused one is enough.
            if (focusedTrack_) {
                const bool on = GetMediaTrackInfo_Value(
                    static_cast<MediaTrack*>(focusedTrack_), "I_SOLO") > 0.5;
                pushButtonLed_(button::kSolo, on);
            }
            if (kDiagSoloCut > 0) {
                --kDiagSoloCut;
                char line[80];
                std::snprintf(line, sizeof(line),
                    "UC1 SoloClear press → anySolo=%d\n", anySolo());
                ShowConsoleMsg(line);
            }
            ++stats_.buttonEventsHandled;
        }
        return;
    }

    if (!focusedTrack_ || !ev.pressed) {
        // Only act on the press edge — UC1 sends a release right after.
        if (ev.pressed) ++stats_.buttonEventsSuppressed;
        return;
    }

    auto bindings = lookupBindingsOnTrack(focusedTrack_);
    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);

    // Polarity — toggle REAPER's per-track phase-invert (B_PHASE),
    // not a plugin param. cap17/18 noted this button produces no
    // FF 22 event on at least some firmwares; if it does fire, we
    // want it routed to REAPER track state. LED mirrors B_PHASE
    // regardless (refresh() picks it up on track changes).
    if (ev.id == button::kPolarity) {
        const bool cur = GetMediaTrackInfo_Value(tr, "B_PHASE") > 0.5;
        SetMediaTrackInfo_Value(tr, "B_PHASE", cur ? 0.0 : 1.0);
        pushButtonLed_(ev.id, !cur);
        ++stats_.buttonEventsHandled;
        return;
    }

    // Channel IN — two modes:
    //   * SSL Channel Strip on track: toggle the plugin's internal
    //     "Channel In" switch (found by VST3 param name). This mirrors
    //     the plugin's own IN button, not the global bypass.
    //   * No SSL plugin: fall back to bypassing the first track FX.
    if (ev.id == button::kChannelIn) {
        if (bindings.channelMap) {
            const int p = channelInParam_(tr, bindings.channelFxIdx);
            if (p >= 0) {
                const double cur = TrackFX_GetParamNormalized(
                    tr, bindings.channelFxIdx, p);
                const double next = (cur > 0.5) ? 0.0 : 1.0;
                TrackFX_SetParamNormalized(
                    tr, bindings.channelFxIdx, p, next);
                pushButtonLed_(ev.id, next > 0.5);
            } else {
                // Param-by-name lookup failed — degrade gracefully to
                // plugin-bypass so the button still does *something*.
                const bool wasEnabled = TrackFX_GetEnabled(
                    tr, bindings.channelFxIdx);
                TrackFX_SetEnabled(tr, bindings.channelFxIdx, !wasEnabled);
                pushButtonLed_(ev.id, !wasEnabled);
            }
        } else {
            // No SSL plugin — bypass the first track FX if any.
            if (TrackFX_GetCount(tr) > 0) {
                const bool wasEnabled = TrackFX_GetEnabled(tr, 0);
                TrackFX_SetEnabled(tr, 0, !wasEnabled);
                pushButtonLed_(ev.id, !wasEnabled);
            }
        }
        ++stats_.buttonEventsHandled;
        return;
    }
    // Bus Comp IN still toggles the Bus Comp plugin's bypass — no
    // separate internal IN param on BC 2 that we need to route to.
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

int UC1Surface::channelInParam_(void* trackRaw, int fxIdx)
{
    MediaTrack* tr = static_cast<MediaTrack*>(trackRaw);
    if (!tr || fxIdx < 0) return -1;

    // Cache by (track, fxIdx) so we only scan the param list once per
    // focus change. Simple 1-entry cache — each focus change blows it
    // away. Good enough for the single-focused-track model.
    static void* cachedTr = nullptr;
    static int   cachedFx = -1;
    static int   cachedP  = -1;
    if (tr == cachedTr && fxIdx == cachedFx) return cachedP;
    cachedTr = tr; cachedFx = fxIdx; cachedP = -1;

    const int n = TrackFX_GetNumParams(tr, fxIdx);
    char buf[256];
    // Known spellings across SSL plugin variants. Strict exact match.
    // Do NOT add "Bypass" here — its semantics are inverted from IN
    // (Bypass=1 means IN=off) and would make the LED mirror the wrong
    // state. Generic names like "In"/"On"/"Channel" are kept but
    // placed last so more specific hits win first.
    static const char* const kCandidates[] = {
        "CsIn", "ChannelIn", "Channel In", "CHANNELIN", "CHANNEL IN",
        "ChIn", "Ch In", "CS In", "CS_IN", "Cs In",
        "In", "On"
    };
    // One-shot diag dump of every param name on first access. Helps
    // identify the correct name when our candidate list misses.
    static bool kDumpedParams = false;
    if (!kDumpedParams) {
        kDumpedParams = true;
        ShowConsoleMsg("UC1 CS param names:\n");
        for (int i = 0; i < n; ++i) {
            if (!TrackFX_GetParamName(tr, fxIdx, i, buf, sizeof(buf))) continue;
            char line[320];
            std::snprintf(line, sizeof(line), "  [%d] '%s'\n", i, buf);
            ShowConsoleMsg(line);
        }
    }
    for (int i = 0; i < n; ++i) {
        if (!TrackFX_GetParamName(tr, fxIdx, i, buf, sizeof(buf))) continue;
        for (auto c : kCandidates) {
            if (std::strcmp(buf, c) == 0) { cachedP = i; return i; }
        }
    }
    return -1;
}

namespace {
// Pot LED ring cell maps, extracted from 2026-04-24 captures (dual_37..
// dual_41). Cells are in the SSL-captured write order; that order
// should approximate the LED arc sweep CCW→CW, but visual
// verification is pending. Encoding per pot:
//   Position: value maps to single-LED-highlight at index v*N
//   Bipolar:  center = middle LED lit, fills outward
//   Additive: value fills LEDs cumulatively from index 0
enum RingEncoding { Position, Bipolar, Additive };

struct RingDef { const uint8_t* cells; int nCells; RingEncoding kind; };

// Low Pass — 10 positions observed in dual_37, Position mode (single
// LED lit at a time). Cells in capture order 0x95..0x9F. Visual
// direction (pot CW = LED CW) is enforced by flipping the value to
// (1-value) for inverted pots in handleKnob_ and refresh() before
// calling pushKnobRing_. No cell reversal needed.
constexpr uint8_t kLpfCells[] = {
    0x95, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
};

const RingDef* ringFor(uint8_t knobId)
{
    static const RingDef kLpf{kLpfCells, 10, Position};
    switch (knobId) {
        case knob::kCSLowPass: return &kLpf;
        // TODO: other pots need per-cap cluster analysis to pin cells.
    }
    return nullptr;
}
} // namespace

void UC1Surface::pushKnobRing_(uint8_t knobId, double normalized)
{
    if (!device_) return;
    const RingDef* def = ringFor(knobId);
    if (!def) return;  // knob not yet mapped — no LED update

    if (normalized < 0.0) normalized = 0.0;
    if (normalized > 1.0) normalized = 1.0;

    // Per-knob state cache so we only push cells that changed state.
    static std::unordered_map<uint8_t, std::vector<uint8_t>> lastStates;
    auto& last = lastStates[knobId];
    if (static_cast<int>(last.size()) != def->nCells) {
        last.assign(def->nCells, 0xFE);  // "unset" sentinel
    }

    // Determine which cells should be lit based on encoding.
    std::vector<uint8_t> target(def->nCells, 0);
    switch (def->kind) {
        case Position: {
            int idx = static_cast<int>(normalized * (def->nCells - 1) + 0.5);
            if (idx < 0) idx = 0;
            if (idx >= def->nCells) idx = def->nCells - 1;
            target[idx] = 1;
            break;
        }
        case Bipolar: {
            // Center = 0.5 → middle LED; fill outward as value moves
            // away from center in either direction.
            const int mid = (def->nCells - 1) / 2;
            const int fromCenter = static_cast<int>(
                std::abs(normalized - 0.5) * (def->nCells - 1) + 0.5);
            const int start = (normalized < 0.5) ? (mid - fromCenter) : mid;
            const int end   = (normalized < 0.5) ? mid : (mid + fromCenter);
            for (int i = 0; i < def->nCells; ++i) {
                if (i >= start && i <= end) target[i] = 1;
            }
            break;
        }
        case Additive: {
            int n = static_cast<int>(normalized * def->nCells + 0.5);
            if (n > def->nCells) n = def->nCells;
            for (int i = 0; i < n; ++i) target[i] = 1;
            break;
        }
    }

    // Push changes: dual-bank encoding per cell. Bank 0x01 (role 0x00)
    // = selection 0/1. Bank 0x02 (role 0x00) = brightness 0/FF.
    for (int i = 0; i < def->nCells; ++i) {
        if (last[i] == target[i]) continue;
        last[i] = target[i];
        const uint8_t cell = def->cells[i];
        const uint8_t selState = target[i] ? 0x01 : 0x00;
        const uint8_t brState  = target[i] ? 0xFF : 0x00;
        // buildLedWrite uses role=0x01 by default; pot rings use role=0x00.
        // Build frames manually to override role.
        auto make = [](uint8_t bank, uint8_t cell, uint8_t state) {
            std::vector<uint8_t> f;
            f.reserve(8);
            f.push_back(0xFF);
            f.push_back(0x13);
            f.push_back(0x04);
            f.push_back(bank);
            f.push_back(cell);
            f.push_back(0x00);     // role
            f.push_back(state);
            uint32_t sum = 0;
            for (size_t k = 1; k < f.size(); ++k) sum += f[k];
            f.push_back(static_cast<uint8_t>(sum & 0xFF));
            return f;
        };
        device_->send(make(0x01, cell, selState));
        device_->send(make(0x02, cell, brState));
    }
}

void UC1Surface::pushButtonLed_(uint8_t buttonId, bool on)
{
    if (!device_) return;
    auto cell = cellForButton(buttonId);
    if (cell.bank == 0 && cell.cell == 0) return;  // unmapped

    // Per-button state encoding.
    //   - Most button LEDs use the bank in led::Cell + 0xFF on / 0x00
    //     off (cap21 decode, plugin-param section).
    //   - Solo Clear uses bank 0x01 + state 0x01 on / 0x00 off (cap17,
    //     verified 2026-04-23).
    //   - Solo and Cut: fan-out test confirmed the LED lights with
    //     bank 0x01 cell 0x97/0x96 state 0x01 (same scheme as Solo
    //     Clear). The bank 0x02 cells from cap21 are probably status
    //     registers, not LED drivers. We override the cell-table bank
    //     for those two buttons and use state=0x01.
    uint8_t bank = cell.bank;
    uint8_t stateOn = led::kStateOn;
    if (buttonId == button::kSoloClear) {
        stateOn = 0x01;
    } else if (buttonId == button::kSolo      ||
               buttonId == button::kCut       ||
               buttonId == button::kPolarity  ||
               buttonId == button::kChannelIn ||
               buttonId == button::kBusCompIn)
    {
        // Central-section track buttons all use bank 0x01 + state 0x01
        // for their LEDs. Confirmed empirically: the bank 0x02 cells
        // listed in cap21 are status registers, not LED drivers.
        bank = 0x01;
        stateOn = 0x01;
    }
    const uint8_t state = on ? stateOn : led::kStateOff;

    auto frame = buildLedWrite(bank, cell.cell, state);

    // One-shot diag so we can eyeball exactly what hits the bulk OUT
    // endpoint. Matches the cap17/cap21 frame format when things are
    // right (e.g. Solo on → "FF 13 04 02 97 01 FF B0").
    static int kDiagLed = 24;
    if (kDiagLed > 0) {
        --kDiagLed;
        char line[128];
        int off = std::snprintf(line, sizeof(line),
            "UC1 LED btn=0x%02x on=%d frame=", buttonId, on);
        for (auto b : frame) {
            off += std::snprintf(line + off, sizeof(line) - off, "%02x ", b);
        }
        std::snprintf(line + off, sizeof(line) - off, "\n");
        ShowConsoleMsg(line);
    }

    device_->send(std::move(frame));
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

    // Track-name carousel — 3 slots [prev, current, next].
    // REAPER track index is 0-based; focused track is the middle slot.
    // Empty slot strings leave the slot's zero-pad intact so edge cases
    // (first/last track) don't show stale names.
    auto nameOfIdx = [](int idx) -> std::string {
        const int n = CountTracks(nullptr);
        if (idx < 0 || idx >= n) return "";
        MediaTrack* t = GetTrack(nullptr, idx);
        char buf[128] = {0};
        if (GetSetMediaTrackInfo_String(t, "P_NAME", buf, false) && buf[0]) return buf;
        char fallback[16];
        std::snprintf(fallback, sizeof(fallback), "Trk %d", idx + 1);
        return fallback;
    };
    // BC-name carousel: only populate slots for tracks that host a Bus
    // Comp 2 (or another BC-mapped plugin). Tracks without BC get an
    // empty slot so the bottom-right display doesn't duplicate the
    // main/CS carousel.
    auto bcNameOfIdx = [&nameOfIdx](int idx) -> std::string {
        const int n = CountTracks(nullptr);
        if (idx < 0 || idx >= n) return "";
        MediaTrack* t = GetTrack(nullptr, idx);
        const auto b = lookupBindingsOnTrack(t);
        if (!b.busCompMap) return "";  // no BC on this track → empty slot
        return nameOfIdx(idx);
    };
    int curIdx = -1;
    if (focusedTrack_) {
        curIdx = static_cast<int>(GetMediaTrackInfo_Value(
            static_cast<MediaTrack*>(focusedTrack_), "IP_TRACKNUMBER")) - 1;
    }
    const std::string prevName = nameOfIdx(curIdx - 1);
    const std::string currName = nameOfIdx(curIdx);
    const std::string nextName = nameOfIdx(curIdx + 1);
    device_->send(buildTrackNameTripleSmall(prevName, currName, nextName));
    device_->send(buildTrackNameTripleLarge(
        bcNameOfIdx(curIdx - 1),
        bcNameOfIdx(curIdx),
        bcNameOfIdx(curIdx + 1)));

    // 7-segment position indicator — show the REAPER track number
    // (1-based) on the central red display. Matches the MAIN/ROUTING
    // page of the Central Control Panel.
    if (focusedTrack_) {
        int idx = static_cast<int>(GetMediaTrackInfo_Value(
            static_cast<MediaTrack*>(focusedTrack_), "IP_TRACKNUMBER"));
        if (idx < 0) idx = 0;
        for (const auto& frame : buildSevenSeg(static_cast<unsigned int>(idx))) {
            device_->send(frame);
        }
    }

    // Central label — 4-char plugin-type tag shown in the UC1 central
    // LCD. "MAIN" when no SSL plugin is focused, otherwise the plugin's
    // shortName ("CS 2", "BC 2", "4K E" …). Also drives the
    // colour-bar-enable flag that gates the coloured top-stripe
    // rendering: 0x01 when plugin context exists, 0x00 for MAIN.
    const bool havePlugin = bindings.channelMap || bindings.busCompMap;
    device_->send(buildColourBarEnable(havePlugin));
    const char* label =
        bindings.channelMap ? bindings.channelMap->shortName :
        bindings.busCompMap ? bindings.busCompMap->shortName :
        "MAIN";
    device_->send(buildCentralLabel(label));

    // Focused-track colour bar — single palette byte. Uses the same
    // quantizer as UF8's color-bar (uf8::quantize on the track's
    // 0xRRGGBB colour). When no plugin is loaded the bar is inactive
    // anyway (colour-bar-enable=0), but we still push a palette=0x00
    // to clear stale state.
    {
        uint8_t palette = 0x00;
        if (focusedTrack_) {
            MediaTrack* t = static_cast<MediaTrack*>(focusedTrack_);
            const uint32_t rgb = static_cast<uint32_t>(GetTrackColor(t)) & 0x00FFFFFFu;
            palette = (rgb == 0) ? 0x00 : uf8::quantize(rgb);
        }
        device_->send(buildFocusedColour(palette));
    }

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
            case ControlDomain::BusComp: {
                // Bus Comp IN (the only button in this domain) uses the
                // bank=0x01/state=0x01 override just like the other
                // central-section track buttons. LED reflects plugin
                // enabled state; defaults to on when BC is present.
                bool bcOn = false;
                if (bindings.busCompMap && tr) {
                    bcOn = TrackFX_GetEnabled(tr, bindings.busCompFxIdx);
                }
                pushButtonLed_(btn, bcOn);
                continue;
            }
            case ControlDomain::ChannelStrip:
                if (btn == button::kChannelIn) {
                    // ChannelIn uses the bank=0x01/state=0x01 LED
                    // encoding override — route through pushButtonLed_
                    // (same path as Solo/Cut/Polarity) rather than the
                    // direct buildLedWrite fall-through below.
                    bool cin = false;
                    if (bindings.channelMap && tr) {
                        const int p = channelInParam_(tr, bindings.channelFxIdx);
                        if (p >= 0) {
                            cin = TrackFX_GetParamNormalized(
                                tr, bindings.channelFxIdx, p) > 0.5;
                        } else {
                            cin = TrackFX_GetEnabled(tr, bindings.channelFxIdx);
                        }
                    } else if (tr && TrackFX_GetCount(tr) > 0) {
                        cin = TrackFX_GetEnabled(tr, 0);
                    }
                    pushButtonLed_(btn, cin);
                    continue;
                } else {
                    on = ledForParam(bindings.channelMap,
                                     bindings.channelFxIdx, btn);
                }
                break;
        }

        // Fine tracks the surface's own modifier state, not a plugin param.
        if (btn == button::kFine) on = fineMode_.load(std::memory_order_relaxed);

        // Track-state LEDs: Solo/Cut/Polarity mirror the focused
        // track; Solo Clear lights when any track in the project is
        // soloed. Route through pushButtonLed_ so the per-button
        // state-encoding overrides (bank/state mappings for
        // Solo/Cut/SoloClear) apply — a direct buildLedWrite here
        // would use the wrong bank and leave stale LEDs lit after
        // track switches.
        if (btn == button::kSolo) {
            pushButtonLed_(btn, tr && GetMediaTrackInfo_Value(tr, "I_SOLO") > 0.5);
            continue;
        }
        if (btn == button::kCut) {
            pushButtonLed_(btn, tr && GetMediaTrackInfo_Value(tr, "B_MUTE") > 0.5);
            continue;
        }
        if (btn == button::kPolarity) {
            pushButtonLed_(btn, tr && GetMediaTrackInfo_Value(tr, "B_PHASE") > 0.5);
            continue;
        }
        if (btn == button::kSoloClear) {
            bool anySolo = false;
            const int nTr = CountTracks(nullptr);
            for (int i = 0; i < nTr; ++i) {
                if (GetMediaTrackInfo_Value(GetTrack(nullptr, i), "I_SOLO") > 0.5) {
                    anySolo = true;
                    break;
                }
            }
            pushButtonLed_(btn, anySolo);
            continue;
        }

        device_->send(buildLedWrite(cell.bank, cell.cell,
                                    on ? led::kStateOn : led::kStateOff));
    }

    // Zero the Bus Comp GR readout so stale values from the last track
    // don't linger until the JSFX probe next ticks.
    if (bindings.busCompMap) {
        device_->send(buildZeroGr());
    }

    // Push every mapped knob's LED ring immediately on focus change so
    // the rings reflect the current plugin state without waiting for
    // the user to actually move a knob. Reads the normalized VST3
    // value for each knob that has a ring mapping defined.
    if (tr && bindings.channelMap) {
        for (uint8_t knobId = 0; knobId < 0x20; ++knobId) {
            const int vst3Param = bindings.channelMap->knobParam[knobId];
            if (vst3Param == kParamNone) continue;
            const double v = TrackFX_GetParamNormalized(
                tr, bindings.channelFxIdx, vst3Param);
            const double visual =
                bindings.channelMap->inverted[knobId] ? (1.0 - v) : v;
            pushKnobRing_(knobId, visual);
        }
    }
    if (tr && bindings.busCompMap) {
        for (uint8_t knobId = 0; knobId < 0x20; ++knobId) {
            const int vst3Param = bindings.busCompMap->knobParam[knobId];
            if (vst3Param == kParamNone) continue;
            const double v = TrackFX_GetParamNormalized(
                tr, bindings.busCompFxIdx, vst3Param);
            const double visual =
                bindings.busCompMap->inverted[knobId] ? (1.0 - v) : v;
            pushKnobRing_(knobId, visual);
        }
    }
}

void UC1Surface::pushGainReduction(float dB)
{
    if (!device_) return;
    // Bus Comp meter (FF 5B 02) — UC1Device streams this at 50 Hz on
    // its own; just update the cached value.
    device_->setGainReduction(dB);

    // Channel-Strip Dynamics GR LEDs (5 discrete LEDs at bank=0x01,
    // cells 0x5C..0x60, per-LED brightness with 5 visible steps).
    // Mapping: ~3 dB per LED, 5 steps per LED = 0.6 dB per step.
    // Brightness states from the capture: {0x19, 0x2D, 0x54, 0x99, 0xFF}.
    static const uint8_t kLevels[5] = {0x19, 0x2D, 0x54, 0x99, 0xFF};
    static uint8_t lastStates[5] = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE};

    float gr = dB;
    if (gr < 0) gr = 0;
    const int pos = static_cast<int>(gr / 0.6f);  // 0..24 across 15 dB
    const int active = (pos / 5 > 4) ? 4 : (pos / 5);   // which LED (0..4)
    const int sub    = (pos % 5 > 4) ? 4 : (pos % 5);   // sub-level 0..4

    uint8_t target[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < active; ++i) target[i] = 0xFF;  // fully past this LED → full
    target[active] = (pos == 0 && gr < 0.3f) ? 0x00 : kLevels[sub];
    // LEDs past active stay 0

    for (int i = 0; i < 5; ++i) {
        if (target[i] != lastStates[i]) {
            lastStates[i] = target[i];
            device_->send(buildLedWrite(0x01, static_cast<uint8_t>(0x5C + i), target[i]));
        }
    }
}

void UC1Surface::pushVu(uint8_t meter, uint8_t level)
{
    if (!device_) return;
    device_->send(buildVuMeter(meter, level));
}

void UC1Surface::pushCsVu(float dbInput, float dbOutput)
{
    if (!device_) return;

    // 16 LED thresholds per meter (user's capture brief):
    // "16 Stück je auf: alle aus, -60, -50, -40, -30, -27, -24, -21,
    // -18, -15, -12, -9, -6, -3, -2, -1 und 0". Each LED is plain
    // on/off — no per-LED brightness.
    static constexpr float kDbThreshold[16] = {
        -60.f, -50.f, -40.f, -30.f, -27.f, -24.f, -21.f, -18.f,
        -15.f, -12.f,  -9.f,  -6.f,  -3.f,  -2.f,  -1.f,   0.f,
    };

    // Cell-to-LED mapping is a best-guess from dual_36 capture: 16
    // cells per meter, written in ascending cell-number order during
    // SSL's "all-on" mass refresh at t=30.14. Visually unverified —
    // user can swap cells or mirror the in/out pair after testing.
    static constexpr uint8_t kInputCells[16] = {
        0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x45, 0x50, 0x66,
        0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    };
    static constexpr uint8_t kOutputCells[16] = {
        0x79, 0x7A, 0x7B, 0x7C, 0x87, 0x5B, 0x5C, 0x5D,
        // Only 8 of 16 mapped — remaining 8 unknown without more capture.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    // dB → N = number of LEDs lit (0..16). One LED lights per crossed
    // threshold. At threshold[i] LEDs 0..i-1 are on, LED i lights at
    // threshold[i+1], etc. At threshold[15] (0 dBFS) all 16 are on.
    auto countLeds = [&](float dB) -> int {
        int n = 0;
        for (int i = 0; i < 16; ++i) {
            if (dB >= kDbThreshold[i]) n = i + 1; else break;
        }
        return n;
    };

    auto pushMeter = [&](const uint8_t* cells, float dB,
                         uint8_t (&lastStates)[16]) {
        const int n = countLeds(dB);
        for (int i = 0; i < 16; ++i) {
            if (cells[i] == 0x00) continue;  // unmapped slot
            const uint8_t target = (i < n) ? 0xFF : 0x00;
            if (lastStates[i] != target) {
                lastStates[i] = target;
                device_->send(buildLedWrite(0x02, cells[i], target));
            }
        }
    };

    static uint8_t lastIn[16]  = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
                                   0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE};
    static uint8_t lastOut[16] = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
                                   0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE};
    pushMeter(kInputCells,  dbInput,  lastIn);
    pushMeter(kOutputCells, dbOutput, lastOut);
}

} // namespace uc1
