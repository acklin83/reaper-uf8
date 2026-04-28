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

namespace uc1 {

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

    // Push Channel Strip I/O VU meters. Input + Output in dBFS.
    // Drives the 16-LED Input meter + 16-LED Output meter on UC1
    // using the dB-threshold table from the user's capture brief:
    // LEDs light at -60, -50, -40, -30, -27, -24, -21, -18, -15,
    // -12, -9, -6, -3, -2, -1, 0 dBFS. Each LED has 5 brightness
    // steps for smooth transitions between thresholds (cells 0x5B..
    // observed with states 0x19 / 0x2D / 0x54 / 0x99 / 0xFF).
    void pushCsVu(float dbInput, float dbOutput);

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
    CascadeState computeCascade_(void* tr, const UC1Bindings& bindings);
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

    // --- state ---
    UC1Device* device_ = nullptr;
    void*      focusedTrack_ = nullptr;  // MediaTrack*

    // BC display anchor — null until the BC encoder is used (then it
    // pins). Persists across CHANNEL-encoder scrolling so the BC
    // section keeps showing the same plugin instance. See
    // effectiveBcTrack_() for fallback semantics.
    void*      bcAnchorTrack_ = nullptr;  // MediaTrack*

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
};

} // namespace uc1
