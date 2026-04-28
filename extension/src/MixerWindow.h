#pragma once
//
// MixerWindow — dockable on-screen Plugin Mixer (Phase 2.6).
//
// Hosts a Dear ImGui context inside a SWELL window. The window is registered
// with REAPER via DockWindowAddEx so the user can dock it next to Mixer /
// Arrange. Render loop is driven from IReaperControlSurface::Run() — same
// tick that pushes UF8/UC1 frames — so all REAPER-API reads (TrackFX_*,
// Track_GetPeakInfo, GainReduction_dB) happen in one synchronous main-thread
// pass per frame.
//
// This header is the Phase 2.6a scaffold. Bodies are stubs until ImGui has
// been vendored into extension/vendor/imgui/ and icontheme.h has been pulled
// from upstream REAPER SDK.
//
// Plan: ~/.claude/plans/splendid-snuggling-hejlsberg.md
// Roadmap: ROADMAP.md, "Phase 2.6 — Plugin Mixer Window"
//

namespace uf8 {

class MixerWindow {
public:
    MixerWindow();
    ~MixerWindow();

    MixerWindow(const MixerWindow&)            = delete;
    MixerWindow& operator=(const MixerWindow&) = delete;

    // Toggle visibility. Wired to the REAPER action
    // "Rea-Sixty: Toggle Plugin Mixer Window".
    void toggle();

    bool isOpen() const;

    // Called from IReaperControlSurface::Run() each tick. No-op when closed.
    // Drives ImGui frame begin/render/end + theme-change probe.
    void onRunTick();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace uf8
