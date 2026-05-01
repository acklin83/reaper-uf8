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

UC1Surface::UC1Surface()
{
    lastButtonLed_.fill(-1);  // -1 = unknown, force first push per button
}

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
    lastButtonLed_.fill(-1);  // re-push every button LED on the new focus
    // Drop the BC-bypass cache too. The next pollBcBypassState_ tick on
    // the new track will refresh the backlight to match its bypass state
    // but won't fire the FF 5C cosmetic — focus change is not a press.
    lastBcBypassed_ = -1;
    refresh();
}

void UC1Surface::invalidateCache()
{
    ringCellCache_.clear();
    lastZone05Text_.clear();  // force the next pushFocusedParamReadout_ to send
    lastButtonLed_.fill(-1);  // re-push every button LED on the next poll
    lastBcBypassed_ = -1;     // re-push BC backlight (without phantom cosmetic)
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

    // Mirror plugin/track state to button LEDs and knob rings every
    // tick. Both have internal dedup, so the cost when idle is just
    // a few REAPER API reads + cache compares — no USB traffic. This
    // is what makes plugin-GUI edits, automation, and preset loads
    // reflect on the surface without waiting for a UC1 input event.
    pollButtonLeds_();
    pollKnobRings_();
    pollBcBypassState_();
    pollGainReduction_();

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

    auto csBindings = lookupBindingsOnTrack(focusedTrack_);
    // BC writes target the BC anchor track (independent of focusedTrack_)
    // so the BC section stays pinned regardless of the user's CS focus.
    void* bcAnchorRaw = effectiveBcTrack_();
    UC1Bindings bcBindings = bcAnchorRaw
        ? ((bcAnchorRaw == focusedTrack_) ? csBindings
                                          : lookupBindingsOnTrack(bcAnchorRaw))
        : UC1Bindings{};
    if (!bcBindings.busCompMap && !csBindings.channelMap) {
        if (logThis) {
            char line[96];
            std::snprintf(line, sizeof(line),
                "UC1 knob 0x%02x delta=%d  (no BC anchor + no CS on focus)\n",
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
    void* writeTrackRaw = focusedTrack_;

    const ControlDomain domain = classifyKnob(ev.id);
    if (domain == ControlDomain::BusComp && bcBindings.busCompMap) {
        map           = bcBindings.busCompMap;
        fxIdx         = bcBindings.busCompFxIdx;
        busCompContext = true;
        writeTrackRaw = bcAnchorRaw;
    } else if (csBindings.channelMap) {
        map   = csBindings.channelMap;
        fxIdx = csBindings.channelFxIdx;
    } else if (bcBindings.busCompMap) {
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

    MediaTrack* tr = static_cast<MediaTrack*>(writeTrackRaw);
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
    // Look up the UF8 plugin on whichever track the write actually went
    // to — for BC knobs that's bcAnchor, not focusedTrack_. Without this
    // a BC knob on a CS-only focused track would skip the focus push.
    auto uf8Match = uf8::lookupPluginOnTrack(writeTrackRaw, uf8Domain);
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
    // BC button targets are routed to the BC anchor (independent of CS
    // focus). For most buttons the CS path stays on focusedTrack_; only
    // the BusCompIn case below needs the anchor.
    void* bcAnchorRaw = effectiveBcTrack_();
    UC1Bindings bcBindings = bcAnchorRaw
        ? ((bcAnchorRaw == focusedTrack_) ? bindings
                                          : lookupBindingsOnTrack(bcAnchorRaw))
        : UC1Bindings{};
    MediaTrack* bcTr = static_cast<MediaTrack*>(bcAnchorRaw);

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
    // CS / BC IN buttons toggle the plug-in's own Bypass param (NOT
    // REAPER's TrackFX_Enabled). Inverted semantic: param=1 means
    // bypassed → IN is OFF, so LED brightness is the inverse of the
    // value we just wrote.
    auto toggleBypassParam = [&](MediaTrack* targetTr, const PluginBindings* m,
                                 int fxIdx, const char* labelLong, int readoutZone) {
        if (!targetTr || !m || m->bypassParam == kParamNone) return false;
        const double cur = TrackFX_GetParamNormalized(targetTr, fxIdx, m->bypassParam);
        const double next = (cur > 0.5) ? 0.0 : 1.0;
        TrackFX_SetParamNormalized(targetTr, fxIdx, m->bypassParam, next);
        const bool inActive = next < 0.5;
        pushButtonLed_(ev.id, inActive);
        pushButtonReadout_(ev.id, labelLong,
                           inActive ? "In" : "Out", readoutZone);
        return true;
    };

    if (ev.id == button::kChannelIn) {
        if (!toggleBypassParam(tr, bindings.channelMap, bindings.channelFxIdx,
                               "Channel Strip", zone::kChannelStripReadout)) {
            // No SSL CS plug-in on the focused track — fall back to
            // bypassing the first track FX (legacy behaviour). Edge
            // case; LED still tracks the new state.
            if (TrackFX_GetCount(tr) > 0) {
                const bool wasEnabled = TrackFX_GetEnabled(tr, 0);
                TrackFX_SetEnabled(tr, 0, !wasEnabled);
                pushButtonLed_(ev.id, !wasEnabled);
                pushButtonReadout_(ev.id, "Channel Strip",
                                   !wasEnabled ? "In" : "Out",
                                   zone::kChannelStripReadout);
            }
        }
        ++stats_.buttonEventsHandled;
        return;
    }
    if (ev.id == button::kBusCompIn) {
        // BC IN targets the BC anchor track, not the CS-focused track.
        toggleBypassParam(bcTr, bcBindings.busCompMap, bcBindings.busCompFxIdx,
                          "Bus Comp", zone::kBusCompReadout);
        ++stats_.buttonEventsHandled;
        return;
    }

    // Plugin-param toggles (EQ In, Dyn In, Fast Attack, etc.). These
    // live on the Channel Strip plugin.
    if (!bindings.channelMap) { ++stats_.buttonEventsSuppressed; return; }
    const int vst3Param = bindings.channelMap->buttonParam[ev.id];
    if (vst3Param == kParamNone) { ++stats_.buttonEventsSuppressed; return; }

    const double cur = TrackFX_GetParamNormalized(tr, bindings.channelFxIdx, vst3Param);

    // EQ Type on 4K E is a 3-state "EQ Colour" cycle (Brown → Black →
    // Orange → Brown). 4K G's EQ Colour param exposes three normalised
    // positions but only two distinct values (Pink at 0.0, Black at
    // 0.5 / 1.0) — user-confirmed 2026-04-30 that 4K G should behave
    // as a 2-way toggle, matching the UF8 V-Pot path. CS 2's EQ Type
    // and 4K B (no EQ Type) are binary or absent.
    const bool is3StateEqColour = (ev.id == button::kEqType)
        && std::strcmp(bindings.channelMap->shortName, "4K E") == 0;

    double next;
    if (is3StateEqColour) {
        int step = static_cast<int>(cur * 2.0 + 0.5);
        if (step < 0) step = 0;
        if (step > 2) step = 2;
        step = (step + 1) % 3;
        next = step * 0.5;
    } else {
        next = (cur < 0.5) ? 1.0 : 0.0;
    }
    TrackFX_SetParamNormalized(tr, bindings.channelFxIdx, vst3Param, next);
    pushButtonLed_(ev.id, next >= 0.5);

    // Project the toggled param onto UF8 so all 8 V-Pots show + control
    // it across the bank — same broadcast model the UC1 knob path uses.
    // Looks up the UF8 PluginMap (separate from uc1::PluginBindings),
    // finds the slot whose vst3Param matches, and sets the focus. UC1
    // zone 0x05 picks this up via pushFocusedParamReadout_ on the next
    // poll tick (and we call it inline so the user sees the new focus
    // immediately rather than after one tick of latency).
    auto uf8Match = uf8::lookupPluginOnTrack(focusedTrack_,
                                             uf8::Domain::ChannelStrip);
    if (uf8Match.map) {
        const int slotIdx = uf8::slotIdxForVst3Param(*uf8Match.map, vst3Param);
        if (slotIdx >= 0) {
            uf8::setFocus({uf8::Domain::ChannelStrip, slotIdx});
            pushFocusedParamReadout_();
        }
    }

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
    const uf8::LinkSlot* slotPtr = uf8::findSlotByLinkIdx(*match.map,
                                                          focused.slotIdx);
    if (!slotPtr) return;  // plugin doesn't expose this Link slot
    const auto& slot = *slotPtr;
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
// Output Gain / Fader Level — full 11-LED ring per user 2026-05-01.
// uc1_32c only swept the visible-active range (4 cells 0x0E..0x11)
// because SSL360 doesn't drive the unused outer cells in a
// param-equivalent sweep; the physical ring is the same 11-LED layout
// every other UC1 knob uses. Cells 0x0C..0x16 byte5=0x01 (the 7-seg
// digit cells at 0x10..0x16 are byte5=0x00 — independent address
// space, no collision).
constexpr uint8_t kFaderLevelCells[] = {0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16};

// ---- Dyn / Gate section (7 knobs) ----
constexpr uint8_t kGateReleaseCells[]   = {0x7C,0x7D,0x7E,0x7F,0x80,0x81,0x82,0x83,0x84,0x85,0x86};
constexpr uint8_t kGateHoldCells[]      = {0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,0x90,0x91};
// 11-LED ring (user 2026-05-01) — earlier 10-cell map missed the CCW LED.
constexpr uint8_t kGateThresholdCells[] = {0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B};
// Gate Range = 11-cell ring 0x66..0x70. The 5 cells BELOW the ring
// (0x61..0x65) are the **Gate GR meter** — the right-hand of the two
// 5-LED GR strips on the Channel Strip Dynamics section (Comp GR is
// the left strip at 0x5C..0x60). Earlier guess that the ring was
// 0x61..0x6B was wrong — the dot bled into the Gate GR strip at
// CCW positions because we were addressing GR cells as ring cells.
constexpr uint8_t kGateRangeCells[]     = {0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70};
constexpr uint8_t kCompReleaseCells[]   = {0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A};
// 11-LED rings (user 2026-05-01) — earlier 10-cell maps missed the CCW LED.
constexpr uint8_t kCompThresholdCells[] = {0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F};
constexpr uint8_t kCompRatioCells[]     = {0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44};

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
    static const RingDef kFaderLevel{kFaderLevelCells, 11, G};
    static const RingDef kGateRelease  {kGateReleaseCells,   11, P};
    static const RingDef kGateHold     {kGateHoldCells,      11, P};
    static const RingDef kGateThr      {kGateThresholdCells, 11, G};
    static const RingDef kGateRange    {kGateRangeCells,     11, P};
    static const RingDef kCompRelease  {kCompReleaseCells,   11, P};
    static const RingDef kCompThr      {kCompThresholdCells, 11, G};
    static const RingDef kCompRatio    {kCompRatioCells,     11, G};
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

UC1Surface::CascadeState UC1Surface::computeCascade_(
    void* csRaw, const UC1Bindings& csBindings,
    void* bcRaw, const UC1Bindings& bcBindings)
{
    CascadeState s{};
    MediaTrack* csTr = static_cast<MediaTrack*>(csRaw);
    MediaTrack* bcTr = static_cast<MediaTrack*>(bcRaw);
    auto readBypass = [](MediaTrack* t, const PluginBindings* m, int fxIdx) -> bool {
        if (!t || !m || m->bypassParam == kParamNone) return false;
        return TrackFX_GetParamNormalized(t, fxIdx, m->bypassParam) > 0.5;
    };
    s.csBypassed = readBypass(csTr, csBindings.channelMap, csBindings.channelFxIdx);
    s.bcBypassed = readBypass(bcTr, bcBindings.busCompMap, bcBindings.busCompFxIdx);
    if (csTr && csBindings.channelMap) {
        const int eqInP = csBindings.channelMap->buttonParam[button::kEqIn];
        if (eqInP != kParamNone) {
            s.eqOff = TrackFX_GetParamNormalized(
                csTr, csBindings.channelFxIdx, eqInP) < 0.5;
        }
        const int dynP = csBindings.channelMap->buttonParam[button::kDynIn];
        if (dynP != kParamNone) {
            s.dynOff = TrackFX_GetParamNormalized(
                csTr, csBindings.channelFxIdx, dynP) < 0.5;
        }
    }
    return s;
}

bool UC1Surface::buttonCascadeDim_(uint8_t btn, const CascadeState& s) const
{
    // Bypass-toggle buttons display their own state — never cascade-dim.
    if (btn == button::kEqIn       || btn == button::kDynIn
     || btn == button::kChannelIn  || btn == button::kBusCompIn) return false;
    // Track / surface buttons aren't plug-in controls — exempt.
    if (btn == button::kSolo       || btn == button::kCut
     || btn == button::kSoloClear  || btn == button::kPolarity
     || btn == button::kFine) return false;
    // EQ subsection: HF/LF Bell + EQ Type
    if (btn == button::kHfBell || btn == button::kEqType
     || btn == button::kLfBell) {
        return s.csBypassed || s.eqOff;
    }
    // DYN subsection: Comp / Gate fast-attack + Peak + Expand + ScListen
    if (btn == button::kFastAttComp || btn == button::kPeak
     || btn == button::kExpand      || btn == button::kFastAttGate
     || btn == button::kScListen) {
        return s.csBypassed || s.dynOff;
    }
    return false;
}

bool UC1Surface::knobCascadeDim_(uint8_t knobId, const CascadeState& s) const
{
    using namespace knob;
    // BC pots (0x0E..0x14: Ratio/ScHpf/Attack/Release/Threshold/Makeup/Mix)
    if (knobId >= kBCRatio && knobId <= kBCMix) return s.bcBypassed;
    // EQ knobs (0x00..0x0B: LP/HP + 4 EQ bands × 3 params)
    if (knobId <= kCSLfGain) return s.csBypassed || s.eqOff;
    // DYN knobs (0x17..0x1D: Gate Release..Comp Ratio)
    if (knobId >= kCSGateRelease && knobId <= kCSCompRatio) {
        return s.csBypassed || s.dynOff;
    }
    // Channel knobs that are still CS plug-in params: Input Trim, Fader.
    if (knobId == kCSInputTrim || knobId == kCSFaderLevel) return s.csBypassed;
    return false;
}

void UC1Surface::pushKnobRing_(uint8_t knobId, double normalized, bool dim)
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

    // Dim mode for the section-bypass cascade: replace 0xFF brightness
    // with 0x33 so the dot is visibly half-bright. Gradient sub-steps
    // (0x19, 0x4C) stay as-is — they're already faded.
    const uint8_t brFull = dim ? 0x33 : 0xFF;

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
        brTarget[mainCell] = brFull;
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
        brTarget[idx] = brFull;
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

void UC1Surface::pushButtonLed_(uint8_t buttonId, LedState state)
{
    if (!device_) return;
    auto cell = cellForButton(buttonId);
    if (cell.bank == 0 && cell.cell == 0) return;  // unmapped

    const bool on = (state != LedState::Off);
    const bool dim = (state == LedState::Dim);

    // Dedup packs the tri-state: 0 = off, 1 = on-bright, 2 = on-dim.
    // pollButtonLeds_ runs every tick so this fires constantly with
    // unchanged values; skip the USB write when nothing changed.
    // invalidateCache() and setFocusedTrack() clear lastButtonLed_ so
    // refreshes re-push.
    if (buttonId < lastButtonLed_.size()) {
        const int8_t want = (state == LedState::Off) ? 0
                          : (state == LedState::Dim) ? 2 : 1;
        if (lastButtonLed_[buttonId] == want) return;
        lastButtonLed_[buttonId] = want;
    }

    auto buildRaw = [](uint8_t bank, uint8_t c, uint8_t b5, uint8_t state) {
        std::vector<uint8_t> f{0xFF, 0x13, 0x04, bank, c, b5, state};
        uint8_t cks = 0;
        for (size_t i = 1; i < f.size(); ++i) cks += f[i];
        f.push_back(cks);
        return f;
    };

    // Right-side Dyn/Gate/SC plugin-param buttons (FastAttComp, Peak,
    // DynIn, Expand, FastAttGate, ScListen) need TWO frames on
    // byte5=0x01 — the dyn-section LED address space:
    //   bank 0x01 = selection bit (0x01 on / 0x00 off)
    //   bank 0x02 = brightness   (0xFF on / 0x00 off)
    // Same dual-bank scheme pushKnobRing_ uses, but with byte5=0x01
    // so the writes don't bleed into EQ-ring rendering (byte5=0x00).
    // Single bank=0x02 frame alone leaves the selection bit clear and
    // the LED stays dark.
    const bool isDynButton =
        buttonId == button::kFastAttComp || buttonId == button::kPeak      ||
        buttonId == button::kDynIn       || buttonId == button::kExpand    ||
        buttonId == button::kFastAttGate || buttonId == button::kScListen;
    if (isDynButton) {
        const uint8_t bri = !on ? 0x00 : (dim ? 0x33 : 0xFF);
        device_->send(buildRaw(0x01, cell.cell, 0x01, on ? 0x01 : 0x00));
        device_->send(buildRaw(0x02, cell.cell, 0x01, bri));
        return;
    }

    // Left-side EQ-section plugin-param buttons (HfBell, EqType, EqIn,
    // LfBell) live in the EQ-section LED address space — byte5=0x00.
    // Cap21/cap22 show SSL360 sending a single bank=0x02 byte5=0x00
    // frame per press, but on our extension that single frame doesn't
    // light the LED (same gap as the dyn buttons before adding the
    // bank=0x01 selection-bit companion). Send the dual-bank pair on
    // byte5=0x00. The cells (0x23, 0x50, 0x51, 0x89) sit in gaps
    // between adjacent EQ rings, so the byte5=0x00 writes don't bleed
    // into any ring rendering.
    const bool isEqButton =
        buttonId == button::kHfBell || buttonId == button::kEqType ||
        buttonId == button::kEqIn   || buttonId == button::kLfBell;
    if (isEqButton) {
        const uint8_t bri = !on ? 0x00 : (dim ? 0x33 : 0xFF);
        device_->send(buildRaw(0x01, cell.cell, 0x00, on ? 0x01 : 0x00));
        device_->send(buildRaw(0x02, cell.cell, 0x00, bri));
        return;
    }

    // Other buttons (Fine, central-section track buttons with Solo/Cut
    // overrides, Solo Clear) keep the single-frame buildLedWrite path
    // — those were empirically verified working with one frame.
    //   - Solo Clear uses bank 0x01 + state 0x01 on / 0x00 off (cap17).
    //   - Solo/Cut/Polarity/ChannelIn/BusCompIn: fan-out test confirmed
    //     the LED lights with bank 0x01 + state 0x01.
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
        bank = 0x01;
        stateOn = 0x01;
    }
    // Dim only meaningful for the kStateOn=0xFF path (the Solo/Cut/etc.
    // 0x01-coded buttons have no firmware-supported dim state); dim
    // requests on those collapse to plain on.
    uint8_t stateByte;
    if (!on)        stateByte = led::kStateOff;
    else if (dim && stateOn == led::kStateOn) stateByte = led::kStateDim;
    else            stateByte = stateOn;

    device_->send(buildLedWrite(bank, cell.cell, stateByte));
}

void UC1Surface::pollButtonLeds_()
{
    if (!device_) return;

    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
    UC1Bindings bindings = tr ? lookupBindingsOnTrack(tr) : UC1Bindings{};
    // BC button LED state reads from the BC anchor (BC section is pinned
    // independent of CS focus).
    void* bcAnchorRaw = effectiveBcTrack_();
    MediaTrack* bcTr = static_cast<MediaTrack*>(bcAnchorRaw);
    UC1Bindings bcBindings = bcAnchorRaw
        ? ((bcAnchorRaw == focusedTrack_) ? bindings
                                          : lookupBindingsOnTrack(bcAnchorRaw))
        : UC1Bindings{};
    const CascadeState cascade = computeCascade_(tr, bindings, bcTr, bcBindings);

    auto ledForParam = [&](const PluginBindings* map, int fxIdx, uint8_t btnId) {
        if (!map || !tr) return false;
        const int p = map->buttonParam[btnId];
        if (p == kParamNone) return false;
        return TrackFX_GetParamNormalized(tr, fxIdx, p) >= 0.5;
    };
    auto stateFor = [&](uint8_t btn, bool on) -> LedState {
        if (!on) return LedState::Off;
        return buttonCascadeDim_(btn, cascade) ? LedState::Dim : LedState::On;
    };

    for (uint8_t btn = 0; btn < 0x20; ++btn) {
        const auto cell = cellForButton(btn);
        if (cell.bank == 0 && cell.cell == 0) continue;

        bool on = false;
        switch (classifyButton(btn)) {
            case ControlDomain::BusComp: {
                // BC IN LED reflects the plug-in's Bypass param: lit when
                // bypass < 0.5 (plug-in active / IN). Falls back to
                // TrackFX_Enabled if a track has BC2 but no bypassParam
                // is registered (shouldn't happen — kept defensively).
                bool bcOn = false;
                if (bcBindings.busCompMap && bcTr) {
                    if (bcBindings.busCompMap->bypassParam != kParamNone) {
                        bcOn = TrackFX_GetParamNormalized(
                            bcTr, bcBindings.busCompFxIdx,
                            bcBindings.busCompMap->bypassParam) < 0.5;
                    } else {
                        bcOn = TrackFX_GetEnabled(bcTr, bcBindings.busCompFxIdx);
                    }
                }
                pushButtonLed_(btn, bcOn);
                continue;
            }
            case ControlDomain::ChannelStrip:
                if (btn == button::kChannelIn) {
                    bool cin = false;
                    if (bindings.channelMap && tr) {
                        if (bindings.channelMap->bypassParam != kParamNone) {
                            cin = TrackFX_GetParamNormalized(
                                tr, bindings.channelFxIdx,
                                bindings.channelMap->bypassParam) < 0.5;
                        } else {
                            cin = TrackFX_GetEnabled(tr, bindings.channelFxIdx);
                        }
                    } else if (tr && TrackFX_GetCount(tr) > 0) {
                        cin = TrackFX_GetEnabled(tr, 0);
                    }
                    pushButtonLed_(btn, cin);
                    continue;
                }
                on = ledForParam(bindings.channelMap, bindings.channelFxIdx, btn);
                break;
        }

        if (btn == button::kFine) on = fineMode_.load(std::memory_order_relaxed);

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

        pushButtonLed_(btn, stateFor(btn, on));
    }
}

void UC1Surface::pollBcBypassState_()
{
    if (!device_) return;
    // BC lives on its own track (typically a bus), independent of the
    // currently-focused track. Use the BC anchor — same source used by
    // the BC carousel + display — so the backlight + cosmetic fire from
    // BC presses regardless of where focus is.
    MediaTrack* tr = static_cast<MediaTrack*>(effectiveBcTrack_());
    if (!tr) {
        lastBcBypassed_ = -1;
        return;
    }
    UC1Bindings b = lookupBindingsOnTrack(tr);
    if (!b.busCompMap || b.busCompMap->bypassParam == kParamNone) {
        lastBcBypassed_ = -1;
        return;
    }
    const bool bypassed = TrackFX_GetParamNormalized(
        tr, b.busCompFxIdx, b.busCompMap->bypassParam) > 0.5;
    const int8_t cur = bypassed ? 1 : 0;
    if (cur == lastBcBypassed_) return;

    // BC mechanical-VU backlight (cap43): binary on/off, FF when enabled
    // / 00 when bypassed. Lives outside the 0x33 dim cascade — has its
    // own cell.
    device_->send(buildLedWrite(0x02, 0x01, bypassed ? 0x00 : 0xFF));

    // FF 5C cosmetic needle-pose (cap45): one frame per real transition.
    // Skip on the unknown→known boot path so we don't fire a phantom
    // "you just toggled" pose at every focus change to a track that's
    // simply already-bypassed or already-enabled.
    if (lastBcBypassed_ != -1) {
        device_->send(buildBcBypassPose(/*entering=*/bypassed));
    }

    lastBcBypassed_ = cur;
}

void UC1Surface::pollGainReduction_()
{
    if (!device_) return;
    // PreSonus VST3 GR-meter standard, exposed by REAPER as a named
    // config parm. Returns a string with the dB value (e.g. "12.345").
    // Documented as "ReaComp + other supported compressors" — SSL Native
    // BC2 / CS2 expose the same readback (verified by user; SSL360
    // itself uses the same host-side mechanism to drive the UC1's
    // mechanical needle in MCU mode). One read per FX present.
    auto readGr = [](MediaTrack* tr, int fxIdx) -> float {
        if (!tr || fxIdx < 0) return 0.0f;
        char buf[64];
        if (!TrackFX_GetNamedConfigParm(tr, fxIdx, "GainReduction_dB",
                                        buf, sizeof(buf))) {
            return 0.0f;  // plug-in doesn't implement the standard
        }
        // String parses as float ("12.345" or similar). Sign convention
        // varies — some plug-ins report negative dB for reduction. Take
        // |value| so pushGainReduction's clamp-positive contract holds.
        const float v = static_cast<float>(std::atof(buf));
        return v < 0 ? -v : v;
    };

    // BC: drives the mechanical analog needle. Source is the BC-anchor
    // track (independent of focus — same as pollBcBypassState_).
    float bcGr = 0.0f;
    MediaTrack* bcTr = static_cast<MediaTrack*>(effectiveBcTrack_());
    if (bcTr) {
        UC1Bindings b = lookupBindingsOnTrack(bcTr);
        if (b.busCompMap) bcGr = readGr(bcTr, b.busCompFxIdx);
    }

    // CS Comp GR: drives the 5-LED Comp strip. Source is the focused
    // track's CS plug-in's combined GainReduction_dB (Comp + Gate
    // contributions blended; in practice the Gate's contribution shows
    // up as a Range-sized spike when gating, so the Comp strip is the
    // closest thing we have to "comp activity" without separate readout).
    float csCompGr = 0.0f;
    MediaTrack* csTr = static_cast<MediaTrack*>(focusedTrack_);
    if (csTr) {
        UC1Bindings b = lookupBindingsOnTrack(csTr);
        if (b.channelMap) csCompGr = readGr(csTr, b.channelFxIdx);
    }

    // CS Gate GR: TODO. SSL CS2 doesn't expose a Gate-only readout via
    // GainReduction_dB; the user's hardware shows Gate GR independently
    // (it lit up alongside the Range knob during dual_35 capture work).
    // Until we find the right data source (separate parmname? Range param
    // value? real-time signal vs Gate-Threshold?), drive at 0 so the
    // strip stays dark — better than mirroring Comp GR onto it.
    float csGateGr = 0.0f;

    pushGainReduction(bcGr, csCompGr, csGateGr);
}

void UC1Surface::pollKnobRings_()
{
    if (!device_) return;
    MediaTrack* tr = static_cast<MediaTrack*>(focusedTrack_);
    UC1Bindings bindings = tr ? lookupBindingsOnTrack(tr) : UC1Bindings{};
    void* bcAnchorRaw = effectiveBcTrack_();
    MediaTrack* bcTr = static_cast<MediaTrack*>(bcAnchorRaw);
    UC1Bindings bcBindings = bcAnchorRaw
        ? ((bcAnchorRaw == focusedTrack_) ? bindings
                                          : lookupBindingsOnTrack(bcAnchorRaw))
        : UC1Bindings{};
    if (!tr && !bcTr) return;
    const CascadeState cascade = computeCascade_(tr, bindings, bcTr, bcBindings);

    auto pushOne = [&](MediaTrack* t, uint8_t knobId,
                       const PluginBindings* m, int fxIdx) {
        if (!t || !m) return;
        const int vst3Param = m->knobParam[knobId];
        if (vst3Param == kParamNone) return;
        const double v = TrackFX_GetParamNormalized(t, fxIdx, vst3Param);
        const double visual = m->inverted[knobId] ? (1.0 - v) : v;
        pushKnobRing_(knobId, visual, knobCascadeDim_(knobId, cascade));
    };

    if (tr && bindings.channelMap) {
        for (uint8_t knobId = 0; knobId < 0x20; ++knobId) {
            pushOne(tr, knobId, bindings.channelMap, bindings.channelFxIdx);
        }
    }
    if (bcTr && bcBindings.busCompMap) {
        for (uint8_t knobId = 0; knobId < 0x20; ++knobId) {
            pushOne(bcTr, knobId, bcBindings.busCompMap, bcBindings.busCompFxIdx);
        }
    }
}

void UC1Surface::refresh()
{
    if (!device_) return;

    auto bindings = focusedTrack_ ? lookupBindingsOnTrack(focusedTrack_) : UC1Bindings{};
    // BC bindings live on the BC anchor (independent of CS focus).
    void* bcAnchorRaw_ = effectiveBcTrack_();
    MediaTrack* bcTr_ = static_cast<MediaTrack*>(bcAnchorRaw_);
    UC1Bindings bcBindings_ = bcAnchorRaw_
        ? ((bcAnchorRaw_ == focusedTrack_) ? bindings
                                           : lookupBindingsOnTrack(bcAnchorRaw_))
        : UC1Bindings{};

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
    // Plugin presence: CS bound to focused track, BC bound to anchor.
    const bool havePlugin = bindings.channelMap || bcBindings_.busCompMap;
    device_->send(buildColourBarEnable(havePlugin));
    const char* label =
        bindings.channelMap   ? bindings.channelMap->shortName :
        bcBindings_.busCompMap ? bcBindings_.busCompMap->shortName :
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
    const CascadeState cascade = computeCascade_(tr, bindings, bcTr_, bcBindings_);
    auto ledForParam = [&](const PluginBindings* map, int fxIdx, uint8_t btnId) {
        if (!map || !tr) return false;
        const int p = map->buttonParam[btnId];
        if (p == kParamNone) return false;
        const double v = TrackFX_GetParamNormalized(tr, fxIdx, p);
        return v >= 0.5;
    };
    auto stateFor = [&](uint8_t btn, bool on) -> LedState {
        if (!on) return LedState::Off;
        return buttonCascadeDim_(btn, cascade) ? LedState::Dim : LedState::On;
    };

    for (uint8_t btn = 0; btn < 0x20; ++btn) {
        const auto cell = cellForButton(btn);
        if (cell.bank == 0 && cell.cell == 0) continue;  // not an LED

        bool on = false;
        switch (classifyButton(btn)) {
            case ControlDomain::BusComp: {
                // BC IN LED reads from BC anchor (BC section pinned).
                bool bcOn = false;
                if (bcBindings_.busCompMap && bcTr_) {
                    if (bcBindings_.busCompMap->bypassParam != kParamNone) {
                        bcOn = TrackFX_GetParamNormalized(
                            bcTr_, bcBindings_.busCompFxIdx,
                            bcBindings_.busCompMap->bypassParam) < 0.5;
                    } else {
                        bcOn = TrackFX_GetEnabled(bcTr_, bcBindings_.busCompFxIdx);
                    }
                }
                pushButtonLed_(btn, bcOn);
                continue;
            }
            case ControlDomain::ChannelStrip:
                if (btn == button::kChannelIn) {
                    // ChannelIn LED — same logic, reads plug-in Bypass.
                    bool cin = false;
                    if (bindings.channelMap && tr) {
                        if (bindings.channelMap->bypassParam != kParamNone) {
                            cin = TrackFX_GetParamNormalized(
                                tr, bindings.channelFxIdx,
                                bindings.channelMap->bypassParam) < 0.5;
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

        // Same dim cascade as pollButtonLeds_ — route through the helper
        // so the dual-bank dim path applies; this fall-through case is
        // for buttons not in the dyn/eq groups but still plug-in params.
        pushButtonLed_(btn, stateFor(btn, on));
    }

    // Zero the Bus Comp GR readout so stale values from the last track
    // don't linger until the JSFX probe next ticks.
    if (bcBindings_.busCompMap) {
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
            pushKnobRing_(knobId, visual, knobCascadeDim_(knobId, cascade));
        }
    }
    // BC knob rings read from the BC anchor track (pinned independent
    // of CS focus).
    if (bcTr_ && bcBindings_.busCompMap) {
        for (uint8_t knobId = 0; knobId < 0x20; ++knobId) {
            const int vst3Param = bcBindings_.busCompMap->knobParam[knobId];
            if (vst3Param == kParamNone) continue;
            const double v = TrackFX_GetParamNormalized(
                bcTr_, bcBindings_.busCompFxIdx, vst3Param);
            const double visual =
                bcBindings_.busCompMap->inverted[knobId] ? (1.0 - v) : v;
            pushKnobRing_(knobId, visual, knobCascadeDim_(knobId, cascade));
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

void UC1Surface::pushGainReduction(float bcGrDb, float csCompGrDb, float csGateGrDb)
{
    if (!device_) return;
    // Bus Comp meter (FF 5B 02) — UC1Device streams this at 50 Hz on
    // its own; just update the cached value with the BC-side dB.
    device_->setGainReduction(bcGrDb);

    // Channel-Strip Dynamics GR LEDs.
    //   Comp GR strip: byte5=0x01, cells 0x5C..0x60  (5 LEDs)
    //   Gate GR strip: byte5=0x01, cells 0x61..0x65  (5 LEDs)
    //
    // Each cell needs a PAIRED write:
    //   bank=0x01 state=0x01 → mark cell as "active in GR group" (selection)
    //   bank=0x02 state=<brightness> → set brightness within the group
    // Without the bank=0x01 selection bit, the bank=0x02 brightness write
    // falls through to a different LED bank (lights the first-CCW LED of
    // Comp Release / Gate Range / Dyn In / Gate Hold). Same pair-write
    // rule as the bank=0x01-required status-register LEDs.
    //
    // Brightness: 5 visible steps {0x19, 0x2D, 0x54, 0x99, 0xFF}, ~3 dB
    // per LED, 0.6 dB per sub-step. Verified from `dual_35_cs_gr_ramp`.
    static const uint8_t kLevels[5] = {0x19, 0x2D, 0x54, 0x99, 0xFF};

    auto stripTargets = [&](float dB, uint8_t (&out)[5]) {
        if (dB < 0) dB = 0;
        const int pos = static_cast<int>(dB / 0.6f);  // 0..24 across 15 dB
        const int active = (pos / 5 > 4) ? 4 : (pos / 5);
        const int sub    = (pos % 5 > 4) ? 4 : (pos % 5);
        for (int i = 0; i < 5; ++i) out[i] = 0;
        for (int i = 0; i < active; ++i) out[i] = 0xFF;
        out[active] = (pos == 0 && dB < 0.3f) ? 0x00 : kLevels[sub];
    };

    // Match SSL360's exact pattern from dual_35 — counted across the
    // whole capture: bank=0x01 fires EXACTLY twice per cell (once at
    // activation, once at deactivation), never on brightness updates.
    // Repeated bank=0x01 state=0x01 emissions on every change destabilise
    // neighbour rings (Comp Release / Gate Range / Dyn In / Gate Hold
    // first-CCW LEDs flicker — user-observed 2026-04-28). Track selection
    // separately from brightness; emit bank=0x01 only on selection edge.
    //
    // Activation order matches dual_35:
    //   1. bank=0x02 cell <new> state=0x00   (preset to dark)
    //   2. bank=0x01 cell <new> state=0x01   (activate)
    //   3. bank=0x02 cell <previous> state=0xFF (lock previous to full)
    //   then ramping bank=0x02 brightness for the active edge.
    auto pushStrip = [&](uint8_t baseCell, const uint8_t (&target)[5],
                         uint8_t (&briCache)[5], uint8_t (&selCache)[5]) {
        bool anyChanged = false;
        for (int i = 0; i < 5; ++i) {
            if (target[i] != briCache[i]) { anyChanged = true; break; }
        }
        if (!anyChanged) return;

        // 1. Emit bank=0x01 ONLY for cells whose activation state changed.
        //    Activation: preset bank=0x02 to 0 first, then bank=0x01 to 1.
        //    Deactivation: just bank=0x01 to 0.
        for (int i = 0; i < 5; ++i) {
            const uint8_t cell = static_cast<uint8_t>(baseCell + i);
            const uint8_t newSel = target[i] ? 0x01 : 0x00;
            if (newSel != selCache[i]) {
                if (newSel == 0x01) {
                    device_->send(buildLedWrite(0x02, cell, 0x00));
                    device_->send(buildLedWrite(0x01, cell, 0x01));
                } else {
                    device_->send(buildLedWrite(0x01, cell, 0x00));
                }
                selCache[i] = newSel;
            }
        }
        // 2. Emit bank=0x02 brightness for every cell whose brightness
        //    changed AND every active cell that needs re-asserting after
        //    a sibling's activation. Simpler: re-emit brightness for all
        //    active cells on any change (matches SSL360's "lock previous
        //    to 0xFF" behaviour for free).
        for (int i = 0; i < 5; ++i) {
            const uint8_t cell = static_cast<uint8_t>(baseCell + i);
            if (target[i] != 0 || briCache[i] != target[i]) {
                device_->send(buildLedWrite(0x02, cell, target[i]));
            }
            briCache[i] = target[i];
        }
    };

    static uint8_t lastCompBri[5] = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE};
    static uint8_t lastGateBri[5] = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE};
    static uint8_t lastCompSel[5] = {0, 0, 0, 0, 0};
    static uint8_t lastGateSel[5] = {0, 0, 0, 0, 0};
    uint8_t compTarget[5];
    uint8_t gateTarget[5];
    stripTargets(csCompGrDb, compTarget);
    stripTargets(csGateGrDb, gateTarget);

    pushStrip(0x5C, compTarget, lastCompBri, lastCompSel);
    pushStrip(0x61, gateTarget, lastGateBri, lastGateSel);
}

void UC1Surface::pushVu(uint8_t meter, uint8_t level)
{
    if (!device_) return;
    device_->send(buildVuMeter(meter, level));
}

void UC1Surface::pushCsVu(float inputL, float inputR,
                          float outputL, float outputR)
{
    if (!device_) return;

    // BC-bypass cascade: when the focused track's BC plug-in is
    // bypassed, silence both meters.
    if (focusedTrack_) {
        UC1Bindings b = lookupBindingsOnTrack(focusedTrack_);
        if (b.busCompMap && b.busCompMap->bypassParam != kParamNone) {
            const double bypass = TrackFX_GetParamNormalized(
                static_cast<MediaTrack*>(focusedTrack_),
                b.busCompFxIdx, b.busCompMap->bypassParam);
            if (bypass > 0.5) {
                inputL = inputR = outputL = outputR = -120.f;
            }
        }
    }

    // Cell map decoded 2026-04-28 from `uc1_13_vu_meters.pcapng`.
    // Each meter is 16 LEDs tall × 2 cells per LED (L+R interleaved):
    //   LED i → L cell = base + 2*i, R cell = base + 2*i + 1
    //   Input  meter: byte5=0x00, base 0xA0  (cells 0xA0..0xBF, 32 total)
    //   Output meter: byte5=0x01, base 0x18  (cells 0x18..0x37, 32 total)
    // bank=0x01, state=0x01 lit / 0x00 off (binary, no brightness).
    //
    // SSL UC1 CS I/O meter LED scale, bottom-to-top, per user spec:
    //   -60, -50, -40, -30, -27, -24, -21, -18, -15, -12, -9, -6, -3, -2, -1, 0 dBFS
    // 16 LEDs, LED 0 = -60 dBFS, LED 15 = 0 dBFS (clip).
    constexpr int kNleds = 16;
    static constexpr float kThreshold[kNleds] = {
        -60.f, -50.f, -40.f, -30.f, -27.f, -24.f, -21.f, -18.f,
        -15.f, -12.f,  -9.f,  -6.f,  -3.f,  -2.f,  -1.f,   0.f,
    };

    constexpr uint8_t kInputBase  = 0xA0;  // byte5=0x00
    constexpr uint8_t kOutputBase = 0x18;  // byte5=0x01

    // Custom frame builder — byte5 is per-meter, can't use buildLedWrite
    // (which hardcodes byte5=0x01).
    auto sendVu = [&](uint8_t cell, uint8_t byte5, uint8_t state) {
        std::vector<uint8_t> f;
        f.reserve(8);
        f.push_back(0xFF);
        f.push_back(0x13);
        f.push_back(0x04);
        f.push_back(0x01);          // bank
        f.push_back(cell);
        f.push_back(byte5);
        f.push_back(state);
        uint32_t sum = 0;
        for (size_t k = 1; k < f.size(); ++k) sum += f[k];
        f.push_back(static_cast<uint8_t>(sum & 0xFF));
        device_->send(std::move(f));
    };

    auto pushMeter = [&](uint8_t base, uint8_t byte5,
                         float dbL, float dbR,
                         uint8_t (&lastL)[kNleds],
                         uint8_t (&lastR)[kNleds]) {
        // Each LED i lights independently per channel when that channel's
        // dB exceeds the LED's threshold. LED 0 (-60 dBFS) lights as
        // soon as audio is present.
        for (int i = 0; i < kNleds; ++i) {
            const uint8_t targetL = (dbL >= kThreshold[i]) ? 0x01 : 0x00;
            const uint8_t targetR = (dbR >= kThreshold[i]) ? 0x01 : 0x00;
            const uint8_t cellL = static_cast<uint8_t>(base + 2 * i);
            const uint8_t cellR = static_cast<uint8_t>(base + 2 * i + 1);
            if (lastL[i] != targetL) {
                lastL[i] = targetL;
                sendVu(cellL, byte5, targetL);
            }
            if (lastR[i] != targetR) {
                lastR[i] = targetR;
                sendVu(cellR, byte5, targetR);
            }
        }
    };

    static uint8_t lastInL[kNleds],  lastInR[kNleds];
    static uint8_t lastOutL[kNleds], lastOutR[kNleds];
    static bool initOnce = false;
    if (!initOnce) {
        for (int i = 0; i < kNleds; ++i) {
            lastInL[i] = lastInR[i] = lastOutL[i] = lastOutR[i] = 0xFE;
        }
        initOnce = true;
    }
    pushMeter(kInputBase,  0x00, inputL,  inputR,  lastInL,  lastInR);
    pushMeter(kOutputBase, 0x01, outputL, outputR, lastOutL, lastOutR);
}

} // namespace uc1
