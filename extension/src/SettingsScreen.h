#pragma once
//
// SettingsScreen — Rea-Sixty configuration UI (full-screen tab inside the
// Mixer Window). Layout consolidates two existing design notes:
//
//   docs/plan-settings-ui.md   — original tab structure (Device, Mappings,
//                                Layers, Modes, Selection Sets, About)
//   docs/bindings.md           — concrete binding format / Learn-mode flow,
//                                builtin action catalogue
//
// Plus the more recent reference memory:
//
//   uf8-softkey-banks.md       — CS 6 banks + BC 2 banks per UF8 UG p.180-
//                                181, kNoSlot positions awaiting raw VST3
//                                / REAPER-action wiring
//
// All persistence still goes through ExtState `rea_sixty` (per-project) +
// JSON config (global) per bindings.md §"Config File". This screen is the
// editor; the file format is shared with Learn Mode and direct hand-edits.
//
// Phase 2.7 scaffold; per-tab bodies arrive incrementally.
//

class ImGui_Context;

namespace uf8 {

class SettingsScreen {
public:
    static void draw(ImGui_Context* ctx);
};

} // namespace uf8
