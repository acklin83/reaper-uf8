#include "SettingsScreen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Bindings.h"
#include "reaper_imgui_functions.h"

// Forward declarations of accessors defined in main.cpp. Same pattern as
// reasixty_followSelectedInMixer / reasixty_toggleMixerWindow — keeps the
// anonymous-namespace globals owned by main.cpp while letting the UI read
// runtime state. Called only from the main thread (via onTimer → ImGui).
bool reasixty_uf8Connected();
bool reasixty_uc1Connected();
const char* reasixty_uf8Serial();
const char* reasixty_uc1Serial();
int  reasixty_brightnessLevel();
int  reasixty_scribbleBrightnessLevel();
void reasixty_setBrightnessLevel(int level);
void reasixty_setScribbleBrightnessLevel(int level);
void reasixty_identifyUf8();
void reasixty_identifyUc1();
bool reasixty_selFollowsColor();
void reasixty_setSelFollowsColor(bool follow);
int  reasixty_ballisticMode();
void reasixty_setBallisticMode(int mode);
void reasixty_exportDiagnostic();  // shows confirmation dialog itself
const char* reasixty_reaperVersion();
void reasixty_openUrl(const char* url);
void reasixty_revealInFinder(const char* path);

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

    char line[128];
    const bool uf8On = reasixty_uf8Connected();
    const bool uc1On = reasixty_uc1Connected();

    auto deviceLine = [&](const char* name, bool on, const char* serial) {
        if (on && serial && *serial) {
            std::snprintf(line, sizeof(line), "  %s   [connected]   SN %s",
                          name, serial);
        } else {
            std::snprintf(line, sizeof(line), "  %s   %s", name,
                          on ? "[connected]" : "[not connected]");
        }
        ImGui_Text(ctx, line);
    };

    deviceLine("UF8", uf8On, reasixty_uf8Serial());
    if (uf8On) {
        ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
        if (ImGui_Button(ctx, "Identify##uf8",
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            reasixty_identifyUf8();
        }
    }

    deviceLine("UC1", uc1On, reasixty_uc1Serial());
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
    ImGui_Text(ctx, "Display behaviour");
    ImGui_Separator(ctx);

    bool selFollow = reasixty_selFollowsColor();
    if (ImGui_Checkbox(ctx, "SEL LED follows REAPER track colour",
                       &selFollow)) {
        reasixty_setSelFollowsColor(selFollow);
    }

    // Ballistic dropdown. Combo's `items` arg is a NUL-separated list
    // followed by a final NUL terminator — the string literal already
    // ends with one implicit \0, so "Peak\0VU\0RMS\0" is the proper
    // double-terminated form.
    static char kBallisticItems[] = "Peak\0VU\0RMS\0";
    int ballistic = reasixty_ballisticMode();
    if (ImGui_Combo(ctx, "Meter ballistic", &ballistic,
                    kBallisticItems,
                    /*popup_max_height_in_items*/ nullptr)) {
        reasixty_setBallisticMode(ballistic);
    }

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Diagnostic");
    ImGui_Separator(ctx);
    if (ImGui_Button(ctx, "Export diagnostic report",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        reasixty_exportDiagnostic();
    }
    ImGui_Text(ctx, "  Bundles version + state + recent traces into a .zip");
    ImGui_Text(ctx, "  on your Desktop. Attach this when reporting bugs.");

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Pending");
    ImGui_Separator(ctx);
    ImGui_Text(ctx, "  Drag-to-reorder for multi-UF8 setups: deferred");
    ImGui_Text(ctx, "  (codebase has no multi-UF8 support yet — single-device assumption");
    ImGui_Text(ctx, "  in the bank-shift / colour-sync / VU-meter paths).");
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
namespace {

using uf8::bindings::ButtonId;

// Short button-face label shown on the schematic (≈ what's printed on
// the physical UF8). Falls back to the canonical snake_case name if a
// button slips through without an entry.
const char* hwFaceLabel(ButtonId id)
{
    switch (id) {
        case ButtonId::BankLeft:    return "Bank ←";
        case ButtonId::BankRight:   return "Bank →";
        case ButtonId::PageLeft:    return "Page ←";
        case ButtonId::PageRight:   return "Page →";
        case ButtonId::Layer1:      return "Layer 1";
        case ButtonId::Layer2:      return "Layer 2";
        case ButtonId::Layer3:      return "Layer 3";
        case ButtonId::Quick1:      return "Quick 1";
        case ButtonId::Quick2:      return "Quick 2";
        case ButtonId::Quick3:      return "Quick 3";
        case ButtonId::PluginBtn:   return "Plugin";
        case ButtonId::Flip:        return "Flip";
        case ButtonId::Pan:         return "Pan";
        case ButtonId::Fine:        return "Fine";
        case ButtonId::Btn360:      return "360";
        case ButtonId::AutoOff:     return "Off";
        case ButtonId::AutoRead:    return "Read";
        case ButtonId::AutoWrite:   return "Write";
        case ButtonId::AutoTrim:    return "Trim";
        case ButtonId::AutoLatch:   return "Latch";
        case ButtonId::AutoTouch:   return "Touch";
        case ButtonId::ZoomUp:      return "Zoom ↑";
        case ButtonId::ZoomDown:    return "Zoom ↓";
        case ButtonId::ZoomLeft:    return "Zoom ←";
        case ButtonId::ZoomRight:   return "Zoom →";
        case ButtonId::ZoomCenter:  return "Fit";
        case ButtonId::Nav:         return "Nav";
        case ButtonId::Nudge:       return "Nudge";
        case ButtonId::EncFocus:    return "Focus";
        case ButtonId::ChannelPush: return "Encoder ⏷";
        default:                    return uf8::bindings::toName(id);
    }
}

// One clickable hardware-face button. Highlighted when selected. On
// click, sets `sel`. Width default 70, height 26 — keeps the row
// compact while still readable. The button label is the hardware face
// (Bank ←, FLIP, etc.); the actual binding is shown in the editor below.
void drawHwButton(ImGui_Context* ctx, ButtonId id, ButtonId& sel,
                  double w = 70, double h = 26)
{
    const bool selected = (id == sel);
    if (selected) {
        // Soft blue highlight, only on the button itself (PushStyleColor
        // is auto-popped by the matching PopStyleColor below).
        ImGui_PushStyleColor(ctx, ImGui_Col_Button,        0x4477CCFF);
        ImGui_PushStyleColor(ctx, ImGui_Col_ButtonHovered, 0x5588DDFF);
        ImGui_PushStyleColor(ctx, ImGui_Col_ButtonActive,  0x6699EEFF);
    }
    ImGui_PushID(ctx, uf8::bindings::toName(id));
    if (ImGui_Button(ctx, hwFaceLabel(id), &w, &h)) {
        sel = id;
    }
    ImGui_PopID(ctx);
    if (selected) {
        int n = 3;
        ImGui_PopStyleColor(ctx, &n);
    }
}

// Greyed, non-clickable button — used for hardware buttons that exist
// on the UF8 but aren't bindable in v1 (per-strip Sel/Cut/Solo, V-Pot
// push, top soft-keys, soft-key bank selectors). Shown so the user
// sees the full hardware layout in context.
void drawLockedButton(ImGui_Context* ctx, const char* label,
                      double w = 70, double h = 26)
{
    ImGui_PushStyleColor(ctx, ImGui_Col_Button,        0x33333355);
    ImGui_PushStyleColor(ctx, ImGui_Col_ButtonHovered, 0x33333355);
    ImGui_PushStyleColor(ctx, ImGui_Col_ButtonActive,  0x33333355);
    ImGui_PushStyleColor(ctx, ImGui_Col_Text,          0x88888888);
    ImGui_Button(ctx, label, &w, &h);
    int n = 4;
    ImGui_PopStyleColor(ctx, &n);
}

void sameLine(ImGui_Context* ctx)
{
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
}

// Schematic of the UF8 — mirrors the physical layout loosely. Per-strip
// area is locked (v1 design); the right-hand control panel hosts the
// bindable globals. Click any active button to select it for editing.
void drawUf8Schematic(ImGui_Context* ctx, ButtonId& sel)
{
    // ---- Strip area (8 columns × 5 row-groups, all locked in v1) ----
    ImGui_Text(ctx, "Strips (per-strip controls — hardcoded in v1):");
    char buf[16];
    auto stripRow = [&](const char* prefix) {
        for (int i = 0; i < 8; ++i) {
            std::snprintf(buf, sizeof(buf), "%s %d", prefix, i + 1);
            drawLockedButton(ctx, buf, /*w*/ 70, /*h*/ 22);
            if (i < 7) sameLine(ctx);
        }
    };
    stripRow("Soft");      // 0x18..0x1F
    stripRow("V-Pot");     // 0x08..0x0F
    stripRow("Sel");       // 0x22..0x37 (sel)
    stripRow("Cut");       //   "         (cut)
    stripRow("Solo");      //   "         (solo)

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Soft-key banks (hardcoded):");
    drawLockedButton(ctx, "V-POT");  sameLine(ctx);
    drawLockedButton(ctx, "Bank 1"); sameLine(ctx);
    drawLockedButton(ctx, "Bank 2"); sameLine(ctx);
    drawLockedButton(ctx, "Bank 3"); sameLine(ctx);
    drawLockedButton(ctx, "Bank 4"); sameLine(ctx);
    drawLockedButton(ctx, "Bank 5");

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    // ---- Bindable globals — laid out roughly like the UF8 right panel ----
    ImGui_Text(ctx, "Top row — modes & layers:");
    drawHwButton(ctx, ButtonId::Btn360,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Quick3,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Quick2,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Quick1,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Layer3,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Layer2,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Layer1,  sel);

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Plugin / Page / Flip:");
    drawHwButton(ctx, ButtonId::PluginBtn, sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::PageLeft,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::PageRight, sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Flip,      sel);

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Pan / Fine:");
    drawHwButton(ctx, ButtonId::Pan,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Fine, sel);

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Automation:");
    drawHwButton(ctx, ButtonId::AutoOff,   sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::AutoRead,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::AutoWrite, sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::AutoTrim,  sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::AutoLatch, sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::AutoTouch, sel);

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Encoder & bank scroll:");
    drawHwButton(ctx, ButtonId::Nav,         sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::Nudge,       sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::EncFocus,    sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::ChannelPush, sel); sameLine(ctx);
    ImGui_Dummy(ctx, 16, 0); sameLine(ctx);
    drawHwButton(ctx, ButtonId::BankLeft,    sel); sameLine(ctx);
    drawHwButton(ctx, ButtonId::BankRight,   sel);

    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Zoom pad:");
    {
        // Fake a cross-pad with Dummy spacers.
        ImGui_Dummy(ctx, 78, 0); sameLine(ctx);
        drawHwButton(ctx, ButtonId::ZoomUp, sel);

        drawHwButton(ctx, ButtonId::ZoomLeft,   sel); sameLine(ctx);
        drawHwButton(ctx, ButtonId::ZoomCenter, sel); sameLine(ctx);
        drawHwButton(ctx, ButtonId::ZoomRight,  sel);

        ImGui_Dummy(ctx, 78, 0); sameLine(ctx);
        drawHwButton(ctx, ButtonId::ZoomDown, sel);
    }
}

// Editor panel for the currently-selected button. Reads the binding
// fresh each frame, writes back through bindings::setBinding on change.
// Lays out one widget per line so each label is fully readable — the
// horizontal-table layout in the previous list view ate too much
// horizontal space.
void drawBindingEditor(ImGui_Context* ctx, int layer, ButtonId id)
{
    using namespace uf8::bindings;

    Binding bd    = getBinding(layer, id);
    bool    dirty = false;

    char header[64];
    std::snprintf(header, sizeof(header),
                  "Editing: %s   (Layer %d)", hwFaceLabel(id), layer + 1);
    ImGui_Text(ctx, header);
    ImGui_Separator(ctx);

    ImGui_PushID(ctx, uf8::bindings::toName(id));

    // ---- Type ----
    static char kTypeItems[] =
        "Do nothing\0"
        "REAPER action\0"
        "Keyboard chord\0"
        "Built-in Rea-Sixty action\0";
    int t = static_cast<int>(bd.type);
    {
        double w = 240;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_Combo(ctx, "Action type", &t, kTypeItems,
                        /*popup_max_height_in_items*/ nullptr)) {
            bd.type = static_cast<ActionType>(t);
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }

    // ---- Action argument (type-dependent) ----
    if (bd.type == ActionType::Builtin) {
        // Combo over registered builtin display names. Internal items
        // (anything starting with __) are filtered out by builtinNames().
        const std::string preview = bd.action.empty()
            ? std::string("<pick a built-in>")
            : builtinDisplayName(bd.action);
        double w = 320;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_BeginCombo(ctx, "Built-in", preview.c_str(),
                             /*flags*/ nullptr)) {
            for (auto& n : builtinNames()) {
                std::string label = builtinDisplayName(n);
                if (label != n) {
                    // Append canonical name for power users.
                    label += "   [" + n + "]";
                }
                bool sel = (n == bd.action);
                if (ImGui_Selectable(ctx, label.c_str(), &sel,
                                     /*flags*/ nullptr,
                                     /*size_w*/ nullptr,
                                     /*size_h*/ nullptr)) {
                    bd.action = n;
                    dirty = true;
                }
            }
            ImGui_EndCombo(ctx);
        }
        ImGui_PopItemWidth(ctx);
    } else if (bd.type == ActionType::Reaper) {
        char buf[128] = {0};
        std::strncpy(buf, bd.action.c_str(), sizeof(buf) - 1);
        double w = 320;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_InputTextWithHint(ctx, "Action ID",
                                    "e.g. 40044 (Track: Toggle FX bypass)",
                                    buf, sizeof(buf), /*flags*/ nullptr)) {
            bd.action = buf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
        ImGui_Text(ctx, "  Find an action's ID in REAPER's Actions list (?).");
    } else if (bd.type == ActionType::Keyboard) {
        char buf[128] = {0};
        std::strncpy(buf, bd.action.c_str(), sizeof(buf) - 1);
        double w = 320;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_InputTextWithHint(ctx, "Key chord",
                                    "e.g. ctrl+s",
                                    buf, sizeof(buf), /*flags*/ nullptr)) {
            bd.action = buf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
        ImGui_Text(ctx, "  (Keyboard simulation lands in Phase D.)");
    }

    // ---- Behavior ----
    static char kBehaviorItems[] =
        "Momentary (fire on press)\0"
        "Toggle (flip on each press)\0"
        "Hold (state mirrors button)\0";
    int b = static_cast<int>(bd.behavior);
    {
        double w = 240;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_Combo(ctx, "Behavior", &b, kBehaviorItems,
                        /*popup_max_height_in_items*/ nullptr)) {
            bd.behavior = static_cast<Behavior>(b);
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }

    // ---- Param (only for builtins that read it) ----
    if (bd.type == ActionType::Builtin && builtinUsesParam(bd.action)) {
        double w = 100;
        ImGui_PushItemWidth(ctx, w);
        int p = bd.param;
        if (ImGui_InputInt(ctx, "Parameter", &p,
                           /*step*/ nullptr, /*step_fast*/ nullptr,
                           /*flags*/ nullptr)) {
            bd.param = p;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }

    ImGui_Spacing(ctx);
    if (ImGui_Button(ctx, "Clear binding (Do nothing)",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        bd = Binding{};   // type=Noop, behavior=Momentary, no action
        dirty = true;
    }

    if (dirty) {
        setBinding(layer, id, bd);
    }

    ImGui_PopID(ctx);
}

} // namespace

// Phase C UI — hardware-schematic view. Click a button on the schematic
// to select it; the editor below reveals the binding details. Auto-saves
// on every change (USB worker picks up the new binding on next press
// through dispatch's lock-protected lookup).
//
// Per-strip Sel/Cut/Solo/Rec, V-Pot push, top soft-keys, and soft-key
// bank selectors are shown greyed/locked — they stay hardcoded in v1
// (resolved Q2). Phase D widens the catalogue.
void SettingsScreen::drawBindings(ImGui_Context* ctx)
{
    using namespace uf8::bindings;

    static int      s_editLayer = 0;
    static ButtonId s_selected  = ButtonId::None;

    // ---- Top: layer + activate + auto-mixer + reset ----
    static char kLayerItems[] = "Layer 1\0Layer 2\0Layer 3\0";
    {
        double w = 160;
        ImGui_PushItemWidth(ctx, w);
        ImGui_Combo(ctx, "Editing layer", &s_editLayer, kLayerItems,
                    /*popup_max_height_in_items*/ nullptr);
        ImGui_PopItemWidth(ctx);
    }

    {
        const int active = getActiveLayer();
        char line[64];
        std::snprintf(line, sizeof(line), "Active on hardware: Layer %d",
                      active + 1);
        ImGui_Text(ctx, line);
        sameLine(ctx);
        if (ImGui_Button(ctx, "Make this layer active",
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            setActiveLayer(s_editLayer);
        }
    }

    if (s_editLayer >= 1) {
        bool autoMixer = get().layers[s_editLayer].autoWhenMixerVisible;
        if (ImGui_Checkbox(ctx,
                "Auto-switch to this layer when Plugin Mixer is open",
                &autoMixer)) {
            setLayerAutoMixer(s_editLayer, autoMixer);
        }
    } else {
        ImGui_Text(ctx, "(Layer 1 is the default — mixer auto-switch lives "
                        "on Layer 2 / 3.)");
    }

    if (ImGui_Button(ctx, "Reset this layer to factory defaults",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        resetLayerToDefaults(s_editLayer);
    }

    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    // ---- Hardware schematic ----
    drawUf8Schematic(ctx, s_selected);

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    // ---- Editor (only when a button is selected) ----
    if (s_selected == ButtonId::None) {
        ImGui_Text(ctx, "Click a button above to edit its binding.");
    } else {
        drawBindingEditor(ctx, s_editLayer, s_selected);
    }
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
// Static text + a few buttons that shell out to `open` for browser /
// Finder reveal. No fancy hyperlink widget — ReaImGui v0.10 has none we
// can rely on; plain Text + Button is cross-version safe.
void SettingsScreen::drawAbout(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Rea-Sixty");
    ImGui_Text(ctx, "Open-source SSL 360 replacement for UF8 / UC1");
    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);

    ImGui_Text(ctx, "Versions");
    ImGui_Separator(ctx);
    char line[256];
    std::snprintf(line, sizeof(line), "  Build:    %s %s",
                  __DATE__, __TIME__);
    ImGui_Text(ctx, line);
    std::snprintf(line, sizeof(line), "  REAPER:   %s",
                  reasixty_reaperVersion());
    ImGui_Text(ctx, line);
    ImGui_Text(ctx, "  ReaImGui: bundled v0.10 ABI");

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Project");
    ImGui_Separator(ctx);
    static const char* kRepoUrl = "https://github.com/acklin83/reaper-uf8";
    std::snprintf(line, sizeof(line), "  Repository:  %s", kRepoUrl);
    ImGui_Text(ctx, line);
    if (ImGui_Button(ctx, "Open repository in browser",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        reasixty_openUrl(kRepoUrl);
    }

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Logs");
    ImGui_Separator(ctx);
    ImGui_Text(ctx, "  /tmp/reaper_uf8_frames.log   (frame trace, when enabled)");
    ImGui_Text(ctx, "  /tmp/reaper_uf8_colors.log   (ColorSync push log)");
    if (ImGui_Button(ctx, "Reveal /tmp in Finder",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        reasixty_revealInFinder("/tmp");
    }

    ImGui_Spacing(ctx);
    ImGui_Spacing(ctx);
    ImGui_Text(ctx, "Acknowledgements");
    ImGui_Separator(ctx);
    ImGui_Text(ctx, "  Built without affiliation with Solid State Logic.");
    ImGui_Text(ctx, "  ReaImGui (cfillion) handles all on-screen rendering.");
    ImGui_Text(ctx, "  libusb drives the UF8 / UC1 vendor-USB endpoints.");
}

} // namespace uf8
