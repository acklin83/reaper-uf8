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

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Push a GR value (positive dB of reduction) to the UC1 Bus Comp
    // meter. Thread-safe; enqueued and flushed on the next send. Safe to
    // call at any rate (clamped internally).
    void pushGainReduction(float dB);

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

    // Push LED-cell state for a button after it toggles.
    void pushButtonLed_(uint8_t buttonId, bool on);

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
    void pushKnobRing_(uint8_t knobId, double normalized);

    // Find the VST3 param index of the SSL Channel Strip's internal
    // "Channel In" switch on the given track+fx slot. Scans param
    // names for common spellings ("CsIn", "ChannelIn", "Channel In"…)
    // and caches the hit. Returns -1 if no match; caller falls back to
    // TrackFX_SetEnabled.
    int channelInParam_(void* track /*MediaTrack**/, int fxIdx);

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

    // Per-knob LED-ring cell state cache. pushKnobRing_ writes only
    // cells whose target state differs from the cache; setFocusedTrack
    // clears it so the next refresh re-writes every cell, preventing
    // device/cache desync (init-flood lit cells we never told the
    // firmware to clear, etc.).
    std::unordered_map<uint8_t, std::vector<uint8_t>> ringCellCache_;
};

} // namespace uc1
