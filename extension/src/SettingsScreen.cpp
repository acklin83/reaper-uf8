#include "SettingsScreen.h"

#include <cstdio>

#include "reaper_imgui_functions.h"

// Forward declarations of accessors defined in main.cpp. Same pattern as
// reasixty_followSelectedInMixer / reasixty_toggleMixerWindow — keeps the
// anonymous-namespace globals owned by main.cpp while letting the UI read
// runtime state. Called only from the main thread (via onTimer → ImGui).
bool reasixty_uf8Connected();
bool reasixty_uc1Connected();
int  reasixty_brightnessLevel();
int  reasixty_scribbleBrightnessLevel();
void reasixty_setBrightnessLevel(int level);
void reasixty_setScribbleBrightnessLevel(int level);
void reasixty_identifyUf8();
void reasixty_identifyUc1();

namespace uf8 {

// ---- Device ---------------------------------------------------------------
// Per docs/plan-settings-ui.md §"Tab: Device" + SSL HOME equivalent (see
// docs/ssl-360-settings-inventory.md):
//   - Connected devices list with USB status dots (UF8 #N, UC1 #N, …)
//     + serial number, drag-to-reorder when ≥2 UF8s
//   - Identify Unit button — overrides target's LCD with a "THIS UNIT"
//     marker for ~2 s. Reuses the existing UF8/UC1 frame protocol.
//   - LED brightness slider (writes the global-brightness frame)
//   - Scribble brightness slider
//   - Meter ballistic selector (PPM / VU / RMS) — UI-only until Phase 1
//     meter forwarding is implemented
//   - SEL-follows-track-color toggle (needs SEL-color frame capture)
//   - Export Diagnostic Report button — produces
//     ~/Desktop/rea_sixty_diag_<date>.zip with build hash, REAPER version,
//     recent extension log, USB device tree.
void SettingsScreen::drawDevice(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Connected devices");
    ImGui_Separator(ctx);

    char line[64];
    const bool uf8On = reasixty_uf8Connected();
    const bool uc1On = reasixty_uc1Connected();

    std::snprintf(line, sizeof(line), "  UF8   %s",
                  uf8On ? "[connected]" : "[not connected]");
    ImGui_Text(ctx, line);
    if (uf8On) {
        ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
        if (ImGui_Button(ctx, "Identify##uf8",
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            reasixty_identifyUf8();
        }
    }

    std::snprintf(line, sizeof(line), "  UC1   %s",
                  uc1On ? "[connected]" : "[not connected]");
    ImGui_Text(ctx, line);
    if (uc1On) {
        ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
        if (ImGui_Button(ctx, "Identify##uc1",
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            reasixty_identifyUc1();
        }
    }

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);

    ImGui_Text(ctx, "Brightness");
    ImGui_Separator(ctx);

    // 5 SSL-equivalent steps: dark / dim / half / bright / full. LED step
    // drives buttons + V-Pot rings + UC1 LEDs. Scribble step drives the
    // UF8 LCD strips, UC1 LCD, and UC1 status displays. Independent so a
    // user can crank the displays while keeping LEDs dim, or vice versa.
    static const char* kLevelNames[5] = {
        "Dark", "Dim", "Half", "Bright", "Full"
    };
    auto fmtLevel = [&](int level) {
        if (level >= 0 && level <= 4) {
            std::snprintf(line, sizeof(line), "  %s", kLevelNames[level]);
            ImGui_Text(ctx, line);
        }
    };

    int led = reasixty_brightnessLevel();
    ImGui_Text(ctx, "  LEDs");
    if (ImGui_SliderInt(ctx, "##led_brightness", &led,
                        /*v_min*/ 0, /*v_max*/ 4,
                        /*format*/ nullptr, /*flags*/ nullptr)) {
        reasixty_setBrightnessLevel(led);
    }
    fmtLevel(led);

    int scr = reasixty_scribbleBrightnessLevel();
    ImGui_Text(ctx, "  Scribble strips / LCDs");
    if (ImGui_SliderInt(ctx, "##scribble_brightness", &scr,
                        /*v_min*/ 0, /*v_max*/ 4,
                        /*format*/ nullptr, /*flags*/ nullptr)) {
        reasixty_setScribbleBrightnessLevel(scr);
    }
    fmtLevel(scr);

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Pending");
    ImGui_Separator(ctx);
    ImGui_Text(ctx, "  TODO: serial # + drag-to-reorder for multi-UF8 setups");
    ImGui_Text(ctx, "  TODO: meter ballistic selector (PPM / VU / RMS)");
    ImGui_Text(ctx, "  TODO: SEL-follows-track-color toggle");
    ImGui_Text(ctx, "  TODO: Export Diagnostic Report button (.zip to Desktop)");
}

// ---- Bindings -------------------------------------------------------------
// Consolidates docs/bindings.md §"Config UI Sketch (ReaImGui)" + §"Binding
// Types" + §"Builtin Action Catalogue (v1)", augmented with SSL-equivalent
// rows from docs/ssl-360-settings-inventory.md.
//
// Persistence: ~/Library/Application Support/REAPER/rea_sixty/bindings.json
// Sections (single scroll):
//   - Per-strip buttons       (Select / Mute / Solo / Rec / V-Pot press)
//   - Transport               (Play / Stop / Rec / RW / FF — REAPER actions)
//   - Global buttons          (Bank L/R, Channel L/R, Flip, Layer cycle)
//   - 3 Quick Keys            (UF8 left-side QUICK row)
//   - 2 Foot-switches         (UF8 jacks; placeholder until USB event decode)
//   - Layer-scoped soft-keys  (per active layer)
//   - Learn button (top right)
void SettingsScreen::drawBindings(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Bindings");
    ImGui_Text(ctx, "  TODO: per-strip target editor (select/mute/solo/rec/vpot_press)");
    ImGui_Text(ctx, "  TODO: transport bindings (REAPER action IDs)");
    ImGui_Text(ctx, "  TODO: global buttons (bank/channel L/R, flip, layer cycle)");
    ImGui_Text(ctx, "  TODO: 3 Quick Keys (UF8 QUICK row, default = layer switches)");
    ImGui_Text(ctx, "  TODO: 2 Foot-switches (UF8 jacks; placeholder until USB event decoded)");
    ImGui_Text(ctx, "  TODO: soft-key bindings table per active layer");
    ImGui_Text(ctx, "  TODO: Learn-mode arming + ESC-to-cancel");
    ImGui_Text(ctx, "  TODO: import / export / reset-to-defaults");
}

// ---- Soft-Key Banks -------------------------------------------------------
// Per memory uf8-softkey-banks.md: CS = 6 banks (V-POT + 1..5),
// BC = 2 banks (V-POT + 1). Authoritative bank tables live in main.cpp's
// `softkey::` namespace. kNoSlot positions await raw VST3 / REAPER action
// wiring.
//
// Source: SSL UF8 User Guide p.180-181.
void SettingsScreen::drawSoftKeyBanks(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Soft-Key Banks (UF8 PM mode)");
    ImGui_Text(ctx, "  TODO: ChannelStrip 6-bank grid (V-POT + Bank 1..5)");
    ImGui_Text(ctx, "  TODO: BusComp 2-bank grid (V-POT + Bank 1)");
    ImGui_Text(ctx, "  TODO: kNoSlot wiring — raw VST3 param picker / REAPER action picker");
    ImGui_Text(ctx, "  TODO: per-position label + colour override");
    ImGui_Text(ctx, "  TODO: bank-follow-focus toggle (default ON)");
}

// ---- Modes ----------------------------------------------------------------
// Phase 2.5 features per ROADMAP.md §"Phase 2.5":
//   2.5a Folder Mode          — long-press SEL toggles parent expand
//   2.5b Show Only Selected   — 8 selection slots persisted by GUID
//   2.5c Show Sends/Receives  — focus-variant Send Layer
//   2.5d Generic FX-param map — Learn-mode for any V-Pot/soft-button
// Plus V-Pot Behaviour (ships in 2.7d alongside the rest of this section):
//   - Always Fine Pan, Always Fine Sends, Show Auto State
// All three SSL-equivalent toggles per docs/ssl-360-settings-inventory.md.
void SettingsScreen::drawModes(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Modes");
    ImGui_Text(ctx, "");
    ImGui_Text(ctx, "Phase 2.5 features:");
    ImGui_Text(ctx, "  TODO: Folder Mode — long-press duration slider, expand depth");
    ImGui_Text(ctx, "  TODO: Show Only Selected — auto-save toggle");
    ImGui_Text(ctx, "  TODO: Send / Receive Layer — enable per-direction");
    ImGui_Text(ctx, "  TODO: Generic FX Mapping — Learn modifier + display format");
    ImGui_Text(ctx, "");
    ImGui_Text(ctx, "V-Pot Behaviour:");
    ImGui_Text(ctx, "  TODO: Always Fine Pan toggle");
    ImGui_Text(ctx, "  TODO: Always Fine Sends toggle");
    ImGui_Text(ctx, "  TODO: Show Auto State on scribble (GetTrackAutomationMode)");
}

// ---- Selection Sets -------------------------------------------------------
// Per ROADMAP.md §"2.5b" + plan-settings-ui.md §"Tab: Selection Sets":
// 8 slots each holding a list of Track GUIDs. Project-scoped via
// SetProjExtState("rea_sixty", "selset_N", …).
void SettingsScreen::drawSelectionSets(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Selection Sets");
    ImGui_Text(ctx, "  TODO: 8-slot grid (Slot 1..8)");
    ImGui_Text(ctx, "  TODO: per-slot name + Track-GUID list editor");
    ImGui_Text(ctx, "  TODO: 'save current selection' / 'recall' buttons");
    ImGui_Text(ctx, "  TODO: missing-track pruning UI");
}

// ---- About ----------------------------------------------------------------
void SettingsScreen::drawAbout(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Rea-Sixty");
    ImGui_Text(ctx, "  Open-source SSL 360° replacement for UF8 / UC1");
    ImGui_Text(ctx, "");
    ImGui_Text(ctx, "  TODO: version + build hash");
    ImGui_Text(ctx, "  TODO: REAPER + ReaImGui versions");
    ImGui_Text(ctx, "  TODO: links — repo, ReaPack URL, issue tracker");
    ImGui_Text(ctx, "  TODO: log-file location button (reveals in Finder)");
}

} // namespace uf8
