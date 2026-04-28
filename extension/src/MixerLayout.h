#pragma once
//
// MixerLayout — pure layout/widget logic for the Plugin Mixer view tab.
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

class ImGui_Context; // ReaImGui opaque context pointer

namespace uf8 {

class MixerLayout {
public:
    static void draw(ImGui_Context* ctx);
};

} // namespace uf8
