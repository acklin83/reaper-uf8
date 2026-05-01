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

struct ButtonRow {
    uf8::bindings::ButtonId id;
    const char*             label;
};

struct Section {
    const char*       header;
    const ButtonRow*  rows;
    int               nRows;
};

// v1 binding rows — must match what fromUf8DeviceId in Bindings.cpp
// resolves. Sections mirror the UF8 hardware layout so the user can find
// rows visually. Per-strip + soft-key bank selectors stay out of the
// catalogue (resolved Q2: hardcoded in v1).
constexpr ButtonRow kBankPage[] = {
    { uf8::bindings::ButtonId::BankLeft,  "Bank ←"  },
    { uf8::bindings::ButtonId::BankRight, "Bank →"  },
    { uf8::bindings::ButtonId::PageLeft,  "Page ←"  },
    { uf8::bindings::ButtonId::PageRight, "Page →"  },
};
constexpr ButtonRow kLayer[] = {
    { uf8::bindings::ButtonId::Layer1, "Layer 1" },
    { uf8::bindings::ButtonId::Layer2, "Layer 2" },
    { uf8::bindings::ButtonId::Layer3, "Layer 3" },
};
constexpr ButtonRow kQuick[] = {
    { uf8::bindings::ButtonId::Quick1, "Quick 1" },
    { uf8::bindings::ButtonId::Quick2, "Quick 2" },
    { uf8::bindings::ButtonId::Quick3, "Quick 3" },
};
constexpr ButtonRow kPluginMode[] = {
    { uf8::bindings::ButtonId::PluginBtn, "Plugin" },
    { uf8::bindings::ButtonId::Flip,      "Flip"   },
    { uf8::bindings::ButtonId::Pan,       "Pan"    },
    { uf8::bindings::ButtonId::Fine,      "Fine"   },
    { uf8::bindings::ButtonId::Btn360,    "360"    },
};
constexpr ButtonRow kAutomation[] = {
    { uf8::bindings::ButtonId::AutoOff,   "Off"   },
    { uf8::bindings::ButtonId::AutoRead,  "Read"  },
    { uf8::bindings::ButtonId::AutoWrite, "Write" },
    { uf8::bindings::ButtonId::AutoTrim,  "Trim"  },
    { uf8::bindings::ButtonId::AutoLatch, "Latch" },
    { uf8::bindings::ButtonId::AutoTouch, "Touch" },
};
constexpr ButtonRow kZoom[] = {
    { uf8::bindings::ButtonId::ZoomUp,     "Zoom ↑" },
    { uf8::bindings::ButtonId::ZoomDown,   "Zoom ↓" },
    { uf8::bindings::ButtonId::ZoomLeft,   "Zoom ←" },
    { uf8::bindings::ButtonId::ZoomRight,  "Zoom →" },
    { uf8::bindings::ButtonId::ZoomCenter, "Zoom ⤧" },
};
constexpr ButtonRow kEncoder[] = {
    { uf8::bindings::ButtonId::Nav,         "Nav"           },
    { uf8::bindings::ButtonId::Nudge,       "Nudge"         },
    { uf8::bindings::ButtonId::EncFocus,    "Focus"         },
    { uf8::bindings::ButtonId::ChannelPush, "Channel Push"  },
};

constexpr Section kSections[] = {
    { "Bank / Page",   kBankPage,   sizeof(kBankPage)/sizeof(kBankPage[0])     },
    { "Layer",         kLayer,      sizeof(kLayer)/sizeof(kLayer[0])           },
    { "Quick keys",    kQuick,      sizeof(kQuick)/sizeof(kQuick[0])           },
    { "Plugin / Mode", kPluginMode, sizeof(kPluginMode)/sizeof(kPluginMode[0]) },
    { "Automation",    kAutomation, sizeof(kAutomation)/sizeof(kAutomation[0]) },
    { "Zoom pad",      kZoom,       sizeof(kZoom)/sizeof(kZoom[0])             },
    { "Encoder modes", kEncoder,    sizeof(kEncoder)/sizeof(kEncoder[0])       },
};

