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

    // --- state ---
    UC1Device* device_ = nullptr;
    void*      focusedTrack_ = nullptr;  // MediaTrack*

    std::mutex               queueMu_;
    std::deque<ButtonEvent>  buttonQueue_;
    std::deque<KnobEvent>    knobQueue_;

    SurfaceStats      stats_;
    std::string       lastError_;

    // Per-knob "fine" toggle. UC1's Fine button acts as a modifier:
    // when on, knob clicks move the param by 1/4 the normal amount.
    std::atomic<bool> fineMode_{false};
};

} // namespace uc1
