#include "FocusedParam.h"

namespace uf8 {

// Default = ChannelStrip / slot 0. This matches the pre-refactor behaviour
// of the old g_pageIdx{0}: on boot, all 8 UF8 strips display slot 0 of
// whichever CS-family plugin sits on each strip's track (e.g. "FADER" on
// CS 2). Domain stays ChannelStrip until a future write — Stage 2+
// introduces transitions to None / BusComp.
std::atomic<FocusedParam> g_focusedParam{
    FocusedParam{ Domain::ChannelStrip, 0 }
};

std::atomic<bool> g_focusedDirty{false};

} // namespace uf8
