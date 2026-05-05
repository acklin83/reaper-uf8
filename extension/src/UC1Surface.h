#pragma once
//
// UC1Surface — the REAPER-facing glue that wires UC1Device events to
// track/plugin state and back.
//
// Threading: UC1Device fires knob/button callbacks on its libusb worker
// thread. REAPER API calls must happen on REAPER's main thread. So the
// surface enqueues raw events and exposes poll() — the host (main.cpp)
// calls poll() from a REAPER timer / Run() hook on the main thread.
// All REAPER API calls happen inside poll().
//

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "FocusedParam.h"
#include "UC1Device.h"
#include "UC1PluginMap.h"
#include "UC1Protocol.h"
#include "VirtualNotch.h"

namespace uc1 {

// Central Control Panel mode (UC1 User Guide p.18-21). The Back/Confirm
// buttons + Secondary Encoder change semantics depending on this mode;
// LCD top-label reflects the current mode. TRANSPORT lives outside any
// menu hierarchy — entered from MAIN via Sec-Encoder push.
enum class Uc1Mode : uint8_t {
    Main,
    ExtFuncs,   // Phase B — display only for now, no scroll/adjust UX
    Routing,    // Phase A3 — Sec-Enc cycles SSL CS routing-order chunk attr
    Presets,    // Phase A4 — Sec-Enc scrolls presets, Confirm/push loads
    Transport,  // Phase A2 — Back=Stop, Confirm=Play, Sec-Enc scrubs
};

// Simple host-visible state. Filled by poll(); inspect for debugging.
struct SurfaceStats {
    uint64_t knobEventsHandled    = 0;
    uint64_t buttonEventsHandled  = 0;
    uint64_t knobEventsSuppressed = 0;  // no plugin binding
    uint64_t buttonEventsSuppressed = 0;
};

class UC1Surface {
public:
    UC1Surface();

    // Attach to a UC1Device. Must be called before the device is opened
    // (so the callbacks are in place when events start flowing) OR right
    // after open(). Safe to call multiple times — replaces the prior
    // hookup.
    void attach(UC1Device& device);

    // Set the track whose SSL plugins the UC1 should drive. Pass nullptr
    // to detach (no track focused — UC1 goes idle). Call this from
    // REAPER's SetTrackSelected callback or similar.
    void setFocusedTrack(void* track /*MediaTrack**/);

    // Force a full re-push of every cached LED state on the next refresh.
    // Used to recover from device init-flood overlap at startup: our
    // initial refresh() can race with the firmware's own LED clear that
    // fires shortly after open(), leaving rings in stale positions until
    // the next focus change. Calling this drops the cache so the next
    // refresh writes every cell from scratch.
    void invalidateCache();

    // Accessor so REAPER callbacks (e.g. SetSurfaceMute) can gate their
    // UC1 refreshes on whether the event concerns the focused track.
    void* focusedTrack() const { return focusedTrack_; }
    // Public version of the BC-anchor resolver — used by the
    // multi-instance picker dispatch to know which track to cycle
    // BC instances on (independent of focused track).
    void* bcAnchorTrackPublic() const { return effectiveBcTrack_(); }

    // Drain queued hardware events. Call from REAPER's main thread on a
    // timer (Run() or a deferred action). Returns the number of events
    // handled this tick.
    int poll();

    // Refresh derived state: re-read all plugin params, push current
    // display text and LED states to the UC1. Call after setFocusedTrack
    // or after the user edits plugin params in REAPER.
    void refresh();

    // Push GR values (positive dB of reduction) to the UC1's three GR
    // surfaces independently:
    //   bcGrDb       → BC mechanical needle (FF 5B stream at 50 Hz)
    //   csCompGrDb   → CS Comp 5-LED strip (cells 0x5C..0x60)
    //   csGateGrDb   → CS Gate 5-LED strip (cells 0x61..0x65)
    // Thread-safe; enqueued and flushed on the next send. Safe to call
    // at any rate (clamped internally). Defaults to 0 silence the
    // respective surface.
    void pushGainReduction(float bcGrDb, float csCompGrDb, float csGateGrDb);
    // Convenience: same value to all three meters.
    void pushGainReduction(float dB) { pushGainReduction(dB, dB, dB); }

    // Push a VU level (0..255 byte value) for input/output meter.
    // meter: 0 = input, 1 = output.
    void pushVu(uint8_t meter, uint8_t level);

