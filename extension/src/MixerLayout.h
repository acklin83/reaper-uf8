#pragma once
//
// MixerLayout — pure layout/widget logic for the Plugin Mixer window.
//
// 75% left: horizontally scrollable channel-strip columns (one per track
//           hosting a CS-family plugin from PluginMap).
// 25% right: vertical Bus Compressor rack (one strip per BC instance).
//
// Decoupled from MixerWindow so unit tests can exercise layout math without
// an ImGui context. Widget callbacks write back via TrackFX_SetParamNormalized
// directly — same path UF8/UC1 already use, no extra plumbing.
//
// Phase 2.6 scaffold; bodies arrive in 2.6b/2.6c.
//

namespace uf8 {

class MixerLayout {
public:
    // Single per-frame entry point. Reads g_visibleTracks (cached on
    // SetTrackListChange Surface callback) + per-track parameter values via
    // TrackFX_GetParamNormalized / GainReduction_dB / Track_GetPeakInfo.
    // Renders ImGui widgets in the active context.
    static void draw();
};

} // namespace uf8
