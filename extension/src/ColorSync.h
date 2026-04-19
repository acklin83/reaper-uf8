#pragma once
//
// ColorSync — REAPER-side bookkeeping. Holds a snapshot of the 8 currently
// visible track colors, quantizes them to palette indices, and pushes to
// the UF8 device whenever something changes.
//
// Triggers:
//   * track-color change (we poll on a REAPER timer; the SDK has no native
//     "track color changed" event — we diff against our snapshot)
//   * mixer-view scroll or track count change (bank shift)
//

#include <array>
#include <cstdint>

#include "Protocol.h"
#include "UF8Device.h"

namespace uf8 {

class ColorSync {
public:
    ColorSync(UF8Device& dev) : dev_(dev) {}

    // Read the current 8 visible tracks from REAPER, quantize, and push if
    // the array has changed. Safe to call on REAPER's main thread. No-op
    // when the device isn't open.
    //
    // `getTrackColor` is injected so we don't depend on reaper_plugin
    // headers here — in main.cpp we wrap the SDK's GetTrackColor into a
    // lambda. The callback must return the 0xRRGGBB color of the Nth
    // currently-visible track (0..7), or 0 if that slot has no track.
    void refresh(const std::function<uint32_t(int visibleSlot)>& getTrackColor);

    // Force the next refresh() to push even if the indices haven't changed.
    // Useful after (re)connecting to the device.
    void invalidate() { lastPushedValid_ = false; }

private:
    UF8Device&                       dev_;
    std::array<uint8_t, kStripCount> lastPushed_{};
    bool                             lastPushedValid_ = false;
};

} // namespace uf8