    // Push Channel Strip stereo I/O VU meters. Each meter is 16 LEDs
    // tall × 2 channels (L + R interleaved at cell pairs), driven by
    // independent dB values per channel:
    //   inputL/inputR  → 16-LED Input  meter (pre-FX, ideally)
    //   outputL/outputR → 16-LED Output meter (post-FX track peak)
    // Bottom LED (LED 0) is "dark padding" — never lit per SSL UC1
    // hardware design. Effective range is LED 1..15.
    // Decoded from `uc1_13_vu_meters.pcapng` (silence / -20 / -10 /
    // -6 / 0 dBFS test stages).
    void pushCsVu(float inputL, float inputR, float outputL, float outputR);

    // For diagnostics.
    SurfaceStats stats() const { return stats_; }
    const std::string& lastError() const { return lastError_; }

private:
    // --- event dispatch (main thread) ---
    void handleKnob_(const KnobEvent& ev);
    void handleButton_(const ButtonEvent& ev);

    // Convert a 6-bit-signed delta click into a 0..1 normalized increment.
    // Sensitivity is tuned so a full sweep takes ~half a pot rotation on
    // the UC1 (~15-20 clicks) without feeling jumpy.
    double clickToDelta_(int8_t delta) const;

    // Push the currently-active knob's value to the right display zone.
    // zone = Bus Comp reader (0x05) for Bus Comp knobs, Channel Strip
    // reader (0x03) for CS knobs.
    void pushKnobReadout_(uint8_t knobId, void* track, int fxIdx,
                          int vst3Param, uint8_t zone,
                          std::string_view label);

    // Push LED-cell state for a button. Three-state version supports
    // the section-bypass cascade — when a section is bypassed, its
    // member LEDs render at half-brightness (kStateDim, 0x33) instead
    // of full bright/off. The bool overload is a convenience for
    // single-button toggles where dim isn't applicable.
    enum class LedState : uint8_t { Off, Dim, On };
    void pushButtonLed_(uint8_t buttonId, LedState state);
    void pushButtonLed_(uint8_t buttonId, bool on) {
        pushButtonLed_(buttonId, on ? LedState::On : LedState::Off);
    }

    // Push a "<label>   <value>" readout to the section LCD after a
    // button toggle, mirroring SSL 360°'s zone-0x03/0x05 transient
    // text. The manual lists this as the "currently selected param
    // name + value" field for both Channel Strip and Bus Comp Mode.
    void pushButtonReadout_(uint8_t buttonId, std::string_view label,
                            std::string_view value, uint8_t zone);

    // The track whose Bus Comp section is shown in the BC carousel /
    // BC slot. Independent of focusedTrack_: the CHANNEL encoder
    // scrolls focusedTrack_ but never moves this; the BC encoder
    // moves only this (REAPER's selection follows as a side-effect,
    // which then updates focusedTrack_ via the SetSurfaceSelected
    // callback). Returns the effective anchor — bcAnchorTrack_ if
    // still in the project, else falls back to the first BC-bearing
    // track in the project (lazy auto-anchor).
    void* effectiveBcTrack_() const;

    // Push the LED ring around a rotary pot. `normalized` is the 0..1
    // VST3 param value. Cell layout per knob is stored in a static
    // table (cap37..cap41); knobs not yet mapped no-op silently.
    // `dim`=true renders the position dot at kStateDim brightness for
    // the section-bypass cascade — rest of the ring stays unlit.
    void pushKnobRing_(uint8_t knobId, double normalized, bool dim = false);

    // Section-bypass cascade. Computed once per poll() before LED
    // pushes; consumed by buttonCascadeDim_/knobCascadeDim_ to override
    // LED brightness on plug-in-param controls when their section is
    // bypassed.
    struct CascadeState {
        bool csBypassed = false;
        bool bcBypassed = false;
        bool eqOff      = false;
        bool dynOff     = false;
    };
    CascadeState computeCascade_(void* csTr, const UC1Bindings& csBindings,
                                 void* bcTr, const UC1Bindings& bcBindings);
    bool buttonCascadeDim_(uint8_t buttonId, const CascadeState&) const;
    bool knobCascadeDim_(uint8_t knobId, const CascadeState&) const;

    // Push the focused-param's value for the focused track to the right
    // section LCD zone (0x03 for ChannelStrip domain, 0x05 for BusComp).
    // Used when an external writer (UF8 Page <->, plugin GUI poll)
    // changes uf8::g_focusedParam, or when setFocusedTrack moves to a
    // new track and the same focused slot now references a different
    // plugin instance with a different value. No-op if domain is None
    // or the focused track lacks a plug-in of the focused domain.
    void pushFocusedParamReadout_();

