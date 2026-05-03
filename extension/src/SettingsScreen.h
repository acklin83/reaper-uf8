#pragma once
//
// SettingsScreen — Rea-Sixty configuration UI. Each section renders
// inside the right-hand content pane of MixerWindow's left-rail nav;
// MixerWindow drives selection, this header just exposes the draws.
//
// Section sources:
//   docs/plan-settings-ui.md      — original tab structure
//   docs/bindings.md              — binding format / Learn-mode flow
//   docs/ssl-360-settings-inventory.md — gap analysis vs SSL 360°
//   memory uf8-softkey-banks.md   — CS 6 banks + BC 2 banks
//
// Persistence still goes through ExtState `rea_sixty` (per-project) +
// JSON config (global) per bindings.md §"Config File".
//

class ImGui_Context;

namespace uf8 {

class SettingsScreen {
public:
    static void drawDevice(ImGui_Context* ctx);
    static void drawBindings(ImGui_Context* ctx);
    static void drawSoftKeyBanks(ImGui_Context* ctx);
    static void drawFxLearn(ImGui_Context* ctx);
    static void drawModes(ImGui_Context* ctx);
    static void drawSelectionSets(ImGui_Context* ctx);
    static void drawAbout(ImGui_Context* ctx);
};

} // namespace uf8
