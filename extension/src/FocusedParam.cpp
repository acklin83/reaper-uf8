#include "FocusedParam.h"

namespace uf8 {

// Default = ChannelStrip / linkIdx 35 (LinkableFaderLevel = "Fader" in
// every CS-family map: CS 2, 4K B, 4K E, 4K G — all align on this index
// per the SSL 360 Link layout). Must be a real linkIdx, not an array
// position: findSlotByLinkIdx walks linkIdx, so 0 resolves to nullptr
// (lowest CS linkIdx is 4) — leaving FLIP and the V-Pot param-drive
// path inactive until the first UC1 knob turn populated a valid focus.
std::atomic<FocusedParam> g_focusedParam{
    FocusedParam{ Domain::ChannelStrip, 35 }
};

std::atomic<bool> g_focusedDirty{false};

} // namespace uf8