    // Render the PRESETS-mode LCD content. Selector subscreen shows a
    // CHANNEL STRIP / BUS COMP toggle; Browse subscreen shows the
    // currently-loaded preset name + "..." prev/next indicators.
    // Called on entering PRESETS and on every encoder rotation /
    // submode switch within PRESETS. No-op outside PRESETS.
    void renderPresetsSubscreen_();

    // Render the EXTENDED FUNCTIONS list LCD content. Long-label header
    // for the currently-selected entry + 3-slot triple showing
    // prev/curr/next short labels. Sec-Encoder rotation moves the
    // cursor, BACK exits to MAIN. Adjust-mode (push to enter, rotate
    // to change value) is Phase B.1 — TBD pending capture data.
    void renderExtFuncsSubscreen_();

    // --- state ---
    UC1Device* device_ = nullptr;
    void*      focusedTrack_ = nullptr;  // MediaTrack*

    // BC display anchor — null until the BC encoder is used (then it
    // pins). Persists across CHANNEL-encoder scrolling so the BC
    // section keeps showing the same plugin instance. See
    // effectiveBcTrack_() for fallback semantics.
    void*      bcAnchorTrack_ = nullptr;  // MediaTrack*

    // Central Control Panel mode. Defaults to MAIN on construction;
    // resets to MAIN on focusedTrack change so a new track always lands
    // in the canonical view. Mode transitions update the LCD top label
    // and gate Back/Confirm/Sec-Enc behavior in poll().
    Uc1Mode    mode_ = Uc1Mode::Main;

    // PRESETS sub-mode (decoded uc1_38 2026-05-01). Pressing PRESETS
    // enters the CS/BC selector; pushing the Sec-Encoder confirms the
    // domain and drills into that domain's preset list.
    enum class PresetsSubMode : uint8_t {
        Selector,  // banner 0x03 — choose CS or BC
        Browse,    // banner 0x02 — preset list inside selected domain
    };
    PresetsSubMode presetsSub_ = PresetsSubMode::Selector;
    bool           presetsSelectCs_ = true;  // false = BC selected

    // EXT_FUNCS scroll cursor — index into the kExtFuncs list (defined
    // inline in UC1Surface.cpp). Reset to 0 on every entry to EXT_FUNCS.
    int            extFuncsIdx_ = 0;
    // EXT_FUNCS adjust state. False = scrolling list (encoder rotation
    // cycles cursor); true = parameter is "live" (encoder rotation
    // changes the param value via TrackFX_SetParamNormalized). Toggled
    // by Sec-Encoder push.
    bool           extFuncsActive_ = false;

    // MAIN-mode BC-scroll overlay (decoded uc1_41 2026-05-01). On every
    // BC-encoder detent in MAIN, SSL360 puts the central LCD into a
    // sub-mode (banner 0x01 sub=0x02) with a "BUS COMP 2" header above
    // the BC-track-name carousel. After ~1.5s of no further detents the
    // overlay reverts to plain MAIN (banner 0x01 sub=0x00, header
    // "MAIN"). bcScrollOverlayUntil_ holds the timeout deadline; the
    // bool flag tracks whether a revert frame is still pending so we
    // don't spam it on every poll.
    bool                                  bcScrollOverlayActive_ = false;
    std::chrono::steady_clock::time_point bcScrollOverlayUntil_{};
    // Mirror for CS / channel-encoder scrolling. While active,
    // pushFocusedParamReadout_ suppresses zone 0x03 (CS readout) so
    // the SMALL track-name carousel has the upper LCD area to itself.
    // Set on each Encoder 1 detent; reverts after the timeout.
    bool                                  csScrollOverlayActive_ = false;
    std::chrono::steady_clock::time_point csScrollOverlayUntil_{};

    std::mutex               queueMu_;
    std::deque<ButtonEvent>  buttonQueue_;
    std::deque<KnobEvent>    knobQueue_;

    SurfaceStats      stats_;
    std::string       lastError_;

    // Per-knob "fine" toggle. UC1's Fine button acts as a modifier:
    // when on, knob clicks move the param by 1/4 the normal amount.
    std::atomic<bool> fineMode_{false};

    // Per-button LED state cache for the per-tick poll. -1 = unknown
    // (forces the first push), 0 = off, 1 = on. Indexed by button ID.
    // Cleared by setFocusedTrack/invalidateCache so a focus change or
    // explicit refresh re-pushes everything.
    std::array<int8_t, 0x20> lastButtonLed_;

    // Push every mapped button's LED based on current plugin/track
    // state, deduped against lastButtonLed_. Cheap when nothing changed.
    // Called from poll() so plugin-GUI edits and project-load init
    // both reflect on the LEDs without waiting for a UC1 button press.
    void pollButtonLeds_();

