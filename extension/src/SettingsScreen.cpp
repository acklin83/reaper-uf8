#include "SettingsScreen.h"

#include "reaper_imgui_functions.h"

namespace uf8 {

namespace {

// ---- Tab: Device ----------------------------------------------------------
// Per docs/plan-settings-ui.md §"Tab: Device":
//   - Connected devices with status dots (UF8 #N, UC1 #N, …)
//   - LED brightness slider (writes the global-brightness frame)
//   - Scribble brightness slider
//   - Meter ballistic selector (PPM / VU / RMS) — UI-only until Phase 1
//     meter forwarding is implemented
//   - "SEL follows track color" toggle — needs the SEL-color frame capture
void drawDeviceTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Device");
    ImGui_Text(ctx, "  TODO: connected-device list with USB status dots");
    ImGui_Text(ctx, "  TODO: LED brightness slider (writes global-brightness frame)");
    ImGui_Text(ctx, "  TODO: scribble brightness slider");
    ImGui_Text(ctx, "  TODO: meter ballistic selector (PPM/VU/RMS)");
    ImGui_Text(ctx, "  TODO: SEL-follows-track-color toggle");
}

// ---- Tab: Bindings --------------------------------------------------------
// Consolidates docs/bindings.md §"Config UI Sketch (ReaImGui)" + §"Binding
// Types" + §"Builtin Action Catalogue (v1)".
// Persistence: ~/Library/Application Support/REAPER/rea_sixty/bindings.json
// Sections (single scroll):
//   - Per-strip buttons       (Select / Mute / Solo / Rec / V-Pot press)
//   - Transport               (Play / Stop / Rec / RW / FF — REAPER actions)
//   - Global buttons          (Bank L/R, Channel L/R, Flip, Layer cycle)
//   - Layer-scoped soft-keys  (per active layer)
//   - Learn button (top right)
void drawBindingsTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Bindings");
    ImGui_Text(ctx, "  TODO: per-strip target editor (select/mute/solo/rec/vpot_press)");
    ImGui_Text(ctx, "  TODO: transport bindings (REAPER action IDs)");
    ImGui_Text(ctx, "  TODO: global buttons (bank/channel L/R, flip, layer cycle)");
    ImGui_Text(ctx, "  TODO: soft-key bindings table per active layer");
    ImGui_Text(ctx, "  TODO: Learn-mode arming + ESC-to-cancel");
    ImGui_Text(ctx, "  TODO: import / export / reset-to-defaults");
}

// ---- Tab: Soft-Key Banks --------------------------------------------------
// Per memory uf8-softkey-banks.md: CS = 6 banks (V-POT + 1..5),
// BC = 2 banks (V-POT + 1). Display authoritative bank tables from
// main.cpp's `softkey::` namespace; let users wire `kNoSlot` positions
// to raw VST3 params (not in the SSL 360 Link map) or arbitrary REAPER
// actions with user-defined label + colour.
//
// Authoritative source: SSL UF8 User Guide p.180-181.
void drawSoftKeyBanksTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Soft-Key Banks (UF8 PM mode)");
    ImGui_Text(ctx, "  TODO: ChannelStrip 6-bank grid (V-POT + Bank 1..5)");
    ImGui_Text(ctx, "  TODO: BusComp 2-bank grid (V-POT + Bank 1)");
    ImGui_Text(ctx, "  TODO: kNoSlot wiring — raw VST3 param picker / REAPER action picker");
    ImGui_Text(ctx, "  TODO: per-position label + colour override");
    ImGui_Text(ctx, "  TODO: bank-follow-focus toggle (default ON)");
}

// ---- Tab: Modes -----------------------------------------------------------
// Phase 2.5 features per ROADMAP.md §"Phase 2.5":
//   2.5a Folder Mode          — long-press SEL toggles parent expand
//   2.5b Show Only Selected   — 8 selection slots persisted by GUID
//   2.5c Show Sends/Receives  — focus-variant Send Layer
//   2.5d Generic FX-param map — Learn-mode for any V-Pot/soft-button
void drawModesTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Modes (Phase 2.5 features)");
    ImGui_Text(ctx, "  TODO: Folder Mode — long-press duration slider, expand depth");
    ImGui_Text(ctx, "  TODO: Show Only Selected — auto-save toggle");
    ImGui_Text(ctx, "  TODO: Send / Receive Layer — enable per-direction");
    ImGui_Text(ctx, "  TODO: Generic FX Mapping — Learn modifier + display format");
}

// ---- Tab: Selection Sets --------------------------------------------------
// Per ROADMAP.md §"2.5b" + plan-settings-ui.md §"Tab: Selection Sets":
// 8 slots each holding a list of Track GUIDs. Project-scoped via
// SetProjExtState("rea_sixty", "selset_N", …). Editor lets user prune
// missing tracks manually and "save current selection" buttons.
void drawSelectionSetsTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Selection Sets");
    ImGui_Text(ctx, "  TODO: 8-slot grid (Slot 1..8)");
    ImGui_Text(ctx, "  TODO: per-slot name + Track-GUID list editor");
    ImGui_Text(ctx, "  TODO: 'save current selection' / 'recall' buttons");
    ImGui_Text(ctx, "  TODO: missing-track pruning UI");
}

// ---- Tab: About -----------------------------------------------------------
void drawAboutTab(ImGui_Context* ctx)
{
    ImGui_Text(ctx, "Rea-Sixty");
    ImGui_Text(ctx, "  Open-source SSL 360° replacement for UF8 / UC1");
    ImGui_Text(ctx, "");
    ImGui_Text(ctx, "  TODO: version, build hash, REAPER + ReaImGui versions");
    ImGui_Text(ctx, "  TODO: links — repo, ReaPack URL, issue tracker");
}

struct Tab {
    const char* label;
    void (*draw)(ImGui_Context*);
};

constexpr Tab kTabs[] = {
    { "Device",         drawDeviceTab        },
    { "Bindings",       drawBindingsTab      },
    { "Soft-Key Banks", drawSoftKeyBanksTab  },
    { "Modes",          drawModesTab         },
    { "Selection Sets", drawSelectionSetsTab },
    { "About",          drawAboutTab         },
};

} // namespace

void SettingsScreen::draw(ImGui_Context* ctx)
{
    if (!ImGui_BeginTabBar(ctx, "settings_tabs", /*flags*/ nullptr))
        return;

    for (const Tab& t : kTabs) {
        if (ImGui_BeginTabItem(ctx, t.label, /*p_open*/ nullptr, /*flags*/ nullptr)) {
            t.draw(ctx);
            ImGui_EndTabItem(ctx);
        }
    }

    ImGui_EndTabBar(ctx);
}

} // namespace uf8