void drawBindingRow(ImGui_Context* ctx, int layer, const ButtonRow& row)
{
    using namespace uf8::bindings;

    ImGui_PushID(ctx, toName(row.id));   // distinct id-stack per row
    Binding bd      = getBinding(layer, row.id);
    bool    dirty   = false;

    // First column — fixed-width label so combos line up across rows.
    ImGui_Text(ctx, row.label);
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr,
                   /*spacing*/ nullptr);

    // Type combo. NUL-separated items must end with an extra NUL — the
    // C-string literal already adds one implicit terminator.
    static char kTypeItems[] = "noop\0reaper\0keyboard\0builtin\0";
    int t = static_cast<int>(bd.type);
    {
        double w = 110;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_Combo(ctx, "##type", &t, kTypeItems,
                        /*popup_max_height_in_items*/ nullptr)) {
            bd.type = static_cast<ActionType>(t);
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);

    // Action argument — meaning depends on type:
    //   Reaper  → REAPER action ID (numeric or named, e.g. "40044")
    //   Keyboard→ key chord string (Phase D wires actual emission)
    //   Builtin → name from registry
    //   Noop    → ignored, hide the field
    {
        double w = 220;
        ImGui_PushItemWidth(ctx, w);
        if (bd.type == ActionType::Builtin) {
            // BeginCombo lets us list a runtime-built set of names.
            if (ImGui_BeginCombo(ctx, "##action",
                                 bd.action.empty() ? "<pick a builtin>"
                                                   : bd.action.c_str(),
                                 /*flags*/ nullptr)) {
                for (auto& n : builtinNames()) {
                    bool sel = (n == bd.action);
                    if (ImGui_Selectable(ctx, n.c_str(), &sel,
                                         /*flags*/ nullptr,
                                         /*size_w*/ nullptr,
                                         /*size_h*/ nullptr)) {
                        bd.action = n;
                        dirty = true;
                    }
                }
                ImGui_EndCombo(ctx);
            }
        } else if (bd.type == ActionType::Reaper
                || bd.type == ActionType::Keyboard) {
            char buf[128] = {0};
            std::strncpy(buf, bd.action.c_str(), sizeof(buf) - 1);
            const char* hint = (bd.type == ActionType::Reaper)
                ? "REAPER action ID (e.g. 40044)"
                : "Key chord (e.g. ctrl+s)";
            if (ImGui_InputTextWithHint(ctx, "##action", hint,
                                        buf, sizeof(buf),
                                        /*flags*/ nullptr)) {
                bd.action = buf;
                dirty = true;
            }
        } else {
            // Noop — placeholder so layout stays aligned.
            ImGui_Text(ctx, "—");
        }
        ImGui_PopItemWidth(ctx);
    }
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);

    // Behavior combo.
    static char kBehaviorItems[] = "momentary\0toggle\0hold\0";
    int b = static_cast<int>(bd.behavior);
    {
        double w = 110;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_Combo(ctx, "##behavior", &b, kBehaviorItems,
                        /*popup_max_height_in_items*/ nullptr)) {
            bd.behavior = static_cast<Behavior>(b);
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);

    // Param spinner — most builtins ignore it, but layer_select +
    // automation_mode use it as the target index / mode.
    {
        double w = 90;
        ImGui_PushItemWidth(ctx, w);
        int p = bd.param;
        if (ImGui_InputInt(ctx, "##param", &p,
                           /*step*/ nullptr, /*step_fast*/ nullptr,
                           /*flags*/ nullptr)) {
            bd.param = p;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }

    if (dirty) {
        uf8::bindings::setBinding(layer, row.id, bd);
    }

    ImGui_PopID(ctx);
}

} // namespace

// Phase C UI per architecture (memory bindings-architecture.md):
//   - Layer selector at the top (3 layers, named in JSON).
//   - For Layer 2/3: auto_when_mixer_visible checkbox.
//   - Reset-to-factory-defaults button (per layer).
//   - Sections grouped by hardware location.
//   - Per-row inline editor: type + action arg + behavior + param.
//
// Auto-saves on any change — setBinding writes JSON synchronously. The
// USB worker thread picks up the new binding on the next press through
// dispatch's lock-protected lookup.
//
// Per-strip Sel/Cut/Solo/Rec, V-Pot push, top soft-key, and soft-key
// bank selectors stay out of this UI in v1 (resolved Q2: hardcoded).
// Phase D widens the catalogue once the per-strip dispatch refactor lands.
void SettingsScreen::drawBindings(ImGui_Context* ctx)
{
    using namespace uf8::bindings;

    // ---- Top controls ----
    static int s_editLayer = 0;
    static char kLayerItems[] = "Layer 1\0Layer 2\0Layer 3\0";
    {
        double w = 160;
        ImGui_PushItemWidth(ctx, w);
        ImGui_Combo(ctx, "Editing layer", &s_editLayer, kLayerItems,
                    /*popup_max_height_in_items*/ nullptr);
        ImGui_PopItemWidth(ctx);
    }

    // Active-layer indicator + quick-switch button.
    {
        const int active = getActiveLayer();
        char line[64];
        std::snprintf(line, sizeof(line), "Active layer: %d", active + 1);
        ImGui_Text(ctx, line);
        ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
        if (ImGui_Button(ctx, "Activate this layer",
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            setActiveLayer(s_editLayer);
        }
    }

    // Layer 2/3: auto-switch when mixer visible. Architecture invariant
    // "at most one layer flagged" is enforced by setLayerAutoMixer.
    if (s_editLayer >= 1) {
        bool autoMixer = get().layers[s_editLayer].autoWhenMixerVisible;
        if (ImGui_Checkbox(ctx, "Auto-switch to this layer when mixer is open",
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

    // ---- Section headers + per-button rows ----
    for (const Section& sec : kSections) {
        if (ImGui_CollapsingHeader(ctx, sec.header,
                                   /*p_visible*/ nullptr,
                                   /*flags*/ nullptr)) {
            for (int i = 0; i < sec.nRows; ++i) {
                drawBindingRow(ctx, s_editLayer, sec.rows[i]);
            }
        }
    }

    ImGui_Separator(ctx);
    ImGui_Text(ctx, "Per-strip Select/Mute/Solo/Rec, V-Pot push, top soft-keys");
    ImGui_Text(ctx, "and soft-key bank selectors stay hardcoded in v1.");
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
