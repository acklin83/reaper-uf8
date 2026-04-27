#pragma once
//
// FocusedParam — single global "currently selected parameter" state shared
// across UF8 + UC1.
//
// SSL 360°'s Plug-in Mixer model: one parameter is "focused" at a time
// (e.g. "HF Gain"). All 8 UF8 V-Pots/scribble strips show that same
// parameter — strip N controls it on track N's plugin instance. UC1's
// dedicated readout zone mirrors the focused param on the selected
// track.
//
// Selection sources (Stage 3+): UC1 knob turn, UF8 top soft-key, REAPER
// plugin GUI move. They all converge here. Stage 1 just establishes
// storage; Stage 2 adds the cross-device setter API.
//
// Domain separates Channel-Strip-family slots (CS 2 / 4K B / 4K E / 4K G —
// share the SSL 360 Link virtual-strip layout, indexed comparably) from
// Bus Comp 2 (own slot list, BC-specific knob bindings on UC1). A track
// can host both; the active domain decides which slot list slotIdx
// indexes into.
//

#include <atomic>
#include <cstdint>

namespace uf8 {

struct alignas(8) FocusedParam {
    enum Domain : uint8_t {
        None         = 0,  // No plugin / fall back to track-name + Vol display
        ChannelStrip = 1,  // Slot index lives in CS-family PluginMap.slots
        BusComp      = 2,  // Slot index lives in Bus Comp 2 PluginMap.slots
    };

    Domain  domain;
    int32_t slotIdx;

    constexpr bool operator==(const FocusedParam& o) const noexcept {
        return domain == o.domain && slotIdx == o.slotIdx;
    }
};
static_assert(sizeof(FocusedParam) == 8,
              "FocusedParam must be 8 bytes for lock-free atomic on common targets");

extern std::atomic<FocusedParam> g_focusedParam;

// Set by any writer of g_focusedParam; consumed by the timer's display
// re-render path to invalidate label/value caches and force a full
// re-push on the next tick.
extern std::atomic<bool> g_focusedDirty;

inline FocusedParam getFocusedParam() noexcept {
    return g_focusedParam.load(std::memory_order_relaxed);
}

} // namespace uf8