    // Push every mapped knob's ring based on the current normalized
    // VST3 param value. pushKnobRing_'s ringCellCache_ dedup means
    // unchanged rings are no-ops. Called from poll() so plugin-GUI
    // edits, automation, and preset loads reflect on the rings without
    // waiting for the user to actually move a knob.
    void pollKnobRings_();

    // Detect BC-bypass state transitions and fire the matching cosmetic
    // frames: the FF 5C single-shot needle-pose (entering vs. exiting
    // bypass) plus the BC mechanical-VU backlight binary cell
    // (bank=0x02 cell=0x01 byte5=0x01, FF when enabled / 00 when bypassed).
    // Decoded in cap43 + cap45 (2026-04-28). Cheap when nothing changes;
    // dedups against lastBcBypassed_ which stays at -1 until first poll.
    void pollBcBypassState_();
    int8_t lastBcBypassed_ = -1;  // -1=unknown, 0=enabled, 1=bypassed

    // Sample BC + CS gain-reduction from the plug-in via REAPER's
    // PreSonus-VST3-standard host hook (TrackFX_GetNamedConfigParm with
    // parmname="GainReduction_dB", documented for "ReaComp + other
    // supported compressors"). REAPER subscribes to the plug-in's GR
    // readback host-side and surfaces it as a string-valued config parm.
    // Called once per poll() tick; cheap (one API call per FX present).
    // Result is pushed into pushGainReduction(), which drives both the
    // BC mechanical needle (via the 50 Hz FF 5B stream) and the CS Comp
    // GR LED strip.
    void pollGainReduction_();

    // Settle window after focus or anchor change — during this brief
    // interval the GR poll skips its push so the BC mechanical needle
    // doesn't visibly twitch toward a stale/transient reading from the
    // outgoing track. setFocusedTrack and the BC-encoder anchor switch
    // both stamp this. ~250 ms covers REAPER's plug-in-state settle on
    // selection change without holding the meter visibly stale.
    std::chrono::steady_clock::time_point grSettleUntil_;

    // Per-knob LED-ring cell state cache. Each entry packs the last-
    // sent (selection_state | brightness_state << 8) so dedup catches
    // brightness changes too, not just selection. pushKnobRing_ skips
    // cells whose packed target matches the cache; setFocusedTrack /
    // invalidateCache clears so the next refresh re-writes everything.
    std::unordered_map<uint8_t, std::vector<uint16_t>> ringCellCache_;

    // Text-level dedup for zone 0x05 (the unified "currently edited
    // value" readout). poll() recomputes the focused param's readout
    // text every tick; if it matches this cache, the send is skipped.
    // Catches all four causes of value change with one mechanism:
    //   - UC1 knob turn (handled inline by pushFocusedParamReadout_)
    //   - UF8 V-Pot rotation on the focused track (caught by poll-tick recompute)
    //   - Page <-/-> focus shift (text changes -> cache miss -> push)
    //   - Plugin-GUI mouse edit (same as UF8 V-Pot path — value-poll catches it)
    // Empty initial value matches a literal empty readout, but real
    // readouts are always 22+ bytes so the first push always fires.
    std::string lastZone05Text_;
    std::string lastZone03Text_;
    // Cached track-name-triple frames (Small = FF 66 25 02 channel
    // carousel, Large = FF 66 2B 04 BC carousel). refresh() populates
    // them; pushFocusedParamReadout_ replays them as part of the SSL
    // 360° readout burst so both carousels survive the precursor's
    // display-context reset.
    std::vector<uint8_t> lastSmallTripleFrame_;
    std::vector<uint8_t> lastLargeTripleFrame_;
    // Cache the last palette index pushed to the focused-track colour-bar
    // zone. pushFocusedParamReadout_ keeps this in sync with the touched
    // plug-in's track colour so the bar follows the active edit (CS or
    // BC), not just the UC1-selected track.
    int  lastFocusedPalette_ = -1;
    // Per-zone "last value-change" timestamp. SSL 360° fires a zone-
    // invalidate burst (precursor + LARGE triple + FF 66 01 <zone>) ~3s
    // after the user stops turning a CS/BC knob to release the LCD's
    // domain-active layout slot. We mirror that: poll() checks these
    // and emits the invalidate burst when the timeout elapses.
    std::chrono::steady_clock::time_point lastZone03Edit_{};
    std::chrono::steady_clock::time_point lastZone05Edit_{};
};

} // namespace uc1
