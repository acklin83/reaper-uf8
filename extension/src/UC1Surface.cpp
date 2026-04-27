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
#include "FocusedParam.h"  // uf8::setFocus — project UC1 knob turns onto the broadcast UF8 strip
#include "Palette.h"  // uf8::quantize for UC1 focused-track colour
#include "PluginMap.h" // uf8::lookupPluginOnTrack + slotIdxForVst3Param

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
    // Invalidate the ring-cell cache so refresh()'s eager ring loops
    // re-write every cell on the new focus, not just the ones whose
    // target state differs from our last-known state. Without this, a
    // cell that the firmware lit (init flood, previous session, etc.)
    // but our cache thinks is OFF stays stuck until the user rotates
    // the dot through it — manifests as a "hanging" LED in EQ rings.
    ringCellCache_.clear();
    refresh();
}

void UC1Surface::invalidateCache()
{
    ringCellCache_.clear();
    lastZone05Text_.clear();  // force the next pushFocusedParamReadout_ to send
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

    // Per-tick value poll. Catches every cause of focused-param change:
    //   - UF8 Page <-/-> shifted slotIdx (text changes)
    //   - UF8 V-Pot rotation on the focused track (value changes)
    //   - Plugin-GUI mouse edit (value changes)
    //   - REAPER automation moving the param under us (value changes)
    // Internal dedup against lastZone05Text_ skips the USB write when
    // nothing changed, so the cost when idle is just two REAPER API
    // calls + a string compare.
    pushFocusedParamReadout_();

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
    // current BC anchor) that has a plugin targeted by the Bus-Comp
    // section. "Next" = first BC track whose project index is
    // greater than the anchor's; "prev" = first BC track whose
    // project index is smaller. Anchor seeds from the effective BC
    // track so subsequent scrolls advance from where the display
    // currently sits, not from the CS focus.
    if (ev.id == knob::kBcEncoder) {
        static int acc = 0;
        static std::chrono::steady_clock::time_point lastT{};
        int step = stepFromAccumulator(acc, lastT, 3);
        if (step == 0) { ++stats_.knobEventsHandled; return; }
        const int n = CountTracks(nullptr);
        if (n <= 0) return;

        int curIdx = -1;
        if (void* anchor = effectiveBcTrack_()) {
            curIdx = static_cast<int>(GetMediaTrackInfo_Value(
                static_cast<MediaTrack*>(anchor), "IP_TRACKNUMBER")) - 1;
        } else if (focusedTrack_) {
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
            // Anchor BC display BEFORE updating REAPER's selection so
            // the SetSurfaceSelected callback's setFocusedTrack chain
            // sees the new anchor in place.
            bcAnchorTrack_ = tr;
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
    // fall back to Channel Strip. The readout zone is unified to 0x05
    // (kBusCompReadout) regardless of which family the knob writes to —
    // see pushFocusedParamReadout_ for the rationale.
    const PluginBindings* map = nullptr;
    int fxIdx = -1;
    bool busCompContext = false;

    const ControlDomain domain = classifyKnob(ev.id);
    if (domain == ControlDomain::BusComp && bindings.busCompMap) {
        map           = bindings.busCompMap;
        fxIdx         = bindings.busCompFxIdx;
        busCompContext = true;
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

    // Project the focused-param onto UF8: turning a UC1 knob makes the
    // touched parameter the new focused param across the bank. We look
    // up UF8's PluginMap (separate types from uc1::PluginBindings even
    // though both describe the same on-track plug-in) for the same
    // domain, then find the slot whose vst3Param matches what UC1 just
    // wrote. If no slot maps (e.g. UC1 knob writes a param UF8's slot
    // list doesn't expose) we leave the focus untouched.
    //
    // Discriminator is busCompContext (= "we actually used the BC map"),
    // NOT the raw `domain` from classifyKnob(): a BC knob on a track
    // without BC2 falls through to the CS map (line 357 above) and the
    // resulting write is a CS-domain operation — so the focus must
    // mirror CS, otherwise we'd write Domain::BusComp + a CS-derived
    // slotIdx and trigger a render against the wrong plug-in family.
    const auto uf8Domain = busCompContext
        ? uf8::Domain::BusComp
        : uf8::Domain::ChannelStrip;
    auto uf8Match = uf8::lookupPluginOnTrack(focusedTrack_, uf8Domain);
    bool focusedParamRendered = false;
    if (uf8Match.map) {
        const int slotIdx = uf8::slotIdxForVst3Param(*uf8Match.map, vst3Param);
        if (slotIdx >= 0) {
            uf8::setFocus({uf8Domain, slotIdx});
            // Unified readout: same code path as poll-tick value polling
            // and Page <-/-> external focus changes. Dedup cache inside
            // pushFocusedParamReadout_ ensures we don't double-push when
            // the next poll() tick runs immediately after this.
            pushFocusedParamReadout_();
            focusedParamRendered = true;
        }
    }
    if (!focusedParamRendered) {
        // Knob wrote a vst3Param outside UF8's slot list (or the track
        // has no UF8-recognised plugin for this domain). Fall back to a
        // direct per-knob readout so the user still sees their edit;
        // skips the dedup cache (next poll-tick will re-render the
        // focused param's text, overwriting this transient).
        pushKnobReadout_(ev.id, tr, fxIdx, vst3Param,
                         zone::kBusCompReadout,
                         labelForKnob(ev.id, busCompContext));
    }
    // Pass the visual position (flipped when the pot is inverted) so
    // the LED ring goes CW when the pot goes CW — independent of which
    // way the VST3 param value moves.
    const double visual = map->inverted[ev.id] ? (1.0 - next) : next;
    pushKnobRing_(ev.id, visual);

    // (Old defensive 7-seg repaint after FaderLevel/BCMix/BCRelease ring
    // moves removed — uc1_31/32 confirmed those rings actually live on
    // byte5=0x01 while the 7-seg writes on byte5=0x00, so the cell
    // numbers collide but the LEDs do not.)

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
            pushButtonReadout_(ev.id, "Fine", next ? "On" : "Off",
                               zone::kChannelStripReadout);
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
            pushButtonReadout_(button::kSolo, "Solo", on ? "On" : "Off",
                               zone::kChannelStripReadout);
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
            pushButtonReadout_(button::kCut, "Cut", on ? "On" : "Off",
                               zone::kChannelStripReadout);
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
            pushButtonReadout_(button::kSoloClear, "Solo Clear",
                               anySolo() ? "On" : "Off",
                               zone::kChannelStripReadout);
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
        pushButtonReadout_(ev.id, "Polarity", !cur ? "In" : "Out",
                           zone::kChannelStripReadout);
        ++stats_.buttonEventsHandled;
        return;
    }

    // Channel IN — two modes:
    //   * SSL Channel Strip on track: toggle the plugin's internal
    //     "Channel In" switch (found by VST3 param name). This mirrors
    //     the plugin's own IN button, not the global bypass.
    //   * No SSL plugin: fall back to bypassing the first track FX.
    if (ev.id == button::kChannelIn) {
        bool newOn = false;
        bool acted = false;
        if (bindings.channelMap) {
            const int p = channelInParam_(tr, bindings.channelFxIdx);
            if (p >= 0) {
                const double cur = TrackFX_GetParamNormalized(
                    tr, bindings.channelFxIdx, p);
                const double next = (cur > 0.5) ? 0.0 : 1.0;
                TrackFX_SetParamNormalized(
                    tr, bindings.channelFxIdx, p, next);
                newOn = next > 0.5;
                acted = true;
            } else {
                // Param-by-name lookup failed — degrade gracefully to
                // plugin-bypass so the button still does *something*.
                const bool wasEnabled = TrackFX_GetEnabled(
                    tr, bindings.channelFxIdx);
                TrackFX_SetEnabled(tr, bindings.channelFxIdx, !wasEnabled);
                newOn = !wasEnabled;
                acted = true;
            }
        } else {
            // No SSL plugin — bypass the first track FX if any.
            if (TrackFX_GetCount(tr) > 0) {
                const bool wasEnabled = TrackFX_GetEnabled(tr, 0);
                TrackFX_SetEnabled(tr, 0, !wasEnabled);
                newOn = !wasEnabled;
                acted = true;
            }
        }
        if (acted) {
            pushButtonLed_(ev.id, newOn);
            pushButtonReadout_(ev.id, "Channel Strip",
                               newOn ? "In" : "Out",
                               zone::kChannelStripReadout);
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
        pushButtonReadout_(ev.id, "Bus Comp",
                           !wasEnabled ? "In" : "Out",
                           zone::kBusCompReadout);
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

    // Push the post-toggle readout. Most CS buttons are binary
    // In/Out toggles; EQ Type is the exception (cycles colours/bell
    // shapes). Use the plugin's formatted string when it produces
    // something more descriptive than "0"/"1", otherwise fall back
    // to "In"/"Out".
    auto labelFor = [](uint8_t id) -> const char* {
        switch (id) {
            case button::kHfBell:      return "HF Bell";
            case button::kEqType:      return "EQ Type";
            case button::kEqIn:        return "EQ";
            case button::kLfBell:      return "LF Bell";
            case button::kFastAttComp: return "Fast Attack";
            case button::kPeak:        return "Peak";
            case button::kDynIn:       return "DYN";
            case button::kExpand:      return "Expander";
            case button::kFastAttGate: return "Fast Attack";
            case button::kScListen:    return "S/C Listen";
        }
        return "";
    };
    char fmtBuf[64] = {0};
    std::string valueText;
    if (TrackFX_FormatParamValueNormalized(tr, bindings.channelFxIdx,
                                           vst3Param, next,
                                           fmtBuf, sizeof(fmtBuf))
        && fmtBuf[0])
    {
        valueText = fmtBuf;
        // Plugins commonly format binary params as "0"/"1"; those
        // read better as "Out"/"In" on the LCD.
        if (valueText == "0") valueText = "Out";
        else if (valueText == "1") valueText = "In";
    } else {
        valueText = (next > 0.5) ? "In" : "Out";
    }
    // S/C Listen is an "On/Off" toggle, not "In/Out".
    if (ev.id == button::kScListen) {
        valueText = (next > 0.5) ? "On" : "Off";
    }
    pushButtonReadout_(ev.id, labelFor(ev.id), valueText,
                       zone::kChannelStripReadout);
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

void* UC1Surface::effectiveBcTrack_() const
{
    // Validate that bcAnchorTrack_ still references a live track.
    // REAPER reuses MediaTrack* pointers across project edits, but a
    // deleted track returns IP_TRACKNUMBER < 0; ValidatePtr2 is the
    // canonical "is this still a track?" check.
    if (bcAnchorTrack_ && ValidatePtr2(nullptr, bcAnchorTrack_, "MediaTrack*")) {
        return bcAnchorTrack_;
    }
    // Lazy fallback: first BC-bearing track in the project. Means the
    // BC carousel shows something useful even before the user touches
    // the BC encoder.
    const int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(nullptr, i);
        if (lookupBindingsOnTrack(t).busCompMap) return t;
    }
    return nullptr;
}

void UC1Surface::pushButtonReadout_(uint8_t /*buttonId*/, std::string_view label,
                                    std::string_view value, uint8_t zone)
{
    if (!device_) return;
    auto readout = formatReadout(label, value);
    device_->send(buildDisplayText(zone, readout, readout.size()));
}

void UC1Surface::pushFocusedParamReadout_()
{
    if (!device_ || !focusedTrack_) return;
    const auto focused = uf8::getFocusedParam();
    if (focused.domain == uf8::Domain::None) return;

    auto match = uf8::lookupPluginOnTrack(focusedTrack_, focused.domain);
    if (!match.map) return;
    if (focused.slotIdx < 0
        || static_cast<size_t>(focused.slotIdx) >= match.map->slots.size())
    {
        return;
    }

    const auto& slot = match.map->slots[focused.slotIdx];
    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
    char formatted[64] = {};
    const double cur = TrackFX_GetParamNormalized(tr, match.fxIndex,
                                                  slot.vst3Param);
    TrackFX_FormatParamValueNormalized(tr, match.fxIndex, slot.vst3Param,
                                       cur, formatted, sizeof(formatted));
    std::string value = stripUnitIfNonNumeric(compactUnit(formatted));
    auto readout = formatReadout(slot.name, value);

    // Dedup: poll() calls this every tick at 30 Hz; skip the USB write
    // when the rendered text didn't change. Same cache also dedups
    // against handleKnob_'s direct call after a UC1 knob turn — the
    // setFocus + pushFocusedParamReadout_ path lands here, fills the
    // cache, and the next poll-tick's recompute is a no-op.
    if (readout == lastZone05Text_) return;
    lastZone05Text_ = readout;

    // Unified readout zone — Channel-Strip params and Bus-Comp params
    // both render here, regardless of which UF8/UC1 control sourced
    // the change. Matches SSL UC1's user-facing "currently edited
    // value" display convention. Zone 0x03 stays reserved for button
    // status text (S/C Listen On, Solo On, ...).
    device_->send(buildDisplayText(zone::kBusCompReadout, readout, readout.size()));
}

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
// Pot LED ring cell maps. ALL UC1 pot rings render as a single moving
// LED dot — the "Kerbe" of the analog knob. No fill/trace, regardless
// of the underlying parameter (gain, frequency, threshold, etc.). This
// is what SSL 360° does natively for the FREQ + Q knobs and what we
// want for every pot.
//
// Cell map source: uc1_15_knob_channelstrip_sweep.pcapng (full CS+Dyn
// sweep, 22 knobs), dual_40_bc_pots.pcapng + uc1_04..07 (BC pots).
// Per-knob attribution by correlating EP 0x81 IN knob events
// (`FF 24 02 <id> <delta>`) with EP 0x02 OUT LED writes, midpoint-
// bucketed by knob-id transitions.
// Two render modes for the LED ring:
//   Position — single bright dot at the LED nearest the value (default).
//   Gradient — single dot on bank 0x01, with bank 0x02 brightness fading
//              over 3 steps {0xFF, 0x4C, 0x19} to neighbouring cells.
//              Used by the bipolar pots whose physical LEDs visibly fade
//              between positions: all 4 EQ Gains, Comp Threshold, Gate
//              Threshold, BC Threshold/Makeup/Mix, Input/Output Gain,
//              Comp Ratio. Decoded from uc1_28/29/31/32 captures
//              showing bank-0x02 states {0x00, 0x19, 0x4C, 0xFF}.
enum class RingMode : uint8_t { Position = 0, Gradient = 1 };
struct RingDef { const uint8_t* cells; int nCells; RingMode mode = RingMode::Position; };

// ---- EQ section (12 knobs) ----
// All rings contiguous 11 cells. Earlier guess that the centre cell of
// each Gain pot was a separately-driven 0-dB indicator was wrong — the
// user reported the 12 o'clock LED missing on all four Gains after we
// excluded those cells. The capture happened to miss writes to those
// cells (sweep timing artefact), but they ARE part of the ring.
constexpr uint8_t kLpfCells[]    = {0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F};
constexpr uint8_t kHpfCells[]    = {0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,0x90,0x91,0x92,0x93,0x94};
constexpr uint8_t kHfGainCells[] = {0x7E,0x7F,0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88};
constexpr uint8_t kHfFreqCells[] = {0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D};
constexpr uint8_t kHmfGainCells[]= {0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70,0x71,0x72};
constexpr uint8_t kHmfFreqCells[]= {0x5D,0x5E,0x5F,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67};
constexpr uint8_t kHmfQCells[]   = {0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C};
constexpr uint8_t kLmfGainCells[]= {0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F};
constexpr uint8_t kLmfFreqCells[]= {0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44};
constexpr uint8_t kLmfQCells[]   = {0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39};
constexpr uint8_t kLfFreqCells[] = {0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E};
constexpr uint8_t kLfGainCells[] = {0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22};

// ---- Channel Strip I/O (Input Trim + Fader Level / Output Gain) ----
// uc1_32: Input Trim — 11 contiguous cells, 3-step brightness, byte5=0x00.
constexpr uint8_t kInputTrimCells[]  = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA};
// Fader Level / Output Gain — knob 0x16, byte5=0x01. Confirmed in
// uc1_32c full sweep: only 4 ring LEDs (cells 0x0E..0x11), each with
// 3-step brightness {0x19, 0x4C, 0xFF} on bank 0x02. Old guess of 10
// cells (0x0E..0x17) was wrong.
constexpr uint8_t kFaderLevelCells[] = {0x0E,0x0F,0x10,0x11};

// ---- Dyn / Gate section (7 knobs) ----
constexpr uint8_t kGateReleaseCells[]   = {0x7C,0x7D,0x7E,0x7F,0x80,0x81,0x82,0x83,0x84,0x85,0x86};
constexpr uint8_t kGateHoldCells[]      = {0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,0x90,0x91};
constexpr uint8_t kGateThresholdCells[] = {0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B};
// Gate Range = 11-cell ring 0x66..0x70. The 5 cells BELOW the ring
// (0x61..0x65) are the **Gate GR meter** — the right-hand of the two
// 5-LED GR strips on the Channel Strip Dynamics section (Comp GR is
// the left strip at 0x5C..0x60). Earlier guess that the ring was
// 0x61..0x6B was wrong — the dot bled into the Gate GR strip at
// CCW positions because we were addressing GR cells as ring cells.
constexpr uint8_t kGateRangeCells[]     = {0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70};
constexpr uint8_t kCompReleaseCells[]   = {0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A};
constexpr uint8_t kCompThresholdCells[] = {0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F};
constexpr uint8_t kCompRatioCells[]     = {0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44};

// ---- Bus Comp section (7 knobs) ----
// Cell maps confirmed in uc1_31 (2026-04-26). All BC pots use byte5=0x00
// EXCEPT BC Mix which uses byte5=0x01 (a quirk — Mix shares the address
// space with Dyn/Gate rings rather than the rest of BC).
constexpr uint8_t kBcRatioCells[]    = {0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC};
constexpr uint8_t kBcScHpfCells[]    = {0xCB,0xCC,0xCD,0xCE,0xCF,0xD0,0xD1,0xD2,0xD3,0xD4,0xD5};
constexpr uint8_t kBcAttackCells[]   = {0xDD,0xDE,0xDF,0xE0,0xE1,0xE2,0xE3};
// BC Release: 6 cells, NO byte-boundary wrap to 0x00 — old guess was wrong.
constexpr uint8_t kBcReleaseCells[]  = {0xFA,0xFB,0xFC,0xFD,0xFE,0xFF};
constexpr uint8_t kBcThresholdCells[]= {0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED};
// BC Makeup — full 10-cell ring per uc1_31 (was 5 in old guess).
constexpr uint8_t kBcMakeupCells[]   = {0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9};
// BC Mix — byte5=0x01, 9 cells 0x03..0x0B (uc1_31b full sweep). Old
// guess including 0x0C was wrong.
constexpr uint8_t kBcMixCells[]      = {0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B};

const RingDef* ringFor(uint8_t knobId)
{
    constexpr auto P = RingMode::Position;
    constexpr auto G = RingMode::Gradient;
    static const RingDef kLpf       {kLpfCells,        11, P};
    static const RingDef kHpf       {kHpfCells,        11, P};
    static const RingDef kHfGain    {kHfGainCells,     11, G};
    static const RingDef kHfFreq    {kHfFreqCells,     11, P};
    static const RingDef kHmfGain   {kHmfGainCells,    11, G};
    static const RingDef kHmfFreq   {kHmfFreqCells,    11, P};
    static const RingDef kHmfQ      {kHmfQCells,       11, P};
    static const RingDef kLmfGain   {kLmfGainCells,    11, G};
    static const RingDef kLmfFreq   {kLmfFreqCells,    11, P};
    static const RingDef kLmfQ      {kLmfQCells,       11, P};
    static const RingDef kLfFreq    {kLfFreqCells,     11, P};
    static const RingDef kLfGain    {kLfGainCells,     11, G};
    static const RingDef kInputTrim {kInputTrimCells,  11, G};
    static const RingDef kFaderLevel{kFaderLevelCells,  4, G};
    static const RingDef kGateRelease  {kGateReleaseCells,   11, P};
    static const RingDef kGateHold     {kGateHoldCells,      11, P};
    static const RingDef kGateThr      {kGateThresholdCells, 10, G};
    static const RingDef kGateRange    {kGateRangeCells,     11, P};
    static const RingDef kCompRelease  {kCompReleaseCells,   11, P};
    static const RingDef kCompThr      {kCompThresholdCells, 10, G};
    static const RingDef kCompRatio    {kCompRatioCells,     10, G};
    static const RingDef kBcRatio   {kBcRatioCells,     7, P};
    static const RingDef kBcScHpf   {kBcScHpfCells,    11, P};
    static const RingDef kBcAttack  {kBcAttackCells,    7, P};
    static const RingDef kBcRelease {kBcReleaseCells,   6, P};
    static const RingDef kBcThr     {kBcThresholdCells,10, G};
    static const RingDef kBcMakeup  {kBcMakeupCells,   10, G};
    static const RingDef kBcMix     {kBcMixCells,       9, G};

    switch (knobId) {
        // EQ
        case knob::kCSLowPass:    return &kLpf;
        case knob::kCSHighPass:   return &kHpf;
        case knob::kCSHfGain:     return &kHfGain;
        case knob::kCSHfFreq:     return &kHfFreq;
        case knob::kCSHmfGain:    return &kHmfGain;
        case knob::kCSHmfFreq:    return &kHmfFreq;
        case knob::kCSHmfQ:       return &kHmfQ;
        case knob::kCSLmfGain:    return &kLmfGain;
        case knob::kCSLmfFreq:    return &kLmfFreq;
        case knob::kCSLmfQ:       return &kLmfQ;
        case knob::kCSLfFreq:     return &kLfFreq;
        case knob::kCSLfGain:     return &kLfGain;
        // Channel Strip I/O
        case knob::kCSInputTrim:      return &kInputTrim;
        case knob::kCSFaderLevel:     return &kFaderLevel;
        // Dyn / Gate
        case knob::kCSGateRelease:    return &kGateRelease;
        case knob::kCSGateHold:       return &kGateHold;
        case knob::kCSGateThreshold:  return &kGateThr;
        case knob::kCSGateRange:      return &kGateRange;
        case knob::kCSCompRelease:    return &kCompRelease;
        case knob::kCSCompThreshold:  return &kCompThr;
        case knob::kCSCompRatio:      return &kCompRatio;
        // Bus Comp
        case knob::kBCRatio:          return &kBcRatio;
        case knob::kBCScHpf:          return &kBcScHpf;
        case knob::kBCAttack:         return &kBcAttack;
        case knob::kBCRelease:        return &kBcRelease;
        case knob::kBCThreshold:      return &kBcThr;
        case knob::kBCMakeup:         return &kBcMakeup;
        case knob::kBCMix:            return &kBcMix;
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

    // Per-knob state cache so we only push cells that changed.
    // Each entry packs sel | (brightness << 8) so we dedup BOTH
    // banks together. 0xFFFF = sentinel "unset", forces a write
    // since neither sel=0/1 nor any real brightness can match.
    auto& last = ringCellCache_[knobId];
    if (static_cast<int>(last.size()) != def->nCells) {
        last.assign(def->nCells, 0xFFFF);
    }

    int idx = 0;
    std::vector<uint8_t> target(def->nCells, 0);
    std::vector<uint8_t> brTarget(def->nCells, 0);

    if (def->mode == RingMode::Gradient) {
        // x3 resolution: each LED-to-LED transition has 3 visual
        // sub-steps. The current LED stays FULL while the next LED
        // (in the direction of rotation) fades up through 0x19 →
        // 0x4C → 0xFF, then the dot snaps to that LED and the cycle
        // repeats. 11 LEDs × 3 sub-steps = 31 distinct visual
        // positions.
        const int subMax = (def->nCells - 1) * 3;
        int k = static_cast<int>(normalized * subMax + 0.5);
        if (k < 0) k = 0;
        if (k > subMax) k = subMax;
        const int mainCell = k / 3;
        const int frac     = k % 3;
        idx = mainCell;
        target[mainCell] = 1;
        brTarget[mainCell] = 0xFF;
        if (frac > 0 && mainCell + 1 < def->nCells) {
            // 1 → 0x19, 2 → 0x4C
            brTarget[mainCell + 1] = (frac == 1) ? 0x19 : 0x4C;
        }
    } else {
        // Position-only: continuous f rounded to nearest LED.
        const double f = normalized * (def->nCells - 1);
        idx = static_cast<int>(f + 0.5);
        if (idx < 0) idx = 0;
        if (idx >= def->nCells) idx = def->nCells - 1;
        target[idx] = 1;
        brTarget[idx] = 0xFF;
    }

    // Dual-bank encoding per cell. Bank 0x01 = selection 0/1, bank 0x02
    // = brightness 0/FF.
    //
    // **byte5 (the "role" byte) is section-dependent**, decoded from
    // SSL360 captures uc1_28 + uc1_29 + dual_40:
    //   - EQ pots (knobs 0x00..0x0B) → byte5 = 0x00
    //   - Input Trim (0x0C) → byte5 = 0x00
    //   - BC pots (knobs 0x0E..0x13) → byte5 = 0x00
    //   - BC Mix (knob 0x14) → byte5 = 0x01 (quirk — Mix is in the
    //     same address space as Dyn/Gate, not the rest of BC)
    //   - Fader Level / Output Gain (knob 0x16) → byte5 = 0x01
    //   - Dyn/Gate pots (knobs 0x17..0x1D) → byte5 = 0x01
    // The two byte5 values address distinct LED groups on bank 0x01/0x02
    // — they are NOT the same physical LEDs, even when cell numbers
    // overlap. Writing the wrong byte5 hits a non-displayed register.
    const uint8_t b5 = (knobId == knob::kBCMix
                        || knobId == knob::kCSFaderLevel
                        || (knobId >= 0x17 && knobId <= 0x1D)) ? 0x01 : 0x00;
    auto make = [b5](uint8_t bank, uint8_t cell, uint8_t state) {
        std::vector<uint8_t> f;
        f.reserve(8);
        f.push_back(0xFF);
        f.push_back(0x13);
        f.push_back(0x04);
        f.push_back(bank);
        f.push_back(cell);
        f.push_back(b5);
        f.push_back(state);
        uint32_t sum = 0;
        for (size_t k = 1; k < f.size(); ++k) sum += f[k];
        f.push_back(static_cast<uint8_t>(sum & 0xFF));
        return f;
    };
    // Dedup both selection AND brightness via the packed cache key.
    // Cells unchanged across both banks skip both writes — a 22-frame
    // Gradient-mode push collapses to ~2-4 frames per knob tick once
    // the dot has moved one cell.
    for (int i = 0; i < def->nCells; ++i) {
        const uint8_t selState = target[i] ? 0x01 : 0x00;
        const uint16_t want = static_cast<uint16_t>(selState)
                            | (static_cast<uint16_t>(brTarget[i]) << 8);
        if (last[i] == want) continue;
        last[i] = want;
        const uint8_t cell = def->cells[i];
        device_->send(make(0x01, cell, selState));
        device_->send(make(0x02, cell, brTarget[i]));
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
    // BC slot uses the BC anchor (independent of CS focus, persists
    // across CHANNEL-encoder scrolling). Falls back gracefully when
    // no BC track exists in the project.
    std::string csName = resolveTrackName();
    void* bcAnchor = effectiveBcTrack_();
    std::string bcName;
    if (bcAnchor) {
        MediaTrack* bcTr = static_cast<MediaTrack*>(bcAnchor);
        char nameBuf[128] = {0};
        if (GetSetMediaTrackInfo_String(bcTr, "P_NAME", nameBuf, false)
            && nameBuf[0])
        {
            bcName = nameBuf;
        } else {
            int idx = static_cast<int>(GetMediaTrackInfo_Value(bcTr, "IP_TRACKNUMBER"));
            char fallback[32];
            std::snprintf(fallback, sizeof(fallback), "Track %d", idx);
            bcName = fallback;
        }
    }

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
    // BC carousel: 3-slot [prev, curr, next] across BC-bearing tracks
    // ONLY (skips non-BC tracks). Mirrors the CS carousel's UX but
    // anchored on the BC focus. Curr is the BC anchor; prev/next are
    // the BC-bearing tracks immediately before/after it in project
    // order. Slots are empty when no BC track exists in that
    // direction.
    std::vector<int> bcIndices;
    {
        const int n = CountTracks(nullptr);
        bcIndices.reserve(n);
        for (int i = 0; i < n; ++i) {
            MediaTrack* t = GetTrack(nullptr, i);
            if (lookupBindingsOnTrack(t).busCompMap) bcIndices.push_back(i);
        }
    }
    int bcAnchorProjIdx = -1;
    if (bcAnchor) {
        bcAnchorProjIdx = static_cast<int>(GetMediaTrackInfo_Value(
            static_cast<MediaTrack*>(bcAnchor), "IP_TRACKNUMBER")) - 1;
    }
    int bcRank = -1;
    for (size_t i = 0; i < bcIndices.size(); ++i) {
        if (bcIndices[i] == bcAnchorProjIdx) { bcRank = static_cast<int>(i); break; }
    }
    auto bcNameAtRank = [&](int rank) -> std::string {
        if (rank < 0 || rank >= static_cast<int>(bcIndices.size())) return "";
        return nameOfIdx(bcIndices[rank]);
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
        bcNameAtRank(bcRank - 1),
        bcNameAtRank(bcRank),
        bcNameAtRank(bcRank + 1)));

    // 7-segment push moved to the end of refresh() — see below. Several
    // knob ring cell maps (FaderLevel, BC Mix, BC Release) overlap the
    // 7-seg ones/tens/hundreds cells on bank 0x01, so the 7-seg has to
    // be the LAST writer in this function for focus-change to land
    // legibly. Pushing it here would just be overwritten by the eager
    // ring loops.

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
    //
    // No more knob exclusions: byte5 routing in pushKnobRing_ now
    // separates EQ/BC (byte5=0x00) from Dyn/Gate + BC Mix + Fader Level
    // (byte5=0x01), so cell numbers can collide with the 7-seg cells
    // without sharing physical LEDs.
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
    // BC knob rings only fire when the focused track itself has a BC
    // plugin (so we never dirty the carousel-anchor's section).
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

    // 7-segment position indicator — show the REAPER track number
    // (1-based) on the central red display. Push order doesn't matter
    // any more for safety (rings on byte5=0x01 won't touch this
    // address space), but kept here at the end for clarity.
    if (focusedTrack_) {
        int idx = static_cast<int>(GetMediaTrackInfo_Value(
            static_cast<MediaTrack*>(focusedTrack_), "IP_TRACKNUMBER"));
        if (idx < 0) idx = 0;
        for (const auto& frame : buildSevenSeg(static_cast<unsigned int>(idx))) {
            device_->send(frame);
        }
    }

    // Focused-param readout for the new track. The focus value itself
    // hasn't changed (track-switch leaves slotIdx + domain alone), but
    // the displayed value depends on the track's plug-in instance, so
    // recompute + push (dedup cache handles the text-equal case).
    pushFocusedParamReadout_();
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
