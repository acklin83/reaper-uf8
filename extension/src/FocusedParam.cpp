#include "FocusedParam.h"

namespace uf8 {

// Default = ChannelStrip / slotIdx -1 (no param focused). The render
// path treats -1 as the "fall back to Pan" sentinel and any other
// linkIdx that doesn't resolve via findSlotByLinkIdx as "leave the
// V-Pot zones blank" (= the param is selected but unavailable on
// this strip's plug-in variant — e.g. IMP IN selected, but track
// hosts CS 2). Soft-key bank LEDs all render dim until a focus is
// set by pressing a top-soft-key or by a UC1 knob touch.
std::atomic<FocusedParam> g_focusedParam{
    FocusedParam{ Domain::ChannelStrip, -1 }
};

std::atomic<bool> g_focusedDirty{false};

std::atomic<void*> g_focusedFxTrack{nullptr};

} // namespace uf8
