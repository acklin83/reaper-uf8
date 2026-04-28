#pragma once
//
// ThemeBridge — pulls REAPER's current color theme and pushes it into the
// active ImGui style. Auto-adopts whatever theme the user has loaded
// (Reapertips, Default, custom). No SSL trademark colors hardcoded.
//
// Three REAPER theme APIs are stitched together:
//   1. GetColorTheme(int idx)        — ~20 indexed colors (deprecated but stable)
//   2. GetColorThemeStruct(int* sz)  — full struct, layout from icontheme.h
//   3. GetThemeColor(name, flags)    — string-based fallback (if available)
//
// REAPER has no theme-change broadcast callback. Workaround: probeHash()
// computes a CRC over the first N bytes of the theme struct each tick;
// when the hash changes, reapply() rebuilds the ImGui style.
//

#include <cstdint>

namespace uf8 {

class ThemeBridge {
public:
    // Pull the current REAPER theme and push to ImGui::GetStyle().
    // No-op-safe to call before any ImGui context exists.
    static void reapply();

    // 32-bit hash over the theme struct head (first ~1 KB). Cheap.
    static uint32_t probeHash();

    // Call from MixerWindow::onRunTick(): if the hash differs from the
    // last applied one, calls reapply(). Idempotent.
    static void tick();
};

} // namespace uf8
