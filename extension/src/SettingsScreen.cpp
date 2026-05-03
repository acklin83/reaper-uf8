#include "SettingsScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Bindings.h"
#include "CsiImport.h"
#include "PluginMap.h"
#include "Protocol.h"
#include "UserPluginCatalog.h"
#include "reaper_imgui_functions.h"
#include "reaper_plugin_functions.h"

// Forward declarations of accessors defined in main.cpp. Same pattern as
// reasixty_followSelectedInMixer / reasixty_toggleMixerWindow — keeps the
// anonymous-namespace globals owned by main.cpp while letting the UI read
// runtime state. Called only from the main thread (via onTimer → ImGui).
bool reasixty_uf8Connected();
bool reasixty_uc1Connected();
const char* reasixty_uf8Serial();
const char* reasixty_uc1Serial();
// REAPER Action picker — Settings → Bindings editor uses these to drive
// REAPER's PromptForAction window and to resolve stored action strings
// to their human-readable names. Implemented in main.cpp; the poll runs
// off onTimer so the picker keeps working even if the user navigates
// away from the editor while the picker is open.
void reasixty_actionPickerStart(int layer, uf8::bindings::ButtonId id,
                                bool longPress);
bool reasixty_actionPickerActiveFor(int layer, uf8::bindings::ButtonId id,
                                    bool longPress);
void reasixty_actionPickerCancel();
std::string reasixty_resolveActionName(const std::string& action);
std::string reasixty_loadReaScript();
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
// Stock SSL plug-in soft-key labels for the read-only tabs.
// domain: 0 = ChannelStrip, 1 = BusComp.
const char* const* reasixty_softkeyStockLabels(int domain, int bank);
int                reasixty_softkeyStockBankCount();
// Currently active SSL soft-key PAGE bank — used by the schematic
// to highlight the matching V-POT/Bank tile and by the per-binding
// editor header to surface the live bank context.
int                reasixty_softkeyCurrentBank();
const char*        reasixty_softkeyCurrentBankName();
// Bindings save/load to user-chosen path. Both spawn a native file
// dialog and persist via uf8::bindings::exportLayerTo / importLayerFrom
// for the layer index passed in. Returns true on success, false on
// cancel or I/O error.
bool reasixty_exportLayerViaDialog(int layer);
bool reasixty_importLayerViaDialog(int layer);
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

// Short button-face label shown over the schematic (≈ what's printed on
// the physical UF8 silk-screen). Used by the editor header too.
const char* hwFaceLabel(ButtonId id)
{
    switch (id) {
        case ButtonId::BankLeft:    return "BANK \xE2\x97\x82";
        case ButtonId::BankRight:   return "BANK \xE2\x96\xB8";
        case ButtonId::PageLeft:    return "PAGE \xE2\x97\x82";
        case ButtonId::PageRight:   return "PAGE \xE2\x96\xB8";
        case ButtonId::Layer1:      return "1";
        case ButtonId::Layer2:      return "2";
        case ButtonId::Layer3:      return "3";
        case ButtonId::Quick1:      return "1";
        case ButtonId::Quick2:      return "2";
        case ButtonId::Quick3:      return "3";
        case ButtonId::PluginBtn:   return "PLUGIN";
        case ButtonId::Flip:        return "FLIP";
        case ButtonId::Pan:         return "PAN";
        case ButtonId::Fine:        return "FINE";
        case ButtonId::Btn360:      return "360\xC2\xB0";
        case ButtonId::AutoOff:     return "OFF";
        case ButtonId::AutoRead:    return "READ";
        case ButtonId::AutoWrite:   return "WRITE";
        case ButtonId::AutoTrim:    return "TRIM";
        case ButtonId::AutoLatch:   return "LATCH";
        case ButtonId::AutoTouch:   return "TOUCH";
        case ButtonId::ZoomUp:      return "\xE2\x96\xB2";
        case ButtonId::ZoomDown:    return "\xE2\x96\xBC";
        case ButtonId::ZoomLeft:    return "\xE2\x97\x82";
        case ButtonId::ZoomRight:   return "\xE2\x96\xB8";
        case ButtonId::ZoomCenter:  return "\xE2\x97\x8F";
        case ButtonId::Nav:         return "NAV";
        case ButtonId::Nudge:       return "NUDGE";
        case ButtonId::EncFocus:    return "FOCUS";
        case ButtonId::ChannelPush: return "ENC PUSH";
        case ButtonId::Channel:     return "CHANNEL";
        case ButtonId::SendPlugin1: return "S/P 1";
        case ButtonId::SendPlugin2: return "S/P 2";
        case ButtonId::SendPlugin3: return "S/P 3";
        case ButtonId::SendPlugin4: return "S/P 4";
        case ButtonId::SendPlugin5: return "S/P 5";
        case ButtonId::SendPlugin6: return "S/P 6";
        case ButtonId::SendPlugin7: return "S/P 7";
        case ButtonId::SendPlugin8: return "S/P 8";
        case ButtonId::TopSoftKey1: return "Soft-Key 1";
        case ButtonId::TopSoftKey2: return "Soft-Key 2";
        case ButtonId::TopSoftKey3: return "Soft-Key 3";
        case ButtonId::TopSoftKey4: return "Soft-Key 4";
        case ButtonId::TopSoftKey5: return "Soft-Key 5";
        case ButtonId::TopSoftKey6: return "Soft-Key 6";
        case ButtonId::TopSoftKey7: return "Soft-Key 7";
        case ButtonId::TopSoftKey8: return "Soft-Key 8";
        case ButtonId::VPotBank:     return "V-POT";
        case ButtonId::SoftKey1Bank: return "BANK 1";
        case ButtonId::SoftKey2Bank: return "BANK 2";
        case ButtonId::SoftKey3Bank: return "BANK 3";
        case ButtonId::SoftKey4Bank: return "BANK 4";
        case ButtonId::SoftKey5Bank: return "BANK 5";
        default:                     return uf8::bindings::toName(id);
    }
}

// Convenience for ImGui_SameLine with default args.
void sameLine(ImGui_Context* ctx)
{
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
}

// Vector-graphics canvas for the UF8 schematic — drawn into the
// surrounding ImGui window's draw list. Coordinates are in the
// schematic's local 0..W × 0..H space; ox/oy translate to screen.
struct VCanvas {
    ImGui_Context*  ctx;
    ImGui_DrawList* dl;
    float           ox, oy;
};

void drawText_(VCanvas& c, float x, float y, uint32_t col, const char* text)
{
    ImGui_DrawList_AddText(c.dl, c.ox + x, c.oy + y, col, text);
}

void drawTextCentered_(VCanvas& c, float cx, float cy, uint32_t col,
                       const char* text)
{
    double tw = 0, th = 0;
    ImGui_CalcTextSize(c.ctx, text, &tw, &th, /*hide_after_##*/ nullptr,
                       /*wrap_width*/ nullptr);
    drawText_(c, cx - float(tw) / 2.0f, cy - float(th) / 2.0f, col, text);
}

void rect_(VCanvas& c, float x, float y, float w, float h,
           uint32_t fill, uint32_t border, double rounding = 3.0)
{
    const float x1 = c.ox + x, y1 = c.oy + y;
    const float x2 = x1 + w,   y2 = y1 + h;
    if (fill) {
        ImGui_DrawList_AddRectFilled(c.dl, x1, y1, x2, y2, fill,
                                     &rounding, /*flags*/ nullptr);
    }
    if (border) {
        ImGui_DrawList_AddRect(c.dl, x1, y1, x2, y2, border,
                               &rounding, /*flags*/ nullptr,
                               /*thickness*/ nullptr);
    }
}

void circle_(VCanvas& c, float cx, float cy, float r,
             uint32_t fill, uint32_t border)
{
    if (fill) {
        ImGui_DrawList_AddCircleFilled(c.dl, c.ox + cx, c.oy + cy, r,
                                       fill, /*num_segments*/ nullptr);
    }
    if (border) {
        ImGui_DrawList_AddCircle(c.dl, c.ox + cx, c.oy + cy, r,
                                 border, /*num_segments*/ nullptr,
                                 /*thickness*/ nullptr);
    }
}

void line_(VCanvas& c, float x1, float y1, float x2, float y2,
           uint32_t col, double thickness = 1.0)
{
    ImGui_DrawList_AddLine(c.dl, c.ox + x1, c.oy + y1,
                           c.ox + x2, c.oy + y2, col, &thickness);
}

// Render the full UF8 schematic. Click hit-test goes against the
// canvas-wide InvisibleButton; per-rect hits are computed by comparing
// the cached mouse-coords against each button's local rectangle.
// `sel` is updated on click. Layout follows SSL UF8 User Guide page 14
// (numbered controls).
void drawUf8Vector(ImGui_Context* ctx, ButtonId& sel)
{
    constexpr float W = 1000, H = 490;

    double oxd = 0, oyd = 0;
    ImGui_GetCursorScreenPos(ctx, &oxd, &oyd);

    // Reserve canvas layout space + provide a single hit target for
    // click capture. Per-button hit-testing happens manually against
    // the cached mouse position below.
    ImGui_InvisibleButton(ctx, "uf8_canvas", W, H, /*flags*/ nullptr);
    const bool canvasHovered = ImGui_IsItemHovered(ctx, /*flags*/ nullptr);
    int leftBtn = 0;
    const bool canvasClicked = ImGui_IsItemClicked(ctx, &leftBtn);

    double mxd = 0, myd = 0;
    ImGui_GetMousePos(ctx, &mxd, &myd);

    VCanvas c {
        ctx, ImGui_GetWindowDrawList(ctx),
        static_cast<float>(oxd), static_cast<float>(oyd)
    };
    const float mx = static_cast<float>(mxd) - c.ox;
    const float my = static_cast<float>(myd) - c.oy;

    auto inside = [&](float x, float y, float w, float h) {
        return canvasHovered
            && mx >= x && mx <= x + w
            && my >= y && my <= y + h;
    };

    // Bindable button: hit-tests against the canvas mouse, draws a
    // hardware-face rectangle, highlights on hover/select. Returns
    // true when this tile was clicked this frame so the caller can
    // optionally fire the binding's action (used by the V-POT/Bank
    // tiles so clicking them in the schematic actually switches the
    // SSL PAGE bank — a hardware proxy for that one row).
    auto drawHwBtn = [&](float x, float y, float w, float h,
                         ButtonId id, const char* label) -> bool
    {
        const bool hot      = inside(x, y, w, h);
        const bool selected = (id == sel);
        const bool clicked  = hot && canvasClicked && leftBtn == 0;
        if (clicked) sel = id;

        const uint32_t fill   = selected ? 0x4477CCFF
                                : hot     ? 0x3A4253FF
                                          : 0x252A33FF;
        const uint32_t border = selected ? 0xAACCFFFF : 0x4A5060FF;
        const uint32_t txt    = selected ? 0xFFFFFFFF : 0xD0D4DAFF;
        rect_(c, x, y, w, h, fill, border, /*rounding*/ 3.5);
        drawTextCentered_(c, x + w / 2.0f, y + h / 2.0f, txt, label);
        return clicked;
    };

    // Locked (non-bindable in v1) button — flatter colour, no hover.
    auto drawLocked = [&](float x, float y, float w, float h,
                          const char* label)
    {
        rect_(c, x, y, w, h, 0x1A1E24FF, 0x383C44FF, 3.0);
        drawTextCentered_(c, x + w / 2.0f, y + h / 2.0f, 0x70747CFF, label);
    };

    // Group label — small all-caps text painted on the chassis above
    // a related cluster of controls (mirrors the SSL silk-screen).
    auto drawGroupLabel = [&](float x, float y, const char* text) {
        drawText_(c, x, y, 0x9CA0AAFF, text);
    };

    // ---- Chassis ----
    rect_(c, 4, 4, W - 8, H - 8, 0x14181EFF, 0x2A3038FF, /*rounding*/ 8.0);

    // ---- Centre: 8 strips ----
    constexpr float kStripX0 = 138, kStripW = 80, kStripGap = 7;
    constexpr ButtonId kStripTsk[8] = {
        ButtonId::TopSoftKey1, ButtonId::TopSoftKey2,
        ButtonId::TopSoftKey3, ButtonId::TopSoftKey4,
        ButtonId::TopSoftKey5, ButtonId::TopSoftKey6,
        ButtonId::TopSoftKey7, ButtonId::TopSoftKey8,
    };
    // Live SSL labels for the active PAGE bank — the top-soft-key
    // scribble area shows whatever the hardware would show right now.
    const int sslBank = reasixty_softkeyCurrentBank();
    const char* const* sslLabels =
        reasixty_softkeyStockLabels(/*domain*/ 0, sslBank);
    const int activeLayer = uf8::bindings::getActiveLayer();
    for (int i = 0; i < 8; ++i) {
        const float sx = kStripX0 + i * (kStripW + kStripGap);
        // Top soft-key — clickable so the user can edit the per-strip
        // binding directly from the schematic.
        char tlbl[4];
        std::snprintf(tlbl, sizeof(tlbl), "%d", i + 1);
        drawHwBtn(sx + 6, 12, kStripW - 12, 22, kStripTsk[i], tlbl);
        // Scribble LCD — show the live top-soft-key label. Resolution
        // mirrors the runtime render path:
        //   1. binding.shortPress[Plain].label   (user override)
        //   2. SSL plug-in's softkey label for the current bank slot —
        //      only when the binding is ssl_softkey (otherwise an empty
        //      slot on a fresh layer would still display SSL labels)
        //   3. blank
        rect_(c, sx + 4, 40, kStripW - 8, 58, 0x080C12FF, 0x444A55FF, 2.0);
        std::string scribble;
        {
            const auto bd =
                uf8::bindings::getBinding(activeLayer, kStripTsk[i]);
            const auto& sp = bd.shortPress[
                static_cast<int>(uf8::bindings::Modifier::Plain)];
            const bool isSslSoftkey =
                sp.type == uf8::bindings::ActionType::Builtin &&
                sp.action == "ssl_softkey";
            if (!sp.label.empty()) {
                scribble = sp.label;
            } else if (isSslSoftkey && sslLabels && sslLabels[i] && *sslLabels[i]) {
                scribble = sslLabels[i];
            }
        }
        if (scribble.size() > 10) scribble.resize(10);
        if (!scribble.empty()) {
            drawTextCentered_(c, sx + kStripW / 2.0f, 68,
                              0x4488DDFF, scribble.c_str());
        }
        // V-Pot (large dial with notch)
        const float vx = sx + kStripW / 2.0f, vy = 124;
        circle_(c, vx, vy, 18, 0x14181EFF, 0x4A5060FF);
        circle_(c, vx, vy, 14, 0x2A3038FF, 0x555A66FF);
        line_(c, vx, vy - 16, vx, vy - 8, 0xCCCCCCFF, 2.0);
        // Solo / Cut / Sel (locked, per-strip)
        drawLocked(sx + 8, 152, kStripW - 16, 16, "SOLO");
        drawLocked(sx + 8, 172, kStripW - 16, 16, "CUT");
        drawLocked(sx + 8, 192, kStripW - 16, 16, "SEL");
        // Fader: scale ticks + track + cap
        const float fx = sx + kStripW / 2.0f;
        const float fyTop = 220, fyBot = 440;
        // Scale tick marks (left side)
        for (int t = 0; t <= 10; ++t) {
            const float ty = fyTop + (fyBot - fyTop) * (t / 10.0f);
            const float len = (t % 5 == 0) ? 6.0f : 3.0f;
            line_(c, fx - 12, ty, fx - 12 + len, ty, 0x6A6E78FF, 1.0);
        }
        // Track
        rect_(c, fx - 1.5f, fyTop, 3, fyBot - fyTop, 0x444B55FF, 0x000000FF, 1.0);
        // Cap (fixed at unity-ish)
        const float capY = fyTop + (fyBot - fyTop) * 0.40f;
        rect_(c, fx - 12, capY - 7, 24, 14, 0x6A7080FF, 0x9CA0AAFF, 2.5);
        line_(c, fx - 9, capY, fx + 9, capY, 0xE0E0E0FF, 1.5);
    }

    // ---- Left panel ----
    // Group labels need a >= 14 px vertical gap to the button row below
    // (default font height ≈ 13 — anything tighter clips the descenders
    // into the button border).
    drawGroupLabel(20, 6, "LAYER");
    drawGroupLabel(64, 6, "QUICK");
    drawHwBtn(15, 22, 36, 22, ButtonId::Layer1, "1");
    drawHwBtn(15, 48, 36, 22, ButtonId::Layer2, "2");
    drawHwBtn(15, 74, 36, 22, ButtonId::Layer3, "3");
    drawHwBtn(57, 22, 36, 22, ButtonId::Quick1, "1");
    drawHwBtn(57, 48, 36, 22, ButtonId::Quick2, "2");
    drawHwBtn(57, 74, 36, 22, ButtonId::Quick3, "3");

    drawHwBtn(15, 108, 78, 24, ButtonId::Btn360, "360\xC2\xB0");

    drawGroupLabel(15, 136, "SEND / PLUGIN");
    {
        constexpr ButtonId kSp[8] = {
            ButtonId::SendPlugin1, ButtonId::SendPlugin2,
            ButtonId::SendPlugin3, ButtonId::SendPlugin4,
            ButtonId::SendPlugin5, ButtonId::SendPlugin6,
            ButtonId::SendPlugin7, ButtonId::SendPlugin8,
        };
        char buf[8];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 2; ++col) {
                const int idx = row * 2 + col;
                std::snprintf(buf, sizeof(buf), "%d", idx + 1);
                drawHwBtn(13 + col * 42, 152 + row * 26, 38, 22,
                          kSp[idx], buf);
            }
        }
    }

    // PLUGIN / CHANNEL — wider buttons so their silk-screen labels fit.
    drawHwBtn(13, 260, 50, 22, ButtonId::PluginBtn, "PLUGIN");
    drawHwBtn(67, 260, 50, 22, ButtonId::Channel,   "CHANNEL");

    // PAGE — label sits 18 px above the arrow row so the descenders
    // don't touch the button frame.
    drawGroupLabel(48, 282, "PAGE");
    drawHwBtn(13, 300, 50, 22, ButtonId::PageLeft,  "\xE2\x97\x82");
    drawHwBtn(67, 300, 50, 22, ButtonId::PageRight, "\xE2\x96\xB8");

    drawHwBtn(13, 326, 104, 22, ButtonId::Flip, "FLIP");

    drawGroupLabel(36, 354, "AUTOMATION");
    // 3 columns × 2 rows. 33-wide buttons fit "WRITE" / "LATCH" / "TOUCH"
    // without overflow at the default ImGui font.
    drawHwBtn(13,  372, 33, 22, ButtonId::AutoOff,   "OFF");
    drawHwBtn(48,  372, 33, 22, ButtonId::AutoRead,  "READ");
    drawHwBtn(83,  372, 33, 22, ButtonId::AutoWrite, "WRITE");
    drawHwBtn(13,  396, 33, 22, ButtonId::AutoTrim,  "TRIM");
    drawHwBtn(48,  396, 33, 22, ButtonId::AutoLatch, "LATCH");
    drawHwBtn(83,  396, 33, 22, ButtonId::AutoTouch, "TOUCH");

    // ---- Right panel ----
    // SSL soft-key bank selectors — 2×3 grid. Each switches the active
    // PAGE bank (V-POT / Bank 1..5) via the softkey_bank_select
    // builtin. User can rebind to anything via the editor. The
    // currently-active bank gets an extra green ring around its tile
    // so the user always sees which bank the top-soft-key labels are
    // sourced from right now.
    {
        const int activeBank = reasixty_softkeyCurrentBank();
        struct BankBtn { float x, y, w; ButtonId id; const char* lbl; int idx; };
        const BankBtn banks[6] = {
            {852, 22, 42, ButtonId::VPotBank,     "V-POT", 0},
            {898, 22, 42, ButtonId::SoftKey1Bank, "1",     1},
            {944, 22, 41, ButtonId::SoftKey2Bank, "2",     2},
            {852, 46, 42, ButtonId::SoftKey3Bank, "3",     3},
            {898, 46, 42, ButtonId::SoftKey4Bank, "4",     4},
            {944, 46, 41, ButtonId::SoftKey5Bank, "5",     5},
        };
        for (auto& b : banks) {
            const bool justClicked = drawHwBtn(b.x, b.y, b.w, 20, b.id, b.lbl);
            if (justClicked) {
                // Hardware proxy: fire the binding's action so the
                // schematic click actually switches the SSL PAGE bank
                // (g_softKeyBank), exactly like pressing the hardware
                // button would.
                uf8::bindings::dispatch(b.id, /*pressed*/ true);
                uf8::bindings::dispatch(b.id, /*pressed*/ false);
            }
            if (b.idx == activeBank) {
                // Inset green outline to mark the live bank without
                // fighting the existing select/hover highlight.
                ImGui_DrawList_AddRect(c.dl,
                    c.ox + b.x - 2, c.oy + b.y - 2,
                    c.ox + b.x + b.w + 2, c.oy + b.y + 22,
                    0x40C040FF, /*rounding*/ nullptr,
                    /*flags*/ nullptr, /*thickness*/ nullptr);
            }
        }
    }

    // PAN + FINE — own row below SOFT KEYS, with breathing room.
    drawHwBtn(852, 80, 64, 22, ButtonId::Pan,  "PAN");
    drawHwBtn(921, 80, 64, 22, ButtonId::Fine, "FINE");

    drawGroupLabel(852, 112, "SELECTION MODE");
    drawLocked(852, 128, 43, 20, "NORM");
    drawLocked(899, 128, 43, 20, "REC");
    drawLocked(946, 128, 39, 20, "AUTO");

    // CHANNEL encoder — clickable hit-area covers the dial body so users
    // can edit the encoder push binding directly from the schematic.
    // Label centered over the dial.
    {
        constexpr float cx = 918, cy = 200, r = 32;
        // Hit-area spans the dial bounds. Selecting drives the
        // ChannelPush binding.
        const float hbx = cx - r, hby = cy - r;
        const float hbw = 2 * r, hbh = 2 * r;
        const bool hot      = inside(hbx, hby, hbw, hbh);
        const bool selected = (ButtonId::ChannelPush == sel);
        if (hot && canvasClicked && leftBtn == 0) sel = ButtonId::ChannelPush;

        const uint32_t edge = selected ? 0xAACCFFFF
                              : hot     ? 0x6688AAFF
                                        : 0x4A5060FF;
        circle_(c, cx, cy, r,        0x14181EFF, edge);
        circle_(c, cx, cy, r - 3,    0x252A33FF, 0x555A66FF);
        circle_(c, cx, cy, r * 0.78f, 0x383C44FF, 0x6A6E78FF);
        line_(c, cx, cy - r * 0.95f, cx, cy - r * 0.62f, 0xE0E0E0FF, 2.5);
        for (int k = 0; k < 24; ++k) {
            const float ang = (k / 24.0f) * 6.2831853f - 1.5707963f;
            const float r0 = r - 4, r1 = r - 1;
            const float x1 = cx + std::cos(ang) * r0;
            const float y1 = cy + std::sin(ang) * r0;
            const float x2 = cx + std::cos(ang) * r1;
            const float y2 = cy + std::sin(ang) * r1;
            line_(c, x1, y1, x2, y2, 0x555A66FF, 1.0);
        }
        // Centered CHANNEL label above the dial.
        drawTextCentered_(c, cx, 158, 0x9CA0AAFF, "CHANNEL");
    }

    // NAV / NUDGE / FOCUS — sit below the encoder. Encoder Push has its
    // own click target on the dial above (kept the labelled bar too so
    // users still see the binding name explicitly).
    drawHwBtn(852, 244, 44, 22, ButtonId::Nav,      "NAV");
    drawHwBtn(898, 244, 44, 22, ButtonId::Nudge,    "NUDGE");
    drawHwBtn(944, 244, 41, 22, ButtonId::EncFocus, "FOCUS");

    drawHwBtn(852, 270, 133, 18, ButtonId::ChannelPush, "ENCODER PUSH");

    drawGroupLabel(902, 298, "BANK");
    drawHwBtn(870, 314, 42, 22, ButtonId::BankLeft,  "\xE2\x97\x82");
    drawHwBtn(922, 314, 42, 22, ButtonId::BankRight, "\xE2\x96\xB8");

    // Zoom pad — cross. Shifted down to follow the encoder column.
    {
        constexpr float cx = 918;
        constexpr float baseY = 372;
        drawHwBtn(cx - 17, baseY - 32, 34, 26, ButtonId::ZoomUp,
                  "\xE2\x96\xB2");
        drawHwBtn(cx - 54, baseY,      34, 26, ButtonId::ZoomLeft,
                  "\xE2\x97\x82");
        drawHwBtn(cx - 17, baseY,      34, 26, ButtonId::ZoomCenter,
                  "\xE2\x97\x8F");
        drawHwBtn(cx + 20, baseY,      34, 26, ButtonId::ZoomRight,
                  "\xE2\x96\xB8");
        drawHwBtn(cx - 17, baseY + 32, 34, 26, ButtonId::ZoomDown,
                  "\xE2\x96\xBC");
    }

    // Brand line — replaces the SSL silk-screen with our product name.
    drawTextCentered_(c, 500, 470, 0x9CA0AAFF, "Rea-Sixty");
}

// Render the full UC1 schematic. Layout follows the SSL UC1 hardware
// (User Guide page 14 + reference photo): three vertical zones —
// Filters/EQ left, Bus-Compressor + Central Control Panel centre,
// Dynamics + Channel right.
//
// EQ + BC + Dynamics each use a 2-column knob layout:
//   EQ:   Gain & Q in column 1, Freq in column 2.
//         EQ Type / EQ In are two small toggles stacked in column 2
//         between bands. HF Bell + LF Bell sit diagonal next to their
//         band's Gain knob.
//   BC:   Threshold / Attack / Ratio / S/C HPF in column 1;
//         Make-Up / Release / IN-toggle / Mix in column 2.
//         Analog VU meter spans both columns at the top.
//   Comp: Ratio + Release column 1, Threshold column 2 (mid-row).
//         Fast-Attack + Peak toggles tucked in the top-right corner.
//         Dyn-IN toggle + GR meter row below the knob block.
//   Gate: Range + Release column 1, Threshold + Hold column 2.
//         Expand + Fast-Attack-Gate stacked beneath.
//   Channel section bottom: Polarity / S/C Listen / Solo Clear in a
//         small column on the left, then SOLO / CUT / FINE as the
//         large bottom row, with Channel-IN large above FINE.
//
// Colour codes follow the silk-screen: red Gain caps on HF, green on
// HMF, blue on LMF, grey on LF + filters + dynamics. Decorative-only;
// no hit-targets in this iteration.
void drawUc1Vector(ImGui_Context* ctx)
{
    constexpr float W = 860, H = 660;

    double oxd = 0, oyd = 0;
    ImGui_GetCursorScreenPos(ctx, &oxd, &oyd);
    ImGui_InvisibleButton(ctx, "uc1_canvas", W, H, /*flags*/ nullptr);

    VCanvas c {
        ctx, ImGui_GetWindowDrawList(ctx),
        static_cast<float>(oxd), static_cast<float>(oyd)
    };

    // Helpers --------------------------------------------------------------
    auto knob = [&](float cx, float cy, float r, uint32_t cap,
                    const char* label = nullptr)
    {
        // Outer ring + inner cap, indicator at 12 o'clock.
        circle_(c, cx, cy, r,        0x14181EFF, 0x4A5060FF);
        circle_(c, cx, cy, r * 0.78f, cap,        0x80808055);
        line_(c,  cx, cy - r * 0.85f, cx, cy - r * 0.45f, 0xE8E8E8FF, 1.6);
        if (label) {
            drawTextCentered_(c, cx, cy + r + 9, 0xB8BCC4FF, label);
        }
    };
    auto btn = [&](float x, float y, float w, float h, const char* label,
                   uint32_t accent = 0x252A33FF)
    {
        rect_(c, x, y, w, h, accent, 0x4A5060FF, 3.0);
        drawTextCentered_(c, x + w / 2.0f, y + h / 2.0f, 0xD0D4DAFF, label);
    };
    auto sectionLabel = [&](float x, float y, const char* text) {
        drawText_(c, x, y, 0x9CA0AAFF, text);
    };

    constexpr uint32_t kRedCap    = 0xC03038FF;  // HF Gain
    constexpr uint32_t kGreenCap  = 0x408840FF;  // HMF Gain + Q
    constexpr uint32_t kBlueCap   = 0x4070C0FF;  // LMF Gain + Q
    constexpr uint32_t kGreyCap   = 0x6A707CFF;  // filters + dynamics
    constexpr uint32_t kBlackCap  = 0x101418FF;  // LF Gain + Freq
    constexpr uint32_t kFreqCap   = 0x3A4150FF;  // Freq knobs (darker grey)
    constexpr uint32_t kAccentBC  = 0x2A4870FF;  // Bus Comp section accent
    constexpr uint32_t kAccentCC  = 0x903030FF;  // Central Control accent

    // Chassis -------------------------------------------------------------
    rect_(c, 4, 4, W - 8, H - 8, 0x14181EFF, 0x2A3038FF, /*rounding*/ 8.0);

    constexpr float kSmallToggleW = 28, kSmallToggleH = 14;
    auto smallToggle = [&](float x, float y, const char* label) {
        rect_(c, x, y, kSmallToggleW, kSmallToggleH,
              0x252A33FF, 0x4A5060FF, 2.5);
        drawTextCentered_(c, x + kSmallToggleW / 2.0f,
                              y + kSmallToggleH / 2.0f,
                              0xC0C4CCFF, label);
    };

    // Mid-sized toggle — same footprint as the Channel section's
    // POLARITY/S-C LISTEN/SOLO CLR buttons (66×22). Used for the four
    // dynamics-section toggles (FAST ATK COMP / PEAK / EXPAND /
    // FAST ATK GATE).
    constexpr float kDynBtnW = 66, kDynBtnH = 22;
    auto dynBtn = [&](float x, float y, const char* label) {
        rect_(c, x, y, kDynBtnW, kDynBtnH, 0x252A33FF, 0x4A5060FF, 3.0);
        drawTextCentered_(c, x + kDynBtnW / 2.0f, y + kDynBtnH / 2.0f,
                              0xC0C4CCFF, label);
    };

    // ---- Left column: Filters + EQ -------------------------------------
    // 2-column layout: Gain & Q live in column 1 (cx=60), Freq in
    // column 2 (cx=170). Lo-Pass and Hi-Pass (no IN buttons!) sit at
    // the top, diagonal to each other. EQ Type + EQ In are two small
    // toggles stacked in column 2 between HMF and LMF; HF/LF Bell
    // toggles sit diagonally next to their band's Gain knob.
    constexpr float kColLx = 12, kColLw = 230;
    rect_(c, kColLx, 12, kColLw, H - 24, 0x1A1E24FF, 0x2A3038FF, 6.0);

    // Filters: Lo-Pass column 1 (top-left), Hi-Pass column 2 diagonal
    // below it. NO IN buttons — they don't exist on UC1.
    knob(kColLx + 60,  44, 20, kGreyCap, "LO-PASS");
    knob(kColLx + 170, 70, 20, kGreyCap, "HI-PASS");
    sectionLabel(kColLx + 14, 110, "FILTERS");
    line_(c, kColLx + 70, 122, kColLx + kColLw - 8, 122, 0x383C44FF, 1.0);

    // HF band — Gain in col 1, Freq in col 2 diagonal, HF Bell toggle
    // beside the Freq knob. Knob spacing 28 px (was 24) for a calmer
    // visual rhythm matching the LMF band below.
    drawText_(c, kColLx + 14, 152, 0xB8BCC4FF, "HF");
    knob(kColLx + 60,  154, 20, kRedCap, "GAIN");
    knob(kColLx + 170, 182, 20, kRedCap, "FREQ");
    smallToggle(kColLx + 192, 146, "BELL");

    // HMF band — Gain + Q in col 1, Freq in col 2. Q matches the
    // band's silk-screen colour (green). Knob spacing GAIN→FREQ 28,
    // FREQ→Q 38 (slightly more than before so the diagonals breathe).
    drawText_(c, kColLx + 14, 230, 0xB8BCC4FF, "HMF");
    knob(kColLx + 60,  232, 20, kGreenCap, "GAIN");
    knob(kColLx + 170, 260, 20, kGreenCap, "FREQ");
    knob(kColLx + 60,  300, 20, kGreenCap, "Q");

    // EQ Type + EQ In: stacked in column 2 deep in the gap between
    // HMF and LMF, with 16-20 px breathing above TYPE and below IN.
    smallToggle(kColLx + 192, 356, "TYPE");
    smallToggle(kColLx + 192, 376, "IN");

    // LMF band — Gain + Q in col 1, Freq in col 2. Q in band-blue.
    // Whole block shifted +32 vs HMF baseline so TYPE/IN get their
    // breathing room.
    drawText_(c, kColLx + 14, 428, 0xB8BCC4FF, "LMF");
    knob(kColLx + 60,  430, 20, kBlueCap, "GAIN");
    knob(kColLx + 170, 458, 20, kBlueCap, "FREQ");
    knob(kColLx + 60,  498, 20, kBlueCap, "Q");

    // LF band — both controls in black per silk-screen. Layout
    // mirrors LMF's Q+FREQ diagonal (Freq upper-right, Gain lower-
    // left), and LF Bell mirrors the HF Bell — HF Bell sits at the
    // level of HF Gain (above HF Freq); LF Bell sits at the level
    // of LF Gain (below LF Freq).
    drawText_(c, kColLx + 14, 556, 0xB8BCC4FF, "LF");
    knob(kColLx + 170, 558, 20, kBlackCap, "FREQ");
    knob(kColLx + 60,  598, 20, kBlackCap, "GAIN");
    smallToggle(kColLx + 192, 591, "BELL");

    // ---- Centre column: Bus Comp (top) + Central Control (bottom) ------
    // Wider than the side columns so the BC knob block can hold an
    // Input-Gain knob to the left of S/C HPF and an Output-Gain knob
    // to the right of MIX, both at the same y as the bottom BC row.
    constexpr float kColCx = kColLx + kColLw + 8, kColCw = 360;
    rect_(c, kColCx, 12, kColCw, 420, 0x1A1E24FF, kAccentBC, 6.0);
    drawTextCentered_(c, kColCx + kColCw / 2.0f, 22,
                      0x9CA0AAFF, "BUS COMPRESSOR");

    // SSL Bus Comp GR meter — LCD-black face, blue scale, 0 left →
    // 20 right (gain-reduction in dB, NOT input level). No red zone:
    // GR meters don't carry the analog-VU "hot" convention. Pivot
    // sits 3 px above the face bottom so the pivot dot stays inside
    // the face and the needle never protrudes.
    {
        const float mw = 196.0f, mh = 80.0f;
        const float mx = kColCx + (kColCw - mw) / 2.0f, my = 44.0f;
        // Outer bezel + LCD-black face inset (palette matches the
        // 7-seg / LCD blocks in the Central Control panel).
        rect_(c, mx, my, mw, mh, 0x141416FF, 0x282A2EFF, 4.0);
        rect_(c, mx + 4, my + 4, mw - 8, mh - 8,
              0x080C12FF, 0x444A55FF, 2.0);
        const float mcx = mx + mw / 2.0f;
        const float mcy = my + mh - 3;             // pivot inside face
        const float ra  = 70;                      // outer scale radius
        // Sweep -130° to -50° (90° arc)
        const float a0 = -3.14159265f * 130.0f / 180.0f;
        const float a1 = -3.14159265f *  50.0f / 180.0f;
        auto dBtoA = [&](float dB) {
            const float t = dB / 20.0f;             // 0..20
            return a0 + t * (a1 - a0);
        };
        constexpr uint32_t kVuBlue = 0x4499DDFF;    // matches LCD text
        // Tick marks: majors at 0/5/10/15/20, minors at 1/2/3/7
        struct Tick { float dB; const char* label; };
        const Tick ticks[] = {
            {0, "0"}, {1, ""}, {2, ""}, {3, ""}, {5, "5"},
            {7, ""}, {10, "10"}, {15, "15"}, {20, "20"}
        };
        for (const Tick& t : ticks) {
            const float a = dBtoA(t.dB);
            const float tlen = t.label[0] ? 8.0f : 5.0f;
            const float x1 = mcx + std::cos(a) * ra,
                        y1 = mcy + std::sin(a) * ra;
            const float x2 = mcx + std::cos(a) * (ra - tlen),
                        y2 = mcy + std::sin(a) * (ra - tlen);
            line_(c, x1, y1, x2, y2, kVuBlue, 1.6);
            if (t.label[0]) {
                const float lx = mcx + std::cos(a) * (ra - 16);
                const float ly = mcy + std::sin(a) * (ra - 16);
                drawTextCentered_(c, lx, ly, kVuBlue, t.label);
            }
        }
        // Needle — mock reading at 7 dB of gain reduction
        const float aN = dBtoA(7.0f);
        line_(c, mcx, mcy,
                 mcx + std::cos(aN) * (ra - 4),
                 mcy + std::sin(aN) * (ra - 4),
                 kVuBlue, 2.0);
        circle_(c, mcx, mcy, 3, kVuBlue, 0);
        // "GR" badge inside the face
        drawTextCentered_(c, mcx, my + mh - 12, kVuBlue, "GR");
    }

    // BC knob 4×2 grid centred in the wider column, plus Input-Gain
    // and Output-Gain flanking the bottom row at the same y as
    // S/C HPF / MIX (per Frank's UC1 layout note).
    //   col 1 (THR / ATTACK / RATIO / S/C HPF)
    //   col 2 (MAKE-UP / RELEASE / IN-toggle / MIX)
    //   bottom-row only: INPUT-GAIN flanks col 1 on the left,
    //                    OUTPUT-GAIN flanks col 2 on the right.
    {
        const float c1x = kColCx + 120, c2x = kColCx + 240;
        const float cInL = kColCx + 60,  cInR = kColCx + 300;
        const float ry[4] = { 172, 234, 296, 358 };
        knob(c1x, ry[0], 20, kAccentBC, "THR");
        knob(c2x, ry[0], 20, kAccentBC, "MAKE-UP");
        knob(c1x, ry[1], 20, kAccentBC, "ATTACK");
        knob(c2x, ry[1], 20, kAccentBC, "RELEASE");
        knob(c1x, ry[2], 20, kAccentBC, "RATIO");
        // BC IN — toggle button instead of a knob (per UC1 hardware).
        // Label sits 8 px below the button bottom so the text top
        // doesn't bump the box edge.
        rect_(c, c2x - 14, ry[2] - 14, 28, 28, 0xE0E0E0FF, 0x808088FF, 3.0);
        drawTextCentered_(c, c2x, ry[2] + 26, 0x9CA0AAFF, "IN");
        knob(c1x,  ry[3], 20, kAccentBC, "S/C HPF");
        knob(c2x,  ry[3], 20, kAccentBC, "MIX");
        knob(cInL, ry[3], 20, kAccentBC, "INPUT");
        knob(cInR, ry[3], 20, kAccentBC, "OUTPUT");
    }

    // Central Control Panel
    rect_(c, kColCx, 440, kColCw, H - 452, 0x1A1E24FF, kAccentCC, 6.0);
    // 7-segment display (top-left)
    rect_(c, kColCx + 14, 454, 56, 30, 0x1A0408FF, 0x401818FF, 3.0);
    drawTextCentered_(c, kColCx + 42, 468, 0xFF3030FF, "001");
    // LCD (centre, with mock content). Width grows with the wider
    // BC column; recompute the label-x at the LCD's new horizontal
    // centre instead of the old +138 hardcode.
    {
        const float lcdX = kColCx + 78, lcdW = kColCw - 138;
        const float lcdCx = lcdX + lcdW / 2.0f;
        rect_(c, lcdX, 452, lcdW, 76, 0x05080CFF, 0x444A55FF, 3.0);
        drawTextCentered_(c, lcdCx, 466, 0x808088FF, "MAIN");
        drawTextCentered_(c, lcdCx, 486, 0xE0E0E0FF, "Track Name");
        drawTextCentered_(c, lcdCx, 506, 0x4488DDFF, "Stereo Bus");
    }

    // CS encoder + BC encoder — symmetrically centred under the LCD.
    {
        const float midX = kColCx + kColCw / 2.0f;
        knob(midX - 40, 590, 24, kGreyCap, "CS Encoder");
        knob(midX + 40, 590, 24, kGreyCap, "BC Encoder");
    }

    // ---- Right column: Dynamics + Channel ------------------------------
    constexpr float kColRx = kColCx + kColCw + 8, kColRw = 230;
    rect_(c, kColRx, 12, kColRw, H - 24, 0x1A1E24FF, 0x2A3038FF, 6.0);

    // Compressor section. Fast Attack + Peak in the top-right corner;
    // 2-column knob layout below: Ratio + Release in col 1, Threshold
    // in col 2 (mid-row). Y-positions chosen so Compressor + Gate are
    // VERTICALLY CENTRED in the column, with Channel section pinned to
    // the bottom (per Frank's note).
    sectionLabel(kColRx + 14, 32, "COMPRESSOR");
    dynBtn(kColRx + kColRw - 76, 24, "FAST ATK");
    dynBtn(kColRx + kColRw - 76, 50, "PEAK");
    {
        const float c1x = kColRx + 60, c2x = kColRx + 156;
        knob(c1x, 96,  20, kGreyCap, "RATIO");
        knob(c2x, 124, 20, kGreyCap, "THRESHOLD");
        knob(c1x, 158, 20, kGreyCap, "RELEASE");
    }
    // Dyn IN toggle (left) + GR meter (right) — vertically centred
    // in the gap between the Compressor knob block (ends ~y=194 incl.
    // RELEASE label) and the Gate section label (y=270). Mid-point
    // y=232. GR row spacing 12 px (was 8) so values + LEDs breathe.
    rect_(c, kColRx + 14, 221, 22, 22, 0xE0E0E0FF, 0x808088FF, 3.0);
    drawTextCentered_(c, kColRx + 25, 251, 0x9CA0AAFF, "IN");
    {
        const float gx = kColRx + kColRw - 26, gy = 208;
        const char* steps[] = { "20", "14", "9", "6", "3" };
        for (int i = 0; i < 5; ++i) {
            const float ly = gy + i * 12;
            circle_(c, gx, ly, 2.5, 0x404448FF, 0);
            drawText_(c, gx - 18, ly - 5, 0x808088FF, steps[i]);
        }
    }

    // Gate / Expander section. Staggered layout matching the Comp
    // section above — Threshold sits below Range diagonally, Hold
    // sits below Release diagonally. Same visual rhythm as the EQ
    // bands' Gain/Freq pairs in the left column.
    sectionLabel(kColRx + 14, 270, "GATE / EXPANDER");
    {
        const float c1x = kColRx + 60, c2x = kColRx + 156;
        knob(c1x, 308, 20, kGreyCap, "RANGE");
        knob(c2x, 332, 20, kGreyCap, "THRESHOLD");
        knob(c1x, 374, 20, kGreyCap, "RELEASE");
        knob(c2x, 398, 20, kGreyCap, "HOLD");
    }
    dynBtn(kColRx + 14, 430, "EXPAND");
    dynBtn(kColRx + 14, 456, "FAST ATK");

    // Channel section — pinned to the bottom of the column. Three
    // columns; small toggles keep their full width so "S/C LISTEN"
    // fits, while IN / SOLO / CUT / FINE are half-size per Frank's
    // note (those four buttons are physically smaller on the UC1).
    sectionLabel(kColRx + 14, 488, "CHANNEL");
    {
        constexpr float bw = 66;          // small-toggle width — fits "S/C LISTEN"
        constexpr float bh = 22;          // small toggle height
        // Half-size dimensions for IN / SOLO / CUT / FINE.
        constexpr float lbw = 33;         // half of bw
        constexpr float lh  = 28;         // half of original 56
        constexpr float inH = 37;         // half of original 74
        // Column 1 left edge (kColRx+14) flush with the dynBtn left
        // edge above (EXPAND / FAST ATK), so Polarity / S/C LISTEN /
        // SOLO CLR sit in the same vertical line as the dynamics
        // toggles. Columns 2/3 shift the same +6 to keep equal gaps.
        const float c1x = kColRx + 14;
        const float c2x = kColRx + 88;
        const float c3x = kColRx + 162;
        // Column 1 — three small toggles + SOLO large at the bottom.
        rect_(c, c1x, 510, bw, bh, 0x252A33FF, 0x4A5060FF, 3.0);
        drawTextCentered_(c, c1x + bw / 2.0f, 521, 0xC0C4CCFF, "Ø");
        rect_(c, c1x, 536, bw, bh, 0x252A33FF, 0x4A5060FF, 3.0);
        drawTextCentered_(c, c1x + bw / 2.0f, 547, 0xC0C4CCFF, "S/C LISTEN");
        rect_(c, c1x, 562, bw, bh, 0x252A33FF, 0x4A5060FF, 3.0);
        drawTextCentered_(c, c1x + bw / 2.0f, 573, 0xC0C4CCFF, "SOLO CLR");
        // Large bottom row — SOLO / CUT / FINE share one Y-line, but
        // each button is half-size and centred in its column slot.
        // Bottom edge (largeY + lh = 635) bündig with the LF Gain
        // label bottom on the left column.
        const float largeY = 607;
        const float soloX = c1x + (bw - lbw) / 2.0f;
        const float cutX  = c2x + (bw - lbw) / 2.0f;
        const float fineX = c3x + (bw - lbw) / 2.0f;
        rect_(c, soloX, largeY, lbw, lh, 0xE0E0E0FF, 0x808088FF, 3.0);
        drawTextCentered_(c, soloX + lbw / 2.0f, largeY + lh / 2.0f, 0x303338FF, "SOLO");
        rect_(c, cutX, largeY, lbw, lh, 0xE0E0E0FF, 0x808088FF, 3.0);
        drawTextCentered_(c, cutX + lbw / 2.0f, largeY + lh / 2.0f, 0x303338FF, "CUT");
        // Column 3 — Channel-IN spans the small-toggle rows (centred
        // vertically over the three of them); FINE in the large
        // bottom row, half-size.
        const float inX = c3x + (bw - lbw) / 2.0f;
        const float inY = 510 + (3 * bh - inH) / 2.0f;
        rect_(c, inX, inY, lbw, inH, 0xE0E0E0FF, 0x808088FF, 3.0);
        drawTextCentered_(c, inX + lbw / 2.0f, inY + inH / 2.0f, 0x303338FF, "IN");
        rect_(c, fineX, largeY, lbw, lh, 0xE0E0E0FF, 0x808088FF, 3.0);
        drawTextCentered_(c, fineX + lbw / 2.0f, largeY + lh / 2.0f, 0x303338FF, "FINE");
    }

    // Brand line
    drawTextCentered_(c, W / 2.0f, H - 14, 0x9CA0AAFF, "Rea-Sixty / UC1");
}

// Push a Rea-Sixty-themed colour set so the editor's combos / buttons /
// inputs match the schematic palette (dark blue-grey, soft accents)
// instead of the default ImGui orange/red. Returns count to pop.
int pushBindingsTheme(ImGui_Context* ctx)
{
    auto pc = [&](int idx, int rgba) { ImGui_PushStyleColor(ctx, idx, rgba); };
    pc(ImGui_Col_FrameBg,         0x252A33FF);
    pc(ImGui_Col_FrameBgHovered,  0x3A4253FF);
    pc(ImGui_Col_FrameBgActive,   0x4477CCFF);
    pc(ImGui_Col_Button,          0x252A33FF);
    pc(ImGui_Col_ButtonHovered,   0x3A4253FF);
    pc(ImGui_Col_ButtonActive,    0x4477CCFF);
    pc(ImGui_Col_Header,          0x252A33FF);
    pc(ImGui_Col_HeaderHovered,   0x3A4253FF);
    pc(ImGui_Col_HeaderActive,    0x4477CCFF);
    pc(ImGui_Col_Border,          0x4A5060FF);
    pc(ImGui_Col_Text,            0xD0D4DAFF);
    pc(ImGui_Col_TextDisabled,    0x6A6E78FF);
    pc(ImGui_Col_PopupBg,         0x14181EFF);
    pc(ImGui_Col_CheckMark,       0xAACCFFFF);
    pc(ImGui_Col_SliderGrab,      0x4477CCFF);
    pc(ImGui_Col_SliderGrabActive,0x6699EEFF);
    pc(ImGui_Col_Separator,       0x3A4253FF);
    pc(ImGui_Col_ChildBg,         0x1A1E24FF);
    return 18;
}

void popBindingsTheme(ImGui_Context* ctx, int n)
{
    ImGui_PopStyleColor(ctx, &n);
}

// Mutable refs into a Binding so the same picker code drives the
// primary action AND the long-press secondary action — the underlying
// fields differ but the widget tree is identical.
struct ActionFieldsRef {
    uf8::bindings::ActionType*  type;
    std::string*                action;
    int*                        param;
    std::string*                label;
    std::string*                midiDevice;
    int*                        midiChannel;
    int*                        midiMsgType;
    int*                        midiData1;
    int*                        midiData2;
};

bool drawActionPicker(ImGui_Context* ctx, const char* prefix,
                      ActionFieldsRef f, int layer,
                      uf8::bindings::ButtonId id, bool isLongPress)
{
    using namespace uf8::bindings;
    bool dirty = false;
    char idbuf[80];

    auto sectionRadio = [&](const char* tag, const char* label, ActionType t) {
        const bool on = (*f.type == t);
        std::snprintf(idbuf, sizeof(idbuf), "%s##%s_%s", label, prefix, tag);
        if (ImGui_RadioButton(ctx, idbuf, on)) {
            *f.type = t;
            dirty = true;
        }
    };

    // ---- Per-action display label ----
    // What the UF8 LCD shows when this slot is the one that will fire
    // (e.g. user soft-key bank slot, or visible while the matching
    // modifier is held). Empty falls back to the binding's top-level
    // label / hardware-face name. Some callers (e.g. drawSlotPicker
    // for short/long binding-editor slots) opt out by passing nullptr.
    if (f.label) {
        char lblBuf[64] = {0};
        std::strncpy(lblBuf, f.label->c_str(), sizeof(lblBuf) - 1);
        std::snprintf(idbuf, sizeof(idbuf),
                      "Display label##%s_lbl", prefix);
        double lw = 220;
        ImGui_PushItemWidth(ctx, lw);
        if (ImGui_InputTextWithHint(ctx, idbuf,
                                    "shown on the UF8 LCD",
                                    lblBuf, sizeof(lblBuf),
                                    nullptr, nullptr)) {
            *f.label = lblBuf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
        ImGui_Spacing(ctx);
    }

    // ---- REAPER Action ----
    sectionRadio("rd_reaper", "REAPER Action", ActionType::Reaper);
    if (*f.type == ActionType::Reaper) {
        ImGui_Indent(ctx, /*indent_w*/ nullptr);
        char buf[128] = {0};
        std::strncpy(buf, f.action->c_str(), sizeof(buf) - 1);
        std::snprintf(idbuf, sizeof(idbuf), "Action ID##%s_actid", prefix);
        double w = 260;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_InputTextWithHint(ctx, idbuf,
                                    "40044  /  _RS123abc",
                                    buf, sizeof(buf),
                                    /*flags*/ nullptr,
                                    /*callback*/ nullptr)) {
            *f.action = buf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        // Browse Action / Load ReaScript — open REAPER's pickers.
        // Browse uses PromptForAction; if a picker is already open for
        // THIS binding, swap the button to "Cancel" so the user can
        // close it without picking. Polling is owned by main.cpp's
        // onTimer hook so the result lands even if the user navigates
        // away from this editor before the picker closes.
        const bool pickerOpen = reasixty_actionPickerActiveFor(
            layer, id, isLongPress);
        std::snprintf(idbuf, sizeof(idbuf), "%s##%s_browse",
                      pickerOpen ? "Cancel Action Pick" : "Browse Action...",
                      prefix);
        if (ImGui_Button(ctx, idbuf,
                         /*size_w*/ nullptr, /*size_h*/ nullptr)) {
            if (pickerOpen) reasixty_actionPickerCancel();
            else            reasixty_actionPickerStart(layer, id, isLongPress);
        }
        sameLine(ctx);
        std::snprintf(idbuf, sizeof(idbuf), "Load ReaScript...##%s_load",
                      prefix);
        if (ImGui_Button(ctx, idbuf, nullptr, nullptr)) {
            std::string picked = reasixty_loadReaScript();
            if (!picked.empty()) {
                *f.action = picked;
                dirty = true;
            }
        }

        // Resolved-name line — dim, just for confirmation. Empty when
        // the field is blank; "(unresolved)" if the stored named cmd no
        // longer exists in this REAPER instance.
        std::string nameStr = reasixty_resolveActionName(*f.action);
        if (!nameStr.empty()) {
            char line[256];
            std::snprintf(line, sizeof(line), "  %s", nameStr.c_str());
            ImGui_TextDisabled(ctx, line);
        }
        ImGui_Unindent(ctx, /*indent_w*/ nullptr);
    }

    // ---- Native (Built-in) Action ----
    sectionRadio("rd_native", "Native Action (Built-in)", ActionType::Builtin);
    if (*f.type == ActionType::Builtin) {
        ImGui_Indent(ctx, nullptr);
        const std::string preview = f.action->empty()
            ? std::string("<pick one>")
            : builtinDisplayName(*f.action);
        std::snprintf(idbuf, sizeof(idbuf), "Built-in##%s_native", prefix);
        double w = 280;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_BeginCombo(ctx, idbuf, preview.c_str(), /*flags*/ nullptr)) {
            // Categorise the builtin catalogue so the dropdown stays
            // browseable as it grows. Each builtin gets bucketed by
            // name prefix; the buckets render in the order kCats
            // dictates with a Separator + disabled "header" entry
            // between groups. Anything that doesn't match a known
            // prefix falls into "Other" at the bottom.
            auto categoryFor = [](const std::string& n) -> const char* {
                if (n == "mod_shift" || n == "mod_cmd" || n == "mod_ctrl")
                    return "Modifiers";
                if (n.rfind("layer_select", 0) == 0)
                    return "Layer";
                if (n.rfind("encoder_", 0) == 0)
                    return "Encoder modes";
                if (n == "bank_left"  || n == "bank_right" ||
                    n == "page_left"  || n == "page_right")
                    return "Bank / Page";
                if (n.rfind("auto_", 0) == 0 || n == "automation_mode")
                    return "Automation";
                if (n.rfind("zoom_", 0) == 0)
                    return "Zoom";
                if (n.rfind("ssl_", 0) == 0 || n == "softkey_bank_select")
                    return "SSL Soft-Keys";
                if (n.rfind("send_", 0) == 0)
                    return "Sends";
                if (n.rfind("recv_", 0) == 0)
                    return "Receives";
                if (n == "show_user_bank")
                    return "User Soft-Key Banks";
                if (n == "flip" || n == "pan_force"
                 || n == "ssl_strip_mode_toggle"
                 || n == "mixer_toggle" || n == "home"
                 || n == "folder_mode" || n == "show_only_selected")
                    return "Mode Toggles";
                if (n.rfind("selset_", 0) == 0)
                    return "Selection Sets";
                if (n == "domain_cs" || n == "domain_bc")
                    return "Domain";
                if (n.rfind("__", 0) == 0)
                    return "";   // hide internals
                return "Other";
            };
            static const char* kCats[] = {
                "Modifiers", "Mode Toggles", "Layer", "Encoder modes",
                "Bank / Page", "Automation", "Zoom",
                "SSL Soft-Keys", "Domain",
                "Sends", "Receives", "User Soft-Key Banks",
                "Selection Sets",
                "Other",
            };
            std::unordered_map<std::string, std::vector<std::string>> bucket;
            for (auto& n : builtinNames()) {
                const char* cat = categoryFor(n);
                if (!cat || !*cat) continue;
                bucket[cat].emplace_back(n);
            }
            for (auto& v : bucket) std::sort(v.second.begin(), v.second.end());

            bool firstCat = true;
            for (auto* cat : kCats) {
                auto it = bucket.find(cat);
                if (it == bucket.end() || it->second.empty()) continue;
                if (!firstCat) ImGui_Separator(ctx);
                firstCat = false;
                int hdrFlags = ImGui_SelectableFlags_Disabled;
                ImGui_Selectable(ctx, cat, /*p_selected*/ nullptr,
                                 &hdrFlags, nullptr, nullptr);
                for (auto& n : it->second) {
                    std::string lbl = "  " + builtinDisplayName(n);
                    bool s = (n == *f.action);
                    if (ImGui_Selectable(ctx, lbl.c_str(), &s,
                                         nullptr, nullptr, nullptr)) {
                        *f.action = n;
                        dirty = true;
                    }
                }
            }
            ImGui_EndCombo(ctx);
        }
        ImGui_PopItemWidth(ctx);
        if (builtinUsesParam(*f.action)) {
            // Modifier builtins use param as a Momentary/Toggle mode
            // selector — show a friendlier combo instead of the raw int.
            const bool isMod = (*f.action == "mod_shift"
                             || *f.action == "mod_cmd"
                             || *f.action == "mod_ctrl");
            // Send/receive routing builtins use param as a Flip flag
            // (0 = Faders default, 1 = V-Pots).
            const bool isRouting =
                f.action->rfind("send_all_", 0) == 0
             || f.action->rfind("recv_all_", 0) == 0
             || *f.action == "send_this"
             || *f.action == "recv_this";
            // SSL Soft-Key builtins use param as the slot index 0..7.
            // Show a combo listing the actual function names from the
            // SSL plug-in's bank (e.g. "BYPASS / IN TRIM / PHASE …").
            // ssl_softkey follows the current PAGE bank (we use V-POT
            // labels as the picker hint since they're the most useful
            // generic set); ssl_bank_* address a specific bank.
            // ssl_softkey is bank-aware — its slot's meaning changes
            // with the live PAGE bank, so auto-filling a single label
            // from one bank would be wrong on every other bank
            // (incident 2026-05-02: sp.label "BYPASS" persisting onto
            // Bank 1's slot 0 = WIDTH). Limit the slot picker + label
            // auto-fill to the explicit-bank ssl_bank_* actions.
            int sslBankIdx = -1;          // -1 = no bank-tied combo / auto-fill
            int sslBankComboOnly = -1;    // show combo only, no label fill
            if (*f.action == "ssl_softkey") {
                sslBankComboOnly = 0;     // V-POT labels as a hint for the combo
            } else if (*f.action == "ssl_bank_vpot") {
                sslBankIdx = 0;
            } else if (f.action->rfind("ssl_bank_", 0) == 0
                    && f.action->size() == 10) {
                const char d = (*f.action)[9];
                if (d >= '1' && d <= '5') sslBankIdx = d - '0';
            }
            if (isMod) {
                static char kModeItems[] =
                    "Momentary (held = active)\0"
                    "Toggle (press flips state)\0";
                std::snprintf(idbuf, sizeof(idbuf), "Mode##%s_modemod", prefix);
                int m = (*f.param == 1) ? 1 : 0;
                double pw = 200;
                ImGui_PushItemWidth(ctx, pw);
                if (ImGui_Combo(ctx, idbuf, &m, kModeItems, nullptr)) {
                    *f.param = m;
                    dirty = true;
                }
                ImGui_PopItemWidth(ctx);
            } else if (isRouting) {
                std::snprintf(idbuf, sizeof(idbuf),
                              "Flip onto V-Pots (default: Faders)##%s_routeflip",
                              prefix);
                bool flipped = (*f.param == 1);
                if (ImGui_Checkbox(ctx, idbuf, &flipped)) {
                    *f.param = flipped ? 1 : 0;
                    dirty = true;
                }
            } else if (sslBankIdx >= 0 || sslBankComboOnly >= 0) {
                const int comboBank = (sslBankIdx >= 0)
                                         ? sslBankIdx : sslBankComboOnly;
                // Build a combo from the bank's labels. Empty slots
                // (some banks have gaps in the SSL plug-in spec) show
                // as "(empty)" so the user sees the slot exists but
                // does nothing if pressed.
                const char* const* labels =
                    reasixty_softkeyStockLabels(/*domain*/ 0, comboBank);
                // Auto-fill the display label only for explicit-bank
                // actions where the slot's meaning is stable. ssl_softkey
                // intentionally skipped — its label has to track the live
                // PAGE bank at render time, not be frozen here.
                if (sslBankIdx >= 0 && f.label && f.label->empty()
                    && labels) {
                    const int curSlot = std::clamp(*f.param, 0, 7);
                    if (labels[curSlot] && *labels[curSlot]) {
                        *f.label = labels[curSlot];
                        dirty = true;
                    }
                }
                char comboItems[8 * 32 + 1] = {0};
                size_t pos = 0;
                for (int i = 0; i < 8; ++i) {
                    const char* l = (labels && labels[i] && *labels[i])
                        ? labels[i] : "(empty)";
                    const size_t n = std::strlen(l);
                    if (pos + n + 2 < sizeof(comboItems)) {
                        std::memcpy(comboItems + pos, l, n);
                        pos += n;
                        comboItems[pos++] = '\0';
                    }
                }
                comboItems[pos] = '\0';
                std::snprintf(idbuf, sizeof(idbuf),
                              "SSL function##%s_sslslot", prefix);
                int slot = std::clamp(*f.param, 0, 7);
                double pw = 240;
                ImGui_PushItemWidth(ctx, pw);
                if (ImGui_Combo(ctx, idbuf, &slot, comboItems, nullptr)) {
                    *f.param = slot;
                    dirty = true;
                    // Auto-fill the slot's display label only for
                    // explicit-bank SSL actions. ssl_softkey is
                    // bank-aware so freezing one slot's name into
                    // sp.label would mis-display on every other bank.
                    if (sslBankIdx >= 0 && f.label && labels) {
                        const char* prevName =
                            labels[std::clamp(slot, 0, 7)];
                        // Detect "previously auto-filled with an SSL
                        // name" by comparing against any of this
                        // bank's labels. User-typed labels survive.
                        bool wasAutoFilled = f.label->empty();
                        for (int j = 0; !wasAutoFilled && j < 8; ++j) {
                            if (labels[j] && *labels[j]
                                && *f.label == labels[j]) {
                                wasAutoFilled = true;
                            }
                        }
                        if (wasAutoFilled && prevName && *prevName) {
                            *f.label = prevName;
                        }
                    }
                }
                ImGui_PopItemWidth(ctx);
            } else {
                std::snprintf(idbuf, sizeof(idbuf), "Param##%s_bparam", prefix);
                double pw = 90;
                ImGui_PushItemWidth(ctx, pw);
                int p = *f.param;
                if (ImGui_InputInt(ctx, idbuf, &p, nullptr, nullptr, nullptr)) {
                    *f.param = p;
                    dirty = true;
                }
                ImGui_PopItemWidth(ctx);
            }
        }
        ImGui_Unindent(ctx, nullptr);
    }

    // ---- MIDI Command ----
    sectionRadio("rd_midi", "MIDI Command", ActionType::Midi);
    if (*f.type == ActionType::Midi) {
        ImGui_Indent(ctx, nullptr);
        // Device (free-text — Phase D will swap to a combo of REAPER's
        // enumerated MIDI outputs).
        char dbuf[64] = {0};
        std::strncpy(dbuf, f.midiDevice->c_str(), sizeof(dbuf) - 1);
        std::snprintf(idbuf, sizeof(idbuf), "Device##%s_dev", prefix);
        double w = 200;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_InputTextWithHint(ctx, idbuf, "(empty = all enabled outputs)",
                                    dbuf, sizeof(dbuf),
                                    /*flags*/ nullptr,
                                    /*callback*/ nullptr)) {
            *f.midiDevice = dbuf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        // Channel 1..16
        std::snprintf(idbuf, sizeof(idbuf), "Channel##%s_ch", prefix);
        double cw = 90;
        ImGui_PushItemWidth(ctx, cw);
        int ch = *f.midiChannel;
        if (ImGui_InputInt(ctx, idbuf, &ch, nullptr, nullptr, nullptr)) {
            if (ch < 1)  ch = 1;
            if (ch > 16) ch = 16;
            *f.midiChannel = ch;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        // Message type
        static char kMidiMsgItems[] =
            "Note On\0Note Off\0Control Change\0Program Change\0";
        std::snprintf(idbuf, sizeof(idbuf), "Message##%s_msg", prefix);
        double mw = 160;
        ImGui_PushItemWidth(ctx, mw);
        int msg = *f.midiMsgType;
        if (ImGui_Combo(ctx, idbuf, &msg, kMidiMsgItems, nullptr)) {
            *f.midiMsgType = msg;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        // Data bytes
        std::snprintf(idbuf, sizeof(idbuf), "Note / CC ###%s_d1", prefix);
        double dw = 90;
        ImGui_PushItemWidth(ctx, dw);
        int d1 = *f.midiData1;
        if (ImGui_InputInt(ctx, idbuf, &d1, nullptr, nullptr, nullptr)) {
            if (d1 < 0)   d1 = 0;
            if (d1 > 127) d1 = 127;
            *f.midiData1 = d1;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        std::snprintf(idbuf, sizeof(idbuf), "Velocity / Value##%s_d2", prefix);
        ImGui_PushItemWidth(ctx, dw);
        int d2 = *f.midiData2;
        if (ImGui_InputInt(ctx, idbuf, &d2, nullptr, nullptr, nullptr)) {
            if (d2 < 0)   d2 = 0;
            if (d2 > 127) d2 = 127;
            *f.midiData2 = d2;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);

        ImGui_Text(ctx, "(Phase D wires real MIDI out — for now logs to "
                        "/tmp/rea_sixty_midi.log)");
        ImGui_Unindent(ctx, nullptr);
    }

    return dirty;
}

// Helper: render an ActionSlot's picker. `prefix` must be unique within
// the editor (the picker uses it for ImGui IDs).
bool drawSlotPicker(ImGui_Context* ctx, const char* prefix,
                    int layer, ButtonId id, uf8::bindings::ActionSlot& s,
                    bool isLongPress)
{
    ActionFieldsRef ref{
        &s.type, &s.action, &s.param, &s.label,
        &s.midiDevice, &s.midiChannel, &s.midiMsgType,
        &s.midiData1, &s.midiData2,
    };
    return drawActionPicker(ctx, prefix, ref, layer, id, isLongPress);
}

// Editor panel — two-column matrix. Left: SHORT PRESS (Plain row + 3
// modifier collapsibles). Right: LONG PRESS (same shape, only for
// Momentary). Auto-saves on every change.
void drawBindingEditor(ImGui_Context* ctx, int layer, ButtonId id)
{
    using namespace uf8::bindings;

    Binding bd    = getBinding(layer, id);
    bool    dirty = false;

    char header[120];
    // Top-soft-keys' default action follows the live PAGE bank — show
    // it in the header so the user always knows which bank's slot N
    // they're looking at on the hardware right now.
    const bool isTopSoftKey =
        id >= ButtonId::TopSoftKey1 && id <= ButtonId::TopSoftKey8;
    if (isTopSoftKey) {
        std::snprintf(header, sizeof(header),
                      "Editing: %s — %s   (Layer %d)",
                      hwFaceLabel(id),
                      reasixty_softkeyCurrentBankName(),
                      layer + 1);
    } else {
        std::snprintf(header, sizeof(header),
                      "Editing: %s   (Layer %d)",
                      hwFaceLabel(id), layer + 1);
    }
    ImGui_Text(ctx, header);
    ImGui_Separator(ctx);

    ImGui_PushID(ctx, uf8::bindings::toName(id));
    const int themePushed = pushBindingsTheme(ctx);

    // True when the binding's plain short slot maps a button to a
    // Modifier role. Combining a modifier with itself is undefined, so
    // we hide the modifier rows + long press for these bindings.
    auto& shortPlain = bd.shortPress[static_cast<int>(Modifier::Plain)];
    const bool plainIsModifier =
        shortPlain.type == ActionType::Builtin
        && (shortPlain.action == "mod_shift"
         || shortPlain.action == "mod_cmd"
         || shortPlain.action == "mod_ctrl");

    const bool momentary = (bd.behavior == Behavior::Momentary);
    const bool modifiersAvailable = momentary && !plainIsModifier;

    // Two side-by-side columns. Each child sized half the available
    // width with matching height so the bordered panels read as a pair.
    double availX = 0, availY = 0;
    ImGui_GetContentRegionAvail(ctx, &availX, &availY);
    double colW = (availX - 16) / 2.0;
    if (colW < 320) colW = 320;
    const double colH = 480;

    static const char* kModNames[]   = { "(no modifier)", "+ Shift / Fine",
                                         "+ Cmd",  "+ Ctrl" };
    static const char* kModSlugs[]   = { "pl", "sh", "cm", "ct" };

    auto drawColumn = [&](const char* title, const char* tag,
                          ActionSlot* slots, bool isLongCol)
    {
        double w = colW, h = colH;
        int childFlags = ImGui_ChildFlags_Borders;
        char childId[32];
        std::snprintf(childId, sizeof(childId), "%s_col", tag);
        if (ImGui_BeginChild(ctx, childId, &w, &h, &childFlags, nullptr)) {
            ImGui_Text(ctx, title);
            ImGui_Separator(ctx);

            // Whether to render the slot rows after the header. Replaces
            // the previous early-`return`s — those skipped EndChild and
            // tore the parent window down (same ReaImGui v0.10 trap as
            // commit c6bb9d0). Now the only exit point is the bottom of
            // the lambda.
            bool renderSlots = true;

            // Behavior combo lives only in the SHORT column — it
            // applies to both columns.
            if (!isLongCol) {
                static char kBehaviorItems[] =
                    "Momentary (fire on press)\0"
                    "Toggle (flip on each press)\0"
                    "Hold (state mirrors button)\0";
                int b = static_cast<int>(bd.behavior);
                double bw = 240;
                ImGui_PushItemWidth(ctx, bw);
                if (ImGui_Combo(ctx, "Behavior##pri_beh", &b, kBehaviorItems,
                                nullptr)) {
                    bd.behavior = static_cast<Behavior>(b);
                    dirty = true;
                }
                ImGui_PopItemWidth(ctx);
                ImGui_Spacing(ctx);
                ImGui_Separator(ctx);
            } else {
                // Long column gets an explicit enable toggle. Greyed
                // out for non-Momentary behaviours.
                if (!momentary) {
                    ImGui_TextDisabled(ctx,
                        "Long-press is only available for Momentary.");
                    if (bd.hasLongPress) { bd.hasLongPress = false; dirty = true; }
                    renderSlots = false;
                } else {
                    bool en = bd.hasLongPress;
                    if (ImGui_Checkbox(ctx, "Enable long-press (held > 0.5 s)",
                                       &en)) {
                        bd.hasLongPress = en;
                        if (en && slots[0].type == ActionType::Noop) {
                            slots[0].type = ActionType::Builtin;
                        }
                        dirty = true;
                    }
                    if (!bd.hasLongPress) {
                        renderSlots = false;
                    } else {
                        ImGui_Spacing(ctx);
                        ImGui_Separator(ctx);
                    }
                }
            }

            if (renderSlots) {
            // Plain row — always drawn first; never collapsed.
            ImGui_Text(ctx, kModNames[0]);
            char slotPrefix[32];
            std::snprintf(slotPrefix, sizeof(slotPrefix), "%s_pl", tag);
            if (drawSlotPicker(ctx, slotPrefix, layer, id, slots[0], isLongCol))
                dirty = true;

            // Modifier rows — each in its own collapsing header. Hidden
            // entirely when this binding is itself a modifier.
            if (plainIsModifier) {
                ImGui_Spacing(ctx);
                ImGui_TextDisabled(ctx,
                    "(Modifier rows hidden — this button IS a modifier.)");
            } else {
                for (int m = 1; m < kModifierCount; ++m) {
                    ImGui_Spacing(ctx);
                    ImGui_Separator(ctx);
                    char hdr[64];
                    if (slots[m].type != ActionType::Noop && !slots[m].action.empty()) {
                        std::snprintf(hdr, sizeof(hdr), "%s   →  %s",
                                      kModNames[m], slots[m].action.c_str());
                    } else {
                        std::snprintf(hdr, sizeof(hdr), "%s   (empty)",
                                      kModNames[m]);
                    }
                    char hdrId[80];
                    std::snprintf(hdrId, sizeof(hdrId), "%s##%s_h_%s",
                                  hdr, tag, kModSlugs[m]);
                    if (ImGui_CollapsingHeader(ctx, hdrId, nullptr, nullptr)) {
                        char modPrefix[32];
                        std::snprintf(modPrefix, sizeof(modPrefix), "%s_%s",
                                      tag, kModSlugs[m]);
                        if (drawSlotPicker(ctx, modPrefix, layer, id,
                                           slots[m], isLongCol)) dirty = true;
                    }
                }
            }
            }   // if (renderSlots)
        }
        ImGui_EndChild(ctx);
    };

    drawColumn("SHORT PRESS", "sp", bd.shortPress, /*isLongCol*/ false);
    ImGui_SameLine(ctx, nullptr, nullptr);
    if (modifiersAvailable) {
        drawColumn("LONG PRESS", "lp", bd.longPress, /*isLongCol*/ true);
    } else {
        // Placeholder column so the editor's two panels stay balanced
        // for Toggle/Hold or modifier-self bindings (where long-press
        // doesn't apply).
        double w = colW, h = colH;
        int childFlags = ImGui_ChildFlags_Borders;
        if (ImGui_BeginChild(ctx, "lp_col_disabled", &w, &h, &childFlags, nullptr)) {
            ImGui_Text(ctx, "LONG PRESS");
            ImGui_Separator(ctx);
            if (!momentary) {
                ImGui_TextDisabled(ctx,
                    "Long-press only available for Momentary primary.");
            } else {
                ImGui_TextDisabled(ctx,
                    "Long-press disabled — this button IS a modifier.");
            }
            if (bd.hasLongPress) { bd.hasLongPress = false; dirty = true; }
        }
        ImGui_EndChild(ctx);
    }

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);

    // ---- LED appearance ----
    // Two independent rows (Active / Inactive). Each row shows a single
    // colour swatch displaying the current value; clicking it opens a
    // popup with the 10 hardware-renderable colours from cap33 (SEL
    // DAW-Colour palette: red, orange, yellow, green, cyan, blue,
    // purple, magenta, pink, white). Brightness sits next to the
    // swatch as Off/Dim/Bright radios.
    //   Active   = lit-state (Toggle on / Hold held / Momentary press)
    //   Inactive = idle state
    ImGui_Text(ctx, "LED");
    ImGui_Spacing(ctx);

    auto drawLedRow = [&](const char* label,
                          uint8_t (&rgb)[3],
                          Brightness& bri,
                          const char* idTag) {
        ImGui_Text(ctx, label);
        ImGui_SameLine(ctx, nullptr, nullptr);
        const double labelColEnd = 80.0;
        ImGui_SetCursorPosX(ctx, labelColEnd);

        // Single swatch reflecting the currently-selected colour. Click
        // opens the palette popup. Slightly wider than tall so it reads
        // as a "field" rather than a single-cell pick.
        const int curRgba =
            (int(rgb[0]) << 24) |
            (int(rgb[1]) << 16) |
            (int(rgb[2]) <<  8) | 0xFF;
        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "##cur_%s", idTag);
        int btnFlags = 0;
        double bw = 56.0, bh = 22.0;
        if (ImGui_ColorButton(ctx, btnId, curRgba, &btnFlags, &bw, &bh)) {
            char popId[32];
            std::snprintf(popId, sizeof(popId), "palette_%s", idTag);
            ImGui_OpenPopup(ctx, popId, nullptr);
        }

        // Palette popup — 10 swatches in a 5x2 grid. Clicking a swatch
        // commits the colour and closes the popup.
        char popId[32];
        std::snprintf(popId, sizeof(popId), "palette_%s", idTag);
        if (ImGui_BeginPopup(ctx, popId, nullptr)) {
            int paletteCount = 0;
            const uf8::PaletteRgb* palette = uf8::selPaletteRgb(&paletteCount);
            const double sw = 26.0;
            const int perRow = 5;
            for (int i = 0; i < paletteCount; ++i) {
                const auto& p = palette[i];
                const int packed =
                    (int(p.r) << 24) |
                    (int(p.g) << 16) |
                    (int(p.b) <<  8) | 0xFF;
                char swId[32];
                std::snprintf(swId, sizeof(swId), "##pp_%s_%d", idTag, i);
                int swFlags = 0;
                double w = sw, h = sw;
                if (ImGui_ColorButton(ctx, swId, packed, &swFlags, &w, &h)) {
                    rgb[0] = p.r;
                    rgb[1] = p.g;
                    rgb[2] = p.b;
                    dirty = true;
                    ImGui_CloseCurrentPopup(ctx);
                }
                if ((i % perRow) != (perRow - 1) && i != paletteCount - 1)
                    ImGui_SameLine(ctx, nullptr, nullptr);
            }
            ImGui_EndPopup(ctx);
        }

        // Brightness radios — same row, after the swatch.
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_Text(ctx, "  ");
        ImGui_SameLine(ctx, nullptr, nullptr);
        const char* names[] = {"Off", "Dim", "Bright"};
        for (int i = 0; i < 3; ++i) {
            char idbuf[32];
            std::snprintf(idbuf, sizeof(idbuf), "%s##b_%s_%d",
                          names[i], idTag, i);
            if (ImGui_RadioButton(ctx, idbuf,
                                  static_cast<int>(bri) == i)) {
                bri = static_cast<Brightness>(i);
                dirty = true;
            }
            if (i < 2) ImGui_SameLine(ctx, nullptr, nullptr);
        }
    };

    drawLedRow("Active",   bd.color,         bd.brightness,         "act");
    drawLedRow("Inactive", bd.inactiveColor, bd.inactiveBrightness, "ina");

    ImGui_Spacing(ctx);
    // LED-when-empty override. Default is OFF — an unbound button stays
    // dark so the surface visibly shows what's available vs assigned.
    // Tick the checkbox to keep the configured Inactive colour glowing
    // even when the binding has no action (useful for hardware labels
    // / colour-coding rows that the user wants to keep visible).
    {
        bool en = bd.ledShowWhenEmpty;
        if (ImGui_Checkbox(ctx,
                "Show LED even when no action is assigned##ledforce",
                &en)) {
            bd.ledShowWhenEmpty = en;
            dirty = true;
        }
    }

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    if (ImGui_Button(ctx, "Clear binding (Do nothing)",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        bd = Binding{};   // reset to default (Noop, Momentary)
        dirty = true;
    }

    popBindingsTheme(ctx, themePushed);

    if (dirty) setBinding(layer, id, bd);

    ImGui_PopID(ctx);
}

} // namespace

// CSI-import sub-section — collapsed by default. Lets the user point at
// an existing CSI Surface directory (defaults to REAPER's bundled
// SSLUF8) and translate the Home zone into a Rea-Sixty layer. See
// CsiImport.cpp for the translation rules.
namespace {

void drawCsiImportSection(ImGui_Context* ctx, int editLayer)
{
    if (!ImGui_CollapsingHeader(ctx, "Import from CSI configuration",
                                /*p_visible*/ nullptr, /*flags*/ nullptr)) {
        return;
    }

    static char s_path[1024] = {0};
    static bool s_pathInited = false;
    static bool s_clearLayerFirst = true;
    static uf8::csi_import::ImportReport s_report;

    // Target layer follows the editor's layer combo on every frame —
    // avoids a duplicate selector here and keeps "Apply" semantically
    // tied to the layer the user currently has open.
    const int s_targetLayer = editLayer;

    if (!s_pathInited) {
        const std::string def = uf8::csi_import::defaultSurfaceDir();
        if (!def.empty()) {
            std::strncpy(s_path, def.c_str(), sizeof(s_path) - 1);
        }
        s_pathInited = true;
    }

    ImGui_TextWrapped(ctx,
        "Reads CSI's Home.zon and translates each global key assignment "
        "into a Rea-Sixty binding. Per-strip rows (Sel/Cut/Solo/Rec) and "
        "modifier-prefixed lines (Shift+, Global+, …) are skipped — they "
        "remain hardcoded in v1.");
    ImGui_Spacing(ctx);

    ImGui_Text(ctx, "CSI surface directory:");
    {
        double w = 600;
        ImGui_PushItemWidth(ctx, w);
        ImGui_InputTextWithHint(ctx, "##csi_path",
            "e.g. ~/Library/Application Support/REAPER/CSI/Surfaces/SSLUF8",
            s_path, sizeof(s_path),
            /*flags*/ nullptr, /*callback*/ nullptr);
        ImGui_PopItemWidth(ctx);
    }
    if (ImGui_Button(ctx, "Reset to default path",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        const std::string def = uf8::csi_import::defaultSurfaceDir();
        std::memset(s_path, 0, sizeof(s_path));
        std::strncpy(s_path, def.c_str(), sizeof(s_path) - 1);
    }

    ImGui_Spacing(ctx);
    {
        char info[64];
        std::snprintf(info, sizeof(info),
                      "Target: Layer %d  (follows the editor combo above)",
                      s_targetLayer + 1);
        ImGui_TextDisabled(ctx, info);
    }
    ImGui_Checkbox(ctx, "Clear target layer before import",
                   &s_clearLayerFirst);

    ImGui_Spacing(ctx);
    if (ImGui_Button(ctx, "Preview", /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        s_report = uf8::csi_import::preview(s_path);
    }
    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr, /*spacing*/ nullptr);
    if (ImGui_Button(ctx, "Apply now",
                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        s_report = uf8::csi_import::apply(s_path, s_targetLayer,
                                          s_clearLayerFirst);
    }

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);

    if (!s_report.loaded && s_report.error.empty()) {
        ImGui_TextWrapped(ctx,
            "Click \"Preview\" to scan the chosen directory, or \"Apply\" "
            "to write the result into the target layer.");
        return;
    }

    if (!s_report.loaded) {
        ImGui_TextColored(ctx, 0xFF6666FF, s_report.error.c_str());
        return;
    }

    char hdr[256];
    std::snprintf(hdr, sizeof(hdr),
                  "Source: %s   |   Mapped: %d   Skipped: %d   Warnings: %d",
                  s_report.zonePath.c_str(),
                  s_report.appliedCount, s_report.skippedCount,
                  s_report.warningCount);
    ImGui_TextWrapped(ctx, hdr);

    ImGui_Spacing(ctx);
    {
        // ReaImGui v0.10 — EndChild MUST be unconditional (must pair with
        // every BeginChild call, regardless of its return value).
        // Same root cause as commit 95405b2 in MixerWindow: a missed
        // EndChild leaves the ImGui stack desynced and the next frame
        // tears down the parent window.
        double childH = 240;
        if (ImGui_BeginChild(ctx, "csi_import_log",
                             /*size_w*/ nullptr, &childH,
                             /*child_flags*/ nullptr,
                             /*window_flags*/ nullptr)) {
            for (const auto& e : s_report.entries) {
                int colour;
                if (!e.applied)        colour = 0x888888FF; // skipped (grey)
                else if (e.warning)    colour = 0xFFC050FF; // warning (amber)
                else                   colour = 0x80FF80FF; // applied (green)
                char line[512];
                std::snprintf(line, sizeof(line), "  %-14s  →  %s",
                              e.widget.c_str(), e.mappedTo.c_str());
                ImGui_TextColored(ctx, colour, line);
                if (!e.action.empty()) {
                    std::snprintf(line, sizeof(line),
                                  "                CSI: %s", e.action.c_str());
                    ImGui_TextColored(ctx, 0x99999999, line);
                }
            }
        }
        ImGui_EndChild(ctx);
    }
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

    drawCsiImportSection(ctx, s_editLayer);
    ImGui_Spacing(ctx);

    // ---- Top: layer-pick row + activate ----
    // Three big tab-like buttons replace the old combo so the user can
    // jump between layers with a single click. The currently-edited
    // layer is highlighted (blue fill); the hardware-active layer is
    // marked with a green dot so the two states stay distinguishable.
    {
        const int active = getActiveLayer();
        const uint32_t kSelFill   = 0x4477CCFF;
        const uint32_t kSelHover  = 0x5588DDFF;
        const uint32_t kSelActive = 0x6699EEFF;
        for (int li = 0; li < 3; ++li) {
            char label[32];
            // U+25CF BLACK CIRCLE marks the layer that's currently
            // driving the hardware so it's visible at a glance.
            std::snprintf(label, sizeof(label), "Layer %d%s##layer_tab_%d",
                          li + 1,
                          (li == active) ? "  \xE2\x97\x8F" : "",
                          li);
            const bool selected = (s_editLayer == li);
            int pushed = 0;
            if (selected) {
                ImGui_PushStyleColor(ctx, ImGui_Col_Button,        kSelFill);
                ImGui_PushStyleColor(ctx, ImGui_Col_ButtonHovered, kSelHover);
                ImGui_PushStyleColor(ctx, ImGui_Col_ButtonActive,  kSelActive);
                pushed = 3;
            }
            double bw = 110, bh = 28;
            if (ImGui_Button(ctx, label, &bw, &bh)) {
                s_editLayer = li;
            }
            if (pushed) {
                ImGui_PopStyleColor(ctx, &pushed);
            }
            if (li < 2) sameLine(ctx);
        }
        sameLine(ctx);
        ImGui_Text(ctx, "  ");
        sameLine(ctx);
        if (s_editLayer == active) {
            ImGui_TextDisabled(ctx, "(this layer is active on hardware)");
        } else {
            if (ImGui_Button(ctx, "Make this layer active",
                             /*size_w*/ nullptr, /*size_h*/ nullptr)) {
                setActiveLayer(s_editLayer);
            }
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

    // ---- Portable per-layer save / load ----
    // Both buttons act on the layer currently selected in the editor
    // (s_editLayer). The status line is sticky-but-cheap: the last
    // action's outcome shows until the next action overwrites it.
    static std::string s_portMsg;
    char btnSave[40], btnLoad[40];
    std::snprintf(btnSave, sizeof(btnSave), "Save layer %d to file…",
                  s_editLayer + 1);
    std::snprintf(btnLoad, sizeof(btnLoad), "Load layer %d from file…",
                  s_editLayer + 1);
    sameLine(ctx);
    if (ImGui_Button(ctx, btnSave, /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        s_portMsg = reasixty_exportLayerViaDialog(s_editLayer)
            ? "Layer exported."
            : "Export cancelled or failed.";
    }
    sameLine(ctx);
    if (ImGui_Button(ctx, btnLoad, /*size_w*/ nullptr, /*size_h*/ nullptr)) {
        s_portMsg = reasixty_importLayerViaDialog(s_editLayer)
            ? "Layer imported."
            : "Import cancelled or failed.";
    }
    // CSI import lives in its own panel at the top of this tab
    // (drawCsiImportSection) — its target-layer follows s_editLayer.
    if (!s_portMsg.empty()) {
        ImGui_TextDisabled(ctx, s_portMsg.c_str());
    }

    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    // ---- Hardware schematic (vector, mirrors SSL UF8 page-14 layout) ----
    drawUf8Vector(ctx, s_selected);

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
// User-defined Soft-Key Banks: 12 storage slots, each holding 8 binding
// slots (mapped 1:1 onto the top-soft-key row above the strips). When
// activated via the `show_user_bank` builtin, the bank's slots
// override the plugin-driven labels + actions for the duration.
//
// MVP editor: bank picker, name input, 8 slots × (label + action picker).
// Modifier matrix / long-press / LED config per slot reuse the existing
// per-binding editor wiring once the user clicks "Edit slot N".
// Render one user-bank's editing surface — bank name + 8 slot
// collapsibles. Mutates `bank` in-place; caller persists via setUserBank
// when `dirty` is true.
void drawUserBankEditor_(ImGui_Context* ctx, int bankIdx,
                         uf8::bindings::UserBank& bank, bool& dirty)
{
    using namespace uf8::bindings;
    {
        char nameBuf[128] = {0};
        std::strncpy(nameBuf, bank.name.c_str(), sizeof(nameBuf) - 1);
        double w = 280;
        ImGui_PushItemWidth(ctx, w);
        if (ImGui_InputTextWithHint(ctx, "Bank name##bank_name",
                                    "e.g. Vocals, Drums, Mixdown",
                                    nameBuf, sizeof(nameBuf),
                                    /*flags*/ nullptr,
                                    /*callback*/ nullptr)) {
            bank.name = nameBuf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
    }
    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    for (int i = 0; i < uf8::bindings::kUserBankSlots; ++i) {
        char idtag[32];
        std::snprintf(idtag, sizeof(idtag), "ub%d_slot%d", bankIdx, i);
        ImGui_PushID(ctx, idtag);
        Binding& bd = bank.slots[i];
        auto& sp = bd.shortPress[static_cast<int>(Modifier::Plain)];

        char header[80];
        const std::string& act = sp.action;
        if (act.empty()) {
            std::snprintf(header, sizeof(header),
                          "Slot %d   (empty)", i + 1);
        } else {
            std::snprintf(header, sizeof(header),
                          "Slot %d   %s", i + 1,
                          bd.label.empty() ? act.c_str() : bd.label.c_str());
        }
        if (ImGui_CollapsingHeader(ctx, header, nullptr, nullptr)) {
            ImGui_Indent(ctx, nullptr);
            char labelBuf[64] = {0};
            std::strncpy(labelBuf, bd.label.c_str(), sizeof(labelBuf) - 1);
            double w = 200;
            ImGui_PushItemWidth(ctx, w);
            if (ImGui_InputTextWithHint(ctx, "Label##slotlabel",
                                        "shown on the top-soft-key LCD",
                                        labelBuf, sizeof(labelBuf),
                                        nullptr, nullptr)) {
                bd.label = labelBuf;
                dirty = true;
            }
            ImGui_PopItemWidth(ctx);

            ActionFieldsRef ref{
                &sp.type, &sp.action, &sp.param, &sp.label,
                &sp.midiDevice, &sp.midiChannel, &sp.midiMsgType,
                &sp.midiData1, &sp.midiData2,
            };
            char prefix[32];
            std::snprintf(prefix, sizeof(prefix), "ub%d_s%d", bankIdx, i);
            if (drawActionPicker(ctx, prefix, ref,
                                 /*layer*/ -1, ButtonId::None,
                                 /*isLongPress*/ false)) {
                dirty = true;
            }
            if (ImGui_Button(ctx, "Clear slot##clr",
                             /*size_w*/ nullptr, /*size_h*/ nullptr)) {
                bd = Binding{};
                dirty = true;
            }
            ImGui_Unindent(ctx, nullptr);
        }
        ImGui_PopID(ctx);
    }
}

void SettingsScreen::drawSoftKeyBanks(ImGui_Context* ctx)
{
    using namespace uf8::bindings;

    ImGui_Text(ctx, "User Soft-Key Banks");
    ImGui_TextDisabled(ctx,
        "12 user-defined banks. Bind a button to \"Show user soft-key "
        "bank\" (param 0..11) to activate one of these banks at "
        "runtime — the top-soft-key row above the V-Pots loads the "
        "bank's 8 slot bindings. SSL Channel-Strip stock banks are "
        "addressable directly via the \"SSL Standard Bank N\" actions "
        "in the per-binding action picker; no separate page needed.");
    ImGui_Spacing(ctx);

    int barFlags = 0;
    if (!ImGui_BeginTabBar(ctx, "skbanks_tabs", &barFlags)) return;

    // ---- 12 user banks (editable) --------------------------------------
    for (int i = 0; i < uf8::bindings::kUserBankCount; ++i) {
        UserBank b = getUserBank(i);
        char tab[64];
        if (b.name.empty()) {
            std::snprintf(tab, sizeof(tab), "Bank %d##utab%d", i + 1, i);
        } else {
            std::snprintf(tab, sizeof(tab), "%s##utab%d", b.name.c_str(), i);
        }
        if (ImGui_BeginTabItem(ctx, tab, nullptr, nullptr)) {
            bool dirty = false;
            drawUserBankEditor_(ctx, i, b, dirty);
            if (dirty) setUserBank(i, b);
            ImGui_EndTabItem(ctx);
        }
    }

    ImGui_EndTabBar(ctx);
}

// ---- FX Learn -------------------------------------------------------------
// Phase 2.5d-A Step 3: Master-View + Editor-View.
//   3a Master-View — table of UserPluginMaps with CRUD.
//   3b Editor-View — slot list (canonical SSL 360 Link topology) on the
//      left, FX-param list on the right, click-to-listen + click-param
//      to bind. Drag-and-drop and vector schematic come in 3c.
// See: docs/plan-fx-learn-and-multi-instance.md §"Editor-View"
namespace {

// Inline form state for "+ New" / per-row error reporting. File-scope
// statics — same pattern as the bindings editor's transient buffers.
char        g_newMatch[128]      = {};
char        g_newDisplay[8]      = {};   // 4 chars + NUL, padded for slack
int         g_newDomain          = 1;    // 1=CS, 2=BC. 0=None reserved.
std::string g_newError;                  // transient inline error text
std::string g_pendingDeleteMatch;        // populated when the confirm popup is open
bool        g_pendingDeleteOpen  = false;// set when row's Del was clicked,
                                         // consumed by the OpenPopup at the
                                         // outer scope so popup id-stack
                                         // matches the BeginPopupModal site.
std::string g_lastSaveError;             // last persistence error, sticky until next save

// Cached list of installed FX populated lazily on first "+ New" open.
// REAPER's EnumInstalledFX walks the entire plugin catalog (can be
// 5000+ entries with FabFilter / Waves bundles), so we cache it for
// the session and only refresh on explicit "Reload" click.
struct InstalledFx {
    std::string name;   // human-readable, e.g. "VST3: SSL Native Channel Strip 2 (SSL)"
    std::string ident;  // identifier, e.g. "ssl_nativechannelstrip2.vst3"
};
std::vector<InstalledFx> g_installedFx;
char g_pickerFilter[64] = {};
int  g_pickerSelectedIdx = -1;   // index into g_installedFx (filtered or full)

void loadInstalledFx_()
{
    g_installedFx.clear();
    int idx = 0;
    const char* name = nullptr;
    const char* ident = nullptr;
    while (EnumInstalledFX(idx, &name, &ident)) {
        InstalledFx e;
        if (name)  e.name  = name;
        if (ident) e.ident = ident;
        if (!e.name.empty()) g_installedFx.push_back(std::move(e));
        ++idx;
        // Defensive cap so a bug in EnumInstalledFX can't lock us up.
        if (idx > 20000) break;
    }
}

// Heuristic: build a 4-char displayShort from a full FX name. Strips
// the "VSTn: " prefix, removes trailing vendor "(...)" parens, then
// takes the first 4 chars (with caps preserved if possible).
std::string deriveShortLabel_(const std::string& fxName)
{
    std::string s = fxName;
    // Strip "VST3: " / "VST: " / "AU: " etc.
    auto colon = s.find(": ");
    if (colon != std::string::npos && colon < 8) s = s.substr(colon + 2);
    // Strip trailing " (vendor)".
    auto paren = s.rfind(" (");
    if (paren != std::string::npos) s = s.substr(0, paren);
    // Trim.
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back()  == ' ') s.pop_back();
    if (s.size() > 4) s.resize(4);
    return s;
}

// Editor-View state. `g_editingMatch` is the current map's `match`
// substring; empty = master-view. `g_listeningLinkIdx == -1` means no
// slot is awaiting a param bind. Param-list filter is sticky between
// renders so the search query survives a tab switch.
std::string g_editingMatch;
int         g_listeningLinkIdx = -1;
char        g_paramFilter[64]  = {};

// Click-and-turn state: when a slot is in listening mode we poll
// REAPER's GetLastTouchedFX every frame so wiggling the actual
// plugin-GUI control binds the touched param to the listening slot.
// We snapshot the current GetLastTouchedFX value at the moment the
// listen begins (or jumps to the next slot) so a stale prior touch
// doesn't auto-bind on entry.
int g_listeningPrevIdx     = -1;
int g_lastTouchedTr        = -2;   // -2 = uninitialised; -1 = no last touch
int g_lastTouchedFx        = -1;
int g_lastTouchedParam     = -1;
// Plugin-selector key — "trackIdx:fxIdx" of the FX instance whose param
// list the editor reads from. -1 trackIdx = master. Empty string = pick
// first match (auto). Cleared whenever the editing map changes so the
// selector doesn't outlive its scope.
std::string g_fxSelectorKey;
std::string g_fxSelectorScope;   // editing match for which the key is valid

const char* domainLabel_(uf8::Domain d)
{
    switch (d) {
        case uf8::Domain::ChannelStrip: return "CS";
        case uf8::Domain::BusComp:      return "BC";
        default:                        return "—";
    }
}

void persistAndReport_()
{
    using uf8::user_plugins::SaveResult;
    g_lastSaveError.clear();
    switch (uf8::user_plugins::save()) {
        case SaveResult::Ok:        break;
        case SaveResult::Collision: g_lastSaveError = "Save refused: a match collides with a built-in plugin map."; break;
        case SaveResult::IoError:   g_lastSaveError = "Save failed: could not write user_plugins.json (see /tmp/rea_sixty.log)."; break;
    }
}

// ---- Editor helpers --------------------------------------------------------

// Pick the canonical slot topology for a domain. The SSL 360 Link CS map
// has the broadest CS slot coverage (input/EQ/dyn/output incl. ext::*
// extension slots); the SSL 360 Link Bus Compressor map covers BC.
// Falling back to the first map of that domain keeps the editor
// renderable even if the Link maps are renamed in future.
const uf8::PluginMap* canonicalTopology_(uf8::Domain d)
{
    const uf8::PluginMap* fallback = nullptr;
    for (const auto& m : uf8::allPluginMaps()) {
        if (m.domain != d) continue;
        if (!fallback) fallback = &m;
        if (d == uf8::Domain::ChannelStrip &&
            std::strcmp(m.displayShort, "Link") == 0) return &m;
        if (d == uf8::Domain::BusComp &&
            std::strcmp(m.displayShort, "L-BC") == 0) return &m;
    }
    return fallback;
}

// All FX instances (across all tracks + master) whose name contains the
// user map's match substring. The plugin-selector dropdown lets the user
// pick which one the editor reads its param list from; the schematic +
// param list bind to whichever instance the user picks. Returns the
// instances in the order they're discovered (master first, then tracks
// 1..N), so the auto-pick (g_fxSelectorKey empty) maps to the same
// instance findEditingFx_ used to return pre-selector.
struct EditingFx { MediaTrack* tr = nullptr; int fxIdx = -1; bool ok = false; };

struct EditingFxInstance {
    MediaTrack* tr;
    int         trIdx;     // -1 = master
    int         fxIdx;
    std::string label;     // human-readable for the combo
    std::string key;       // "trIdx:fxIdx" for the selector
};

std::vector<EditingFxInstance> findEditingFxAll_(const std::string& match)
{
    std::vector<EditingFxInstance> out;
    if (match.empty()) return out;
    char buf[512];

    auto scan = [&](MediaTrack* tr, int trIdx) {
        if (!tr) return;
        const int n = TrackFX_GetCount(tr);
        for (int i = 0; i < n; ++i) {
            if (!TrackFX_GetFXName(tr, i, buf, sizeof(buf))) continue;
            if (std::strstr(buf, match.c_str()) == nullptr) continue;
            // Track name (best-effort; empty for unnamed tracks).
            char trName[128] = {};
            if (trIdx >= 0) {
                GetTrackName(tr, trName, sizeof(trName));
            }
            char lbl[700];
            if (trIdx < 0) {
                std::snprintf(lbl, sizeof(lbl),
                    "Master / FX %d  '%s'", i + 1, buf);
            } else if (trName[0]) {
                std::snprintf(lbl, sizeof(lbl),
                    "Track %d '%s' / FX %d  '%s'",
                    trIdx + 1, trName, i + 1, buf);
            } else {
                std::snprintf(lbl, sizeof(lbl),
                    "Track %d / FX %d  '%s'",
                    trIdx + 1, i + 1, buf);
            }
            char keybuf[32];
            std::snprintf(keybuf, sizeof(keybuf), "%d:%d", trIdx, i);
            out.push_back({ tr, trIdx, i, std::string(lbl), std::string(keybuf) });
        }
    };

    scan(GetMasterTrack(nullptr), -1);
    const int trackCount = CountTracks(nullptr);
    for (int t = 0; t < trackCount; ++t) scan(GetTrack(nullptr, t), t);
    return out;
}

// Resolve the user's selector choice (or auto-pick) to an EditingFx.
EditingFx pickEditingFx_(const std::vector<EditingFxInstance>& list)
{
    if (list.empty()) return {};
    if (!g_fxSelectorKey.empty()) {
        for (const auto& e : list) {
            if (e.key == g_fxSelectorKey) return { e.tr, e.fxIdx, true };
        }
    }
    // Auto-pick = first hit (master before tracks; preserves pre-selector
    // behaviour when nothing's been selected explicitly).
    return { list[0].tr, list[0].fxIdx, true };
}

// Find the existing vst3Param (if any) bound to `linkIdx` for the map
// currently being edited. Returns -1 when not yet mapped.
int mappedVst3For_(int linkIdx)
{
    for (const auto& m : uf8::user_plugins::get().maps) {
        if (m.match != g_editingMatch) continue;
        for (const auto& s : m.slots) {
            if (s.linkIdx == linkIdx) return s.vst3Param;
        }
        break;
    }
    return -1;
}

// Bind / overwrite a slot's vst3Param on the editing map, then save.
void bindSlot_(int linkIdx, int vst3Param)
{
    if (g_editingMatch.empty() || linkIdx < 0 || vst3Param < 0) return;

    auto cat = uf8::user_plugins::get();   // copy
    bool changed = false;
    for (auto& m : cat.maps) {
        if (m.match != g_editingMatch) continue;
        bool replaced = false;
        for (auto& s : m.slots) {
            if (s.linkIdx == linkIdx) {
                s.vst3Param = vst3Param;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            uf8::UserLinkSlot s{};
            s.linkIdx   = linkIdx;
            s.vst3Param = vst3Param;
            s.inverted  = false;
            m.slots.push_back(s);
        }
        uf8::user_plugins::upsert(m);
        changed = true;
        break;
    }
    if (changed) persistAndReport_();
}

// Remove a slot binding from the editing map.
void unbindSlot_(int linkIdx)
{
    if (g_editingMatch.empty() || linkIdx < 0) return;
    auto cat = uf8::user_plugins::get();
    for (auto& m : cat.maps) {
        if (m.match != g_editingMatch) continue;
        auto before = m.slots.size();
        m.slots.erase(
            std::remove_if(m.slots.begin(), m.slots.end(),
                [&](const uf8::UserLinkSlot& s) { return s.linkIdx == linkIdx; }),
            m.slots.end());
        if (m.slots.size() != before) {
            uf8::user_plugins::upsert(m);
            persistAndReport_();
        }
        break;
    }
}

// Move listening to the next still-unmapped slot in topology order, or
// clear when nothing else needs binding. Used after a click-bind to
// support quick mass mapping without an extra click per slot.
void autoAdvanceListening_(const uf8::PluginMap& topo)
{
    if (g_listeningLinkIdx < 0) return;
    bool past = false;
    for (const auto& s : topo.slots) {
        if (!past) {
            if (s.linkIdx == g_listeningLinkIdx) past = true;
            continue;
        }
        if (mappedVst3For_(s.linkIdx) < 0) {
            g_listeningLinkIdx = s.linkIdx;
            return;
        }
    }
    g_listeningLinkIdx = -1;
}

// Toggle the inverted-flag on a mapped slot.
void toggleInverted_(int linkIdx)
{
    if (g_editingMatch.empty() || linkIdx < 0) return;
    auto cat = uf8::user_plugins::get();
    for (auto& m : cat.maps) {
        if (m.match != g_editingMatch) continue;
        for (auto& s : m.slots) {
            if (s.linkIdx == linkIdx) {
                s.inverted = !s.inverted;
                uf8::user_plugins::upsert(m);
                persistAndReport_();
                return;
            }
        }
        break;
    }
}

// ---- Schematic layout -----------------------------------------------------
// One pad per slot in a domain's canonical topology. Coordinates are local
// to the schematic origin (0,0 = top-left of the canvas inside the left
// child window). Width/height fixed per pad (60x24) for an even grid.
//
// linkIdx values must be a subset of the canonical topology (kSsl360LinkSlots
// for CS, kSsl360LinkBcSlots for BC). Anything not present in the topology
// gets a ghost-render placeholder ("n/a") — keeps the layout stable when
// the canonical map changes.
struct SchematicPad {
    int         linkIdx;
    float       x, y;
    const char* shortLabel;   // 4-6 chars rendered inside the pad
};

struct SchematicSection {
    const char* title;
    float       x, y;
};

struct DomainSchematic {
    const SchematicPad*     pads;
    int                     padCount;
    const SchematicSection* sections;
    int                     sectionCount;
    float                   width, height;
};

constexpr float kPadW = 60.0f, kPadH = 22.0f;

// Channel-Strip canvas — 8 columns of 60px pads + 4px gaps == 508 px wide.
// Section labels track the row above each cluster.
constexpr SchematicSection kCsSections[] = {
    { "INPUT",     4,    8 },
    { "EQ",        4,   58 },
    { "FILTERS",   4,  204 },
    { "DYNAMICS",  4,  254 },
    { "OUTPUT",    4,  362 },
    { "EXTRAS",    4,  418 },
};

// Channel-Strip pads. linkIdx columns refer to kSsl360LinkSlots indices.
constexpr SchematicPad kCsPads[] = {
    // INPUT row
    {  0,   4,  22, "BYP"   },
    {  4,  68,  22, "IN"    },
    {  5, 132,  22, "POL"   },
    { 36, 196,  22, "S/C L" },

    // EQ row 1: EqIn / EqType
    { 15,   4,  72, "EQ IN" },
    { 14,  68,  72, "TYPE"  },

    // EQ columns: HF / HMF / LMF / LF (3 controls each, stacked rows)
    //   row a (y=104): bell/Q
    {  8,   4, 104, "HF BL" },
    { 13,  68, 104, "HMF Q" },
    { 18, 132, 104, "LMF Q" },
    { 21, 196, 104, "LF BL" },
    //   row b (y=128): gain
    {  9,   4, 128, "HF GN" },
    { 11,  68, 128, "HMF G" },
    { 16, 132, 128, "LMF G" },
    { 20, 196, 128, "LF GN" },
    //   row c (y=152): freq
    { 10,   4, 152, "HF FQ" },
    { 12,  68, 152, "HMF F" },
    { 17, 132, 152, "LMF F" },
    { 19, 196, 152, "LF FQ" },

    // FILTERS row
    {  7,   4, 218, "HPF"   },
    {  6,  68, 218, "LPF"   },

    // DYNAMICS — DynIn + Comp row + Gate row
    { 22,   4, 268, "DYN IN" },

    {  27,  4, 296, "C THR" },
    {  26, 68, 296, "C RAT" },
    {  24,132, 296, "C ATK" },
    {  28,196, 296, "C REL" },
    {  23,260, 296, "C MIX" },
    {  25,324, 296, "C PK"  },

    {  30,  4, 324, "G THR" },
    {  29, 68, 324, "G RNG" },
    {  34,132, 324, "G ATK" },
    {  31,196, 324, "G REL" },
    {  32,260, 324, "G HLD" },
    {  33,324, 324, "G/E"   },

    // OUTPUT — width / pan / out / fader (FaderLevel highlighted)
    {   2,  4, 376, "WID"   },
    {   3, 68, 376, "PAN"   },
    {  37,132, 376, "OUT"   },
    {   1,196, 376, "FADER" },

    // EXTRAS — quick-access, saturation, group sense
    {  38,  4, 432, "QA1"   },
    {  39, 68, 432, "QA2"   },
    {  40,132, 432, "QA3"   },
    {  41,196, 432, "QA4"   },
    {  42,260, 432, "QA5"   },
    {  43,324, 432, "QA6"   },
    {  44,388, 432, "SAT"   },
    {  45,452, 432, "SAT.I" },
    {  46,516, 432, "GRP"   },
};

// Bus-Comp canvas — much smaller. One row of ratio/threshold/etc + a
// second row for sidechain + group sense.
constexpr SchematicSection kBcSections[] = {
    { "COMPRESSOR", 4,   8 },
    { "SIDECHAIN",  4,  84 },
    { "EXTRAS",     4, 140 },
};

constexpr SchematicPad kBcPads[] = {
    {  0,   4,  22, "BYP"  },
    {  1,  68,  22, "THR"  },
    {  5, 132,  22, "RAT"  },
    {  3, 196,  22, "ATK"  },
    {  4, 260,  22, "REL"  },
    {  2, 324,  22, "MAKE" },
    {  7, 388,  22, "MIX"  },

    {  6,   4,  98, "S/C HPF" },

    { 46,   4, 154, "GRP"  },
};

DomainSchematic schematicFor_(uf8::Domain d)
{
    switch (d) {
        case uf8::Domain::ChannelStrip:
            return { kCsPads,
                     int(sizeof(kCsPads) / sizeof(kCsPads[0])),
                     kCsSections,
                     int(sizeof(kCsSections) / sizeof(kCsSections[0])),
                     584.0f, 470.0f };
        case uf8::Domain::BusComp:
            return { kBcPads,
                     int(sizeof(kBcPads) / sizeof(kBcPads[0])),
                     kBcSections,
                     int(sizeof(kBcSections) / sizeof(kBcSections[0])),
                     460.0f, 200.0f };
        default:
            return { nullptr, 0, nullptr, 0, 0, 0 };
    }
}

// Render one schematic pad. Combines the visual rect/text (via DrawList,
// always drawn) with an InvisibleButton (so drag-drop targets and popups
// can hook off the last-added item). State drives the colour:
//   - listening : amber fill + outline
//   - mapped    : green outline
//   - unmapped  : flat dark fill
// Returns true on left-click (caller toggles listening). Drag-drop accept
// + right-click context popup are handled inline.
void drawSchematicPad_(ImGui_Context* ctx,
                       ImGui_DrawList* dl,
                       float ox, float oy,
                       const SchematicPad& pad,
                       const uf8::PluginMap& topo,
                       const EditingFx& fx)
{
    const uf8::LinkSlot* slot = uf8::findSlotByLinkIdx(topo, pad.linkIdx);
    const int  mapped     = mappedVst3For_(pad.linkIdx);
    const bool isMapped   = (mapped >= 0);
    const bool isListen   = (g_listeningLinkIdx == pad.linkIdx);
    const bool exists     = (slot != nullptr);

    // Colours — match the Bindings-editor schematic palette so the
    // FX-Learn view feels like part of the same family. Listening state
    // pulses the border alpha (sin wave, 1 Hz-ish) so the user sees at
    // a glance which pad is awaiting a bind.
    uint32_t fill, border, txt;
    if (!exists) {
        fill   = 0x14181EFF;
        border = 0x2A3038FF;
        txt    = 0x55595FFF;
    } else if (isListen) {
        const double t     = ImGui_GetTime(ctx);
        const double pulse = 0.5 + 0.5 * std::sin(t * 6.0);  // 0..1
        const uint32_t bA  = static_cast<uint32_t>(140 + 115 * pulse) & 0xFFu;
        fill   = 0x4A3A1AFF;
        border = (0xFFE040u << 8) | bA;
        txt    = 0xFFFFFFFF;
    } else if (isMapped) {
        fill   = 0x1F3A24FF;
        border = 0x60C060FF;
        txt    = 0xE0E0E0FF;
    } else {
        fill   = 0x252A33FF;
        border = 0x4A5060FF;
        txt    = 0xC0C4CAFF;
    }

    double rounding = 3.0;
    ImGui_DrawList_AddRectFilled(dl, ox, oy, ox + kPadW, oy + kPadH,
                                 fill, &rounding, nullptr);
    ImGui_DrawList_AddRect(dl, ox, oy, ox + kPadW, oy + kPadH,
                           border, &rounding, nullptr, nullptr);

    // Centred label.
    double tw = 0, th = 0;
    ImGui_CalcTextSize(ctx, pad.shortLabel, &tw, &th, nullptr, nullptr);
    ImGui_DrawList_AddText(dl,
        ox + (kPadW - float(tw)) / 2.0f,
        oy + (kPadH - float(th)) / 2.0f,
        txt, pad.shortLabel);

    // Inverted-flag mark — small "i" in upper-right corner.
    if (isMapped) {
        bool inv = false;
        for (const auto& m : uf8::user_plugins::get().maps) {
            if (m.match != g_editingMatch) continue;
            for (const auto& s : m.slots) {
                if (s.linkIdx == pad.linkIdx) { inv = s.inverted; break; }
            }
            break;
        }
        if (inv) {
            ImGui_DrawList_AddText(dl, ox + kPadW - 8, oy + 1, 0xFFC04CFF, "i");
        }
    }

    // ---- ImGui interaction layer over the pad ---------------------------
    // SetCursorScreenPos + InvisibleButton makes this a real ImGui item so
    // BeginDragDropTarget / BeginPopupContextItem hook off it correctly.
    char btnId[32];
    std::snprintf(btnId, sizeof(btnId), "##fxl_pad_%d", pad.linkIdx);
    ImGui_SetCursorScreenPos(ctx, ox, oy);
    int ibFlags = 0;
    ImGui_InvisibleButton(ctx, btnId, kPadW, kPadH, &ibFlags);

    // Left click = toggle listen. Disabled for slots not in the topology.
    int lbtn = 0;
    if (exists && ImGui_IsItemClicked(ctx, &lbtn) && lbtn == 0) {
        g_listeningLinkIdx = isListen ? -1 : pad.linkIdx;
    }

    // Hover tooltip: long slot name + currently-bound param info. Only when
    // the slot exists in the canonical topology.
    if (exists && ImGui_IsItemHovered(ctx, nullptr)) {
        char tip[256];
        if (isMapped) {
            char pname[128] = {};
            if (fx.ok) {
                TrackFX_GetParamName(fx.tr, fx.fxIdx, mapped,
                                     pname, sizeof(pname));
            }
            std::snprintf(tip, sizeof(tip),
                "%s\n  -> param %d  '%s'",
                slot->name ? slot->name : "(slot)", mapped, pname);
        } else {
            std::snprintf(tip, sizeof(tip),
                "%s\n  unmapped — drag a param here to bind",
                slot->name ? slot->name : "(slot)");
        }
        ImGui_SetTooltip(ctx, tip);
    }

    // Drag-drop target — accept "FXL_PARAM" payload (vst3 param index as
    // ASCII). Bind directly without going through the listening state.
    if (exists && ImGui_BeginDragDropTarget(ctx)) {
        char payload[16] = {};
        int  dropFlags   = 0;
        if (ImGui_AcceptDragDropPayload(ctx, "FXL_PARAM",
                                        payload, int(sizeof(payload)),
                                        &dropFlags)) {
            const int p = std::atoi(payload);
            if (p >= 0) {
                bindSlot_(pad.linkIdx, p);
                // If the user was click-and-turn listening on this same
                // pad, advance to the next unmapped slot — DnD finishes
                // the bind, no need for a second click to move on.
                if (g_listeningLinkIdx == pad.linkIdx) {
                    autoAdvanceListening_(topo);
                }
            }
        }
        ImGui_EndDragDropTarget(ctx);
    }

    // Right-click context menu — only meaningful when something is mapped.
    // Open the popup and remember which slot it belongs to so the popup
    // body can render the right actions.
    if (exists && isMapped) {
        char popId[40];
        std::snprintf(popId, sizeof(popId), "fxl_ctx_%d", pad.linkIdx);
        if (ImGui_BeginPopupContextItem(ctx, popId, nullptr)) {
            char title[160];
            std::snprintf(title, sizeof(title),
                "%s  -> param %d",
                slot->name ? slot->name : "(slot)", mapped);
            ImGui_TextDisabled(ctx, title);
            ImGui_Separator(ctx);

            bool inverted = false;
            for (const auto& m : uf8::user_plugins::get().maps) {
                if (m.match != g_editingMatch) continue;
                for (const auto& s : m.slots) {
                    if (s.linkIdx == pad.linkIdx) { inverted = s.inverted; break; }
                }
                break;
            }
            char invLbl[40];
            std::snprintf(invLbl, sizeof(invLbl),
                inverted ? "Inverted [on]" : "Inverted [off]");
            if (ImGui_MenuItem(ctx, invLbl, nullptr, nullptr, nullptr)) {
                toggleInverted_(pad.linkIdx);
            }
            if (ImGui_MenuItem(ctx, "Clear binding", nullptr,
                               nullptr, nullptr)) {
                if (g_listeningLinkIdx == pad.linkIdx)
                    g_listeningLinkIdx = -1;
                unbindSlot_(pad.linkIdx);
            }
            ImGui_EndPopup(ctx);
        }
    }
}

// Render the full schematic into the current window. Caller owns the
// surrounding child-window + scroll. Origin = current cursor screen pos.
void drawFxLearnSchematic_(ImGui_Context* ctx,
                           const uf8::PluginMap& topo,
                           uf8::Domain domain,
                           const EditingFx& fx)
{
    const DomainSchematic ds = schematicFor_(domain);
    if (!ds.pads || ds.padCount == 0) {
        ImGui_TextDisabled(ctx,
            "(no schematic for this domain — slot list fallback TODO)");
        return;
    }

    double oxd = 0, oyd = 0;
    ImGui_GetCursorScreenPos(ctx, &oxd, &oyd);
    const float ox = float(oxd), oy = float(oyd);

    ImGui_DrawList* dl = ImGui_GetWindowDrawList(ctx);

    // Backplate — subtle chassis tint behind the whole canvas. Keeps the
    // schematic visually distinct from the surrounding panels.
    {
        double rnd = 6.0;
        ImGui_DrawList_AddRectFilled(dl, ox, oy,
            ox + ds.width, oy + ds.height,
            0x10141AFF, &rnd, nullptr);
        ImGui_DrawList_AddRect(dl, ox, oy,
            ox + ds.width, oy + ds.height,
            0x252A33FF, &rnd, nullptr, nullptr);
    }

    // Section labels.
    for (int i = 0; i < ds.sectionCount; ++i) {
        const auto& sec = ds.sections[i];
        ImGui_DrawList_AddText(dl,
            ox + sec.x, oy + sec.y,
            0x9CA0AAFF, sec.title);
        // Underline under the label.
        double tw = 0, th = 0;
        ImGui_CalcTextSize(ctx, sec.title, &tw, &th, nullptr, nullptr);
        double thick = 1.0;
        ImGui_DrawList_AddLine(dl,
            ox + sec.x, oy + sec.y + float(th) + 1,
            ox + sec.x + ds.width - 8 - float(sec.x), oy + sec.y + float(th) + 1,
            0x2A3038FF, &thick);
    }

    // Pads.
    for (int i = 0; i < ds.padCount; ++i) {
        const auto& pad = ds.pads[i];
        drawSchematicPad_(ctx, dl, ox + pad.x, oy + pad.y, pad, topo, fx);
    }

    // Reserve content rect so the parent BeginChild scrolls to fit.
    ImGui_SetCursorScreenPos(ctx, oxd, oyd);
    ImGui_Dummy(ctx, ds.width, ds.height);
}

// Render the Editor-View. Pre-condition: g_editingMatch is non-empty and
// names a map currently in the catalog. Caller is drawFxLearn.
void drawFxLearnEditor_(ImGui_Context* ctx)
{
    using namespace uf8;

    // Resolve the editing map. If the catalog no longer contains it
    // (e.g. user deleted via another path), bail back to master-view.
    const UserPluginMap* editing = nullptr;
    for (const auto& m : user_plugins::get().maps) {
        if (m.match == g_editingMatch) { editing = &m; break; }
    }
    if (!editing) {
        g_editingMatch.clear();
        g_listeningLinkIdx = -1;
        return;
    }

    // ESC clears any in-progress listening — matches the plan-doc
    // §"Interaktion" expectation. Only fires when nothing else is
    // capturing keyboard (e.g. param-filter input box has focus).
    if (g_listeningLinkIdx >= 0) {
        bool repeat = false;
        if (ImGui_IsKeyPressed(ctx, ImGui_Key_Escape, &repeat)) {
            g_listeningLinkIdx = -1;
        }
    }

    // ---- Header / breadcrumb --------------------------------------------
    if (ImGui_Button(ctx, "<- All maps##fxl_back", nullptr, nullptr)) {
        g_editingMatch.clear();
        g_listeningLinkIdx = -1;
        return;
    }
    ImGui_SameLine(ctx, nullptr, nullptr);
    char hdr[256];
    std::snprintf(hdr, sizeof(hdr), "  Editing: %s  [%s]",
                  editing->match.c_str(), domainLabel_(editing->domain));
    ImGui_Text(ctx, hdr);

    // Reset the FX-selector key when the editing map changes — the old
    // key (track:fx pair) means nothing for a different map.
    if (g_fxSelectorScope != editing->match) {
        g_fxSelectorKey.clear();
        g_fxSelectorScope = editing->match;
    }

    // Enumerate every FX-instance whose name contains the match — fed to
    // the plugin-selector combo and to pickEditingFx_.
    const auto fxList = findEditingFxAll_(editing->match);
    const EditingFx fx = pickEditingFx_(fxList);

    if (fxList.empty()) {
        ImGui_Spacing(ctx);
        ImGui_TextColored(ctx, 0xFFC04CFF,
            "No FX matching that name found on any track. Insert one to "
            "see its parameter list.");
        ImGui_Spacing(ctx);

        // Pick target: first selected track, else first track in project,
        // else master. Surface the choice in the button label so the user
        // doesn't have to guess where it landed.
        MediaTrack* target = GetSelectedTrack(nullptr, 0);
        const char* targetLabel = "selected track";
        if (!target) {
            target = GetTrack(nullptr, 0);
            targetLabel = "first track";
        }
        if (!target) {
            target = GetMasterTrack(nullptr);
            targetLabel = "master track";
        }

        char insLabel[160];
        std::snprintf(insLabel, sizeof(insLabel),
            "Insert '%s' on %s##fxl_ins",
            editing->match.c_str(), targetLabel);
        if (target && ImGui_Button(ctx, insLabel, nullptr, nullptr)) {
            // TrackFX_AddByName matches partial / fuzzy. instantiate=-1
            // means "always add a new instance".
            const int idx = TrackFX_AddByName(target, editing->match.c_str(),
                                              /*recFX*/ false,
                                              /*instantiate*/ -1);
            if (idx >= 0) {
                // 3 = show floating window so the user can wiggle controls
                // and the GetLastTouchedFX poll picks them up.
                TrackFX_Show(target, idx, 3);
            } else {
                g_lastSaveError =
                    "Insert failed — REAPER couldn't find a plug-in matching "
                    "the match string. Edit the match in the master view to "
                    "match an installed plug-in name.";
            }
        }
    } else {
        // Plugin-selector combo. Preview shows the active instance's
        // label; opening the combo lists all matches across master + tracks.
        // Auto-pick row at the top falls back to first-match behaviour.
        const char* preview = nullptr;
        for (const auto& e : fxList) {
            if (e.key == g_fxSelectorKey) { preview = e.label.c_str(); break; }
        }
        if (!preview) preview = fxList[0].label.c_str();

        ImGui_Text(ctx, "  Instance:");
        ImGui_SameLine(ctx, nullptr, nullptr);
        int comboFlags = 0;
        if (ImGui_BeginCombo(ctx, "##fxl_instance", preview, &comboFlags)) {
            // "Auto" entry — clears the selector.
            const bool autoActive = g_fxSelectorKey.empty();
            bool sel0 = autoActive;
            int  sf0  = 0;
            if (ImGui_Selectable(ctx, "Auto (first match)",
                                 &sel0, &sf0, nullptr, nullptr)) {
                g_fxSelectorKey.clear();
            }
            for (const auto& e : fxList) {
                bool sel = (e.key == g_fxSelectorKey);
                int  sf  = 0;
                char rowId[700];
                std::snprintf(rowId, sizeof(rowId), "%s##fxl_inst_%s",
                              e.label.c_str(), e.key.c_str());
                if (ImGui_Selectable(ctx, rowId, &sel, &sf, nullptr, nullptr)) {
                    g_fxSelectorKey = e.key;
                }
            }
            ImGui_EndCombo(ctx);
        }
    }

    if (!g_lastSaveError.empty()) {
        ImGui_Spacing(ctx);
        ImGui_TextColored(ctx, 0xCC4444FF, g_lastSaveError.c_str());
    }

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    // ---- Two-column body ------------------------------------------------
    const PluginMap* topo = canonicalTopology_(editing->domain);
    if (!topo) {
        ImGui_TextDisabled(ctx,
            "No canonical slot topology available for this domain.");
        return;
    }

    // Click-and-turn: while listening, poll REAPER's GetLastTouchedFX
    // and bind the touched param to the listening slot if the touched
    // FX's name contains our editing match. Snapshot baseline whenever
    // listening just started or auto-advanced so prior touches don't
    // auto-bind on entry.
    if (g_listeningLinkIdx >= 0 && g_listeningLinkIdx != g_listeningPrevIdx) {
        int t = -1, f = -1, p = -1;
        if (GetLastTouchedFX(&t, &f, &p)) {
            g_lastTouchedTr = t; g_lastTouchedFx = f; g_lastTouchedParam = p;
        } else {
            g_lastTouchedTr = -1; g_lastTouchedFx = -1; g_lastTouchedParam = -1;
        }
    }
    g_listeningPrevIdx = g_listeningLinkIdx;

    if (g_listeningLinkIdx >= 0) {
        int t = -1, f = -1, p = -1;
        if (GetLastTouchedFX(&t, &f, &p)) {
            const bool changed = (t != g_lastTouchedTr ||
                                  f != g_lastTouchedFx ||
                                  p != g_lastTouchedParam);
            if (changed) {
                MediaTrack* tr = nullptr;
                if (t == 0)      tr = GetMasterTrack(nullptr);
                else if (t > 0)  tr = GetTrack(nullptr, t - 1);
                if (tr) {
                    char fxName[256] = {};
                    if (TrackFX_GetFXName(tr, f, fxName, sizeof(fxName)) &&
                        std::string(fxName).find(editing->match) != std::string::npos)
                    {
                        bindSlot_(g_listeningLinkIdx, p);
                        autoAdvanceListening_(*topo);
                    }
                }
                g_lastTouchedTr = t; g_lastTouchedFx = f; g_lastTouchedParam = p;
            }
        }
    }

    double avX = 0.0, avY = 0.0;
    ImGui_GetContentRegionAvail(ctx, &avX, &avY);
    if (avX < 200.0) avX = 600.0;   // safety for embedded contexts
    if (avY < 200.0) avY = 360.0;
    // Non-const because ImGui_BeginChild takes double* (in/out for the
    // size hint). WDL's macros also #define min/max, so std::min has to
    // be parenthesised to dodge the macro.
    double leftW  = avX * 0.52;
    double rightW = avX - leftW - 12.0;  // gutter

    // Left pane — vector schematic of the canonical SSL 360 Link
    // topology. Slots render as colour-coded pads; click to listen,
    // drag a param onto a pad to bind, right-click for context menu.
    int childFlags = 0, winFlags = 0;
    double hLeft = avY - 8.0;
    if (ImGui_BeginChild(ctx, "fxl_slots", &leftW, &hLeft,
                         &childFlags, &winFlags)) {
        drawFxLearnSchematic_(ctx, *topo, editing->domain, fx);
        ImGui_EndChild(ctx);
    }

    ImGui_SameLine(ctx, nullptr, nullptr);

    // Right pane — param list.
    if (ImGui_BeginChild(ctx, "fxl_params", &rightW, &hLeft,
                         &childFlags, &winFlags)) {
        if (!fx.ok) {
            ImGui_TextDisabled(ctx, "Insert a matching FX to list its params.");
        } else {
            char hint[256];
            if (g_listeningLinkIdx >= 0) {
                const LinkSlot* listenSlot =
                    findSlotByLinkIdx(*topo, g_listeningLinkIdx);
                std::snprintf(hint, sizeof(hint),
                    "Listening for: %s\n"
                    "  - click a parameter below, OR\n"
                    "  - wiggle the control in the plug-in window",
                    listenSlot ? listenSlot->name : "(slot)");
                ImGui_TextColored(ctx, 0xFFE040FF, hint);
            } else {
                ImGui_TextDisabled(ctx,
                    "Drag a param onto a slot, or click a slot to start "
                    "listening.");
            }
            ImGui_Spacing(ctx);

            ImGui_Text(ctx, "Filter:");
            ImGui_SameLine(ctx, nullptr, nullptr);
            double filterW = rightW - 80.0;
            if (filterW < 80.0) filterW = 80.0;
            // No SetNextItemWidth in this ReaImGui sig set; use plain
            // InputTextWithHint at default width.
            ImGui_InputTextWithHint(ctx, "##fxl_param_filter",
                "type to filter...", g_paramFilter,
                static_cast<int>(sizeof(g_paramFilter)),
                nullptr, nullptr);
            ImGui_Spacing(ctx);
            ImGui_Separator(ctx);
            ImGui_Spacing(ctx);

            // Build a quick reverse-map: vst3Param -> linkIdx (any of
            // the editing map's slots). Lets us tag the "Already mapped
            // -> Slot Name" badge in the param list.
            std::unordered_map<int, int> usedBy;
            for (const auto& slt : editing->slots) {
                usedBy[slt.vst3Param] = slt.linkIdx;
            }

            const int paramCount = TrackFX_GetNumParams(fx.tr, fx.fxIdx);
            // Cap iteration so a 5000-param plugin doesn't tank the
            // frame; the user can always sharpen the filter to reach
            // the rest. 1024 is comfortably above any musical plugin.
            const int kMaxParams = 1024;
            const int n = (std::min)(paramCount, kMaxParams);
            const std::string filt = g_paramFilter;

            char pname[128];
            for (int p = 0; p < n; ++p) {
                pname[0] = 0;
                TrackFX_GetParamName(fx.tr, fx.fxIdx, p, pname, sizeof(pname));

                if (!filt.empty()) {
                    if (std::string(pname).find(filt) == std::string::npos)
                        continue;
                }

                // Visually mark already-mapped params by appending the
                // bound slot name as a suffix in the button label.
                char rowLbl[256];
                auto it = usedBy.find(p);
                if (it != usedBy.end()) {
                    const LinkSlot* boundSlot =
                        findSlotByLinkIdx(*topo, it->second);
                    std::snprintf(rowLbl, sizeof(rowLbl),
                        "  [%4d] %-32s  -> %s##fxl_param_%d",
                        p, pname,
                        boundSlot ? boundSlot->name : "(slot)", p);
                } else {
                    std::snprintf(rowLbl, sizeof(rowLbl),
                        "  [%4d] %-32s##fxl_param_%d",
                        p, pname, p);
                }

                bool selected = false;
                int  selFlags = ImGui_SelectableFlags_AllowDoubleClick;
                if (ImGui_Selectable(ctx, rowLbl, &selected, &selFlags,
                                     nullptr, nullptr)) {
                    if (g_listeningLinkIdx >= 0) {
                        bindSlot_(g_listeningLinkIdx, p);
                        autoAdvanceListening_(*topo);
                    }
                }

                // Drag source — payload is the vst3 param index encoded
                // as ASCII. Schematic pads accept it via "FXL_PARAM" type.
                int dndFlags = 0;
                if (ImGui_BeginDragDropSource(ctx, &dndFlags)) {
                    char payload[16];
                    std::snprintf(payload, sizeof(payload), "%d", p);
                    ImGui_SetDragDropPayload(ctx, "FXL_PARAM", payload,
                                             nullptr);
                    char preview[160];
                    std::snprintf(preview, sizeof(preview),
                        "param %d  %s", p, pname);
                    ImGui_Text(ctx, preview);
                    ImGui_EndDragDropSource(ctx);
                }
            }

            if (paramCount > kMaxParams) {
                ImGui_Spacing(ctx);
                char overflow[96];
                std::snprintf(overflow, sizeof(overflow),
                    "(showing first %d of %d params — use filter)",
                    kMaxParams, paramCount);
                ImGui_TextDisabled(ctx, overflow);
            }
        }
        ImGui_EndChild(ctx);
    }
}

} // namespace

void SettingsScreen::drawFxLearn(ImGui_Context* ctx)
{
    using namespace uf8;

    // Editor-View takes over when a map is being edited. Master-View is
    // the default. Switch is driven by g_editingMatch (set by the Edit
    // button below; cleared by the editor's "<- All maps" breadcrumb).
    if (!g_editingMatch.empty()) {
        drawFxLearnEditor_(ctx);
        return;
    }

    ImGui_Text(ctx, "FX Learn — User Plugin Maps");
    ImGui_Spacing(ctx);
    ImGui_TextWrapped(ctx,
        "Teach third-party plug-ins to behave as virtual Channel-Strip or "
        "Bus-Comp. Built-in maps (SSL CS 2 / 4K B/E/G / BC 2 / 360 Link) "
        "always win — user maps can't shadow them.");
    ImGui_Spacing(ctx);

    if (ImGui_Button(ctx, "+ New##fxl_new", nullptr, nullptr)) {
        std::memset(g_newMatch,   0, sizeof(g_newMatch));
        std::memset(g_newDisplay, 0, sizeof(g_newDisplay));
        g_newDomain = 1;
        g_newError.clear();
        std::memset(g_pickerFilter, 0, sizeof(g_pickerFilter));
        g_pickerSelectedIdx = -1;
        if (g_installedFx.empty()) loadInstalledFx_();
        ImGui_OpenPopup(ctx, "fxl_new_popup", nullptr);
    }
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_TextDisabled(ctx, "Import / Export — TODO");

    if (!g_lastSaveError.empty()) {
        ImGui_Spacing(ctx);
        // Use TextColored via packed RGBA (red, fully opaque).
        ImGui_TextColored(ctx, 0xCC4444FF, g_lastSaveError.c_str());
    }

    ImGui_Spacing(ctx);
    ImGui_Separator(ctx);
    ImGui_Spacing(ctx);

    const auto& cat = user_plugins::get();

    if (cat.maps.empty()) {
        ImGui_TextDisabled(ctx,
            "No user plugin maps yet. Click '+ New' to teach a third-party "
            "plug-in.");
    } else {
        const int kCols = 6;
        int tblFlags = 0;  // default: borders=outer, no row-bg
        // CRITICAL: each TableSetupColumn that passes a non-null init_width
        // MUST also pass WidthFixed in flags — otherwise ImGui interprets
        // the value as a stretch *weight*, which with values like 36..240
        // explodes the column layout and bricks the parent window for the
        // next frame (Begin returns false forever). See learnings.md
        // "ReaImGui TableSetupColumn pixel-width trap" (2026-05-03).
        if (ImGui_BeginTable(ctx, "fxl_master", kCols, &tblFlags,
                             nullptr, nullptr, nullptr)) {
            int wFlag = ImGui_TableColumnFlags_WidthFixed;
            double wDefault = 36.0, wShort = 64.0, wMatch = 240.0,
                   wDomain = 50.0, wSlots = 64.0, wActions = 100.0;
            ImGui_TableSetupColumn(ctx, "Default", &wFlag, &wDefault, nullptr);
            ImGui_TableSetupColumn(ctx, "Short",   &wFlag, &wShort,   nullptr);
            ImGui_TableSetupColumn(ctx, "Match",   &wFlag, &wMatch,   nullptr);
            ImGui_TableSetupColumn(ctx, "Domain",  &wFlag, &wDomain,  nullptr);
            ImGui_TableSetupColumn(ctx, "Slots",   &wFlag, &wSlots,   nullptr);
            ImGui_TableSetupColumn(ctx, "Actions", &wFlag, &wActions, nullptr);
            ImGui_TableHeadersRow(ctx);

            // Index loop so per-row IDs are stable. A drop while iterating
            // would invalidate references; defer destructive actions to
            // post-loop via the popup.
            for (size_t i = 0; i < cat.maps.size(); ++i) {
                const UserPluginMap& m = cat.maps[i];

                ImGui_TableNextRow(ctx, nullptr, nullptr);

                // Default — clickable star toggle.
                ImGui_TableNextColumn(ctx);
                {
                    char btnId[64];
                    std::snprintf(btnId, sizeof(btnId),
                        "%s##fxl_def_%zu", m.isDefault ? "*" : "-", i);
                    if (ImGui_Button(ctx, btnId, nullptr, nullptr)) {
                        UserPluginMap copy = m;
                        copy.isDefault = !copy.isDefault;
                        user_plugins::upsert(std::move(copy));
                        persistAndReport_();
                    }
                }

                ImGui_TableNextColumn(ctx);
                ImGui_Text(ctx,
                    m.displayShort.empty() ? "USR" : m.displayShort.c_str());

                ImGui_TableNextColumn(ctx);
                ImGui_Text(ctx, m.match.c_str());

                ImGui_TableNextColumn(ctx);
                ImGui_Text(ctx, domainLabel_(m.domain));

                ImGui_TableNextColumn(ctx);
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%zu", m.slots.size());
                    ImGui_Text(ctx, buf);
                }

                ImGui_TableNextColumn(ctx);
                {
                    char editId[64];
                    std::snprintf(editId, sizeof(editId),
                                  "Edit##fxl_edit_%zu", i);
                    if (ImGui_Button(ctx, editId, nullptr, nullptr)) {
                        g_editingMatch     = m.match;
                        g_listeningLinkIdx = -1;
                        std::memset(g_paramFilter, 0, sizeof(g_paramFilter));
                    }

                    ImGui_SameLine(ctx, nullptr, nullptr);
                    char delId[64];
                    std::snprintf(delId, sizeof(delId),
                                  "Del##fxl_del_%zu", i);
                    if (ImGui_Button(ctx, delId, nullptr, nullptr)) {
                        // Defer the OpenPopup to the outer scope so the
                        // popup ID-stack matches BeginPopupModal below;
                        // calling OpenPopup inside the table cell uses a
                        // deeper ID prefix, BeginPopupModal can't find it.
                        g_pendingDeleteMatch = m.match;
                        g_pendingDeleteOpen  = true;
                    }
                }
            }

            ImGui_EndTable(ctx);
        }
    }

    // Hoist the deferred OpenPopup for Delete out of the table-cell scope
    // (see g_pendingDeleteOpen comment).
    if (g_pendingDeleteOpen) {
        ImGui_OpenPopup(ctx, "fxl_del_popup", nullptr);
        g_pendingDeleteOpen = false;
    }

    // ---- "+ New" popup ----------------------------------------------------
    if (ImGui_BeginPopupModal(ctx, "fxl_new_popup", nullptr, nullptr)) {
        ImGui_Text(ctx, "New User Plugin Map");
        ImGui_Spacing(ctx);

        // -- Plugin browser --------------------------------------------------
        ImGui_Text(ctx, "Pick a plug-in:");
        ImGui_SameLine(ctx, nullptr, nullptr);
        char counter[64];
        std::snprintf(counter, sizeof(counter), "(%zu installed)",
                      g_installedFx.size());
        ImGui_TextDisabled(ctx, counter);
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Reload##fxl_picker_reload", nullptr, nullptr)) {
            loadInstalledFx_();
            g_pickerSelectedIdx = -1;
        }

        ImGui_InputTextWithHint(ctx, "##fxl_picker_filter",
            "type to filter — e.g. 'fabfilter pro-q'",
            g_pickerFilter, static_cast<int>(sizeof(g_pickerFilter)),
            nullptr, nullptr);

        // Filterable list. Case-insensitive substring match.
        std::string flt = g_pickerFilter;
        for (auto& c : flt) c = static_cast<char>(std::tolower(c));

        // Lowercased haystacks computed lazily once per render — fine for
        // a few thousand entries; if it ever shows up in profiles we can
        // pre-compute on load.
        double childW = 0, childH = 240.0;
        int    childFlags = 0, winFlags = 0;
        if (ImGui_BeginChild(ctx, "fxl_picker_list", &childW, &childH,
                             &childFlags, &winFlags)) {
            for (size_t i = 0; i < g_installedFx.size(); ++i) {
                if (!flt.empty()) {
                    std::string lc = g_installedFx[i].name;
                    for (auto& c : lc) c = static_cast<char>(std::tolower(c));
                    if (lc.find(flt) == std::string::npos) continue;
                }
                bool selected = (int(i) == g_pickerSelectedIdx);
                int  selFlags = 0;
                char rowId[640];
                std::snprintf(rowId, sizeof(rowId), "%s##fxl_pick_%zu",
                              g_installedFx[i].name.c_str(), i);
                if (ImGui_Selectable(ctx, rowId, &selected, &selFlags,
                                     nullptr, nullptr)) {
                    g_pickerSelectedIdx = int(i);
                    // Auto-fill match (FX name minus "VSTn: " prefix &
                    // trailing " (vendor)") + 4-char display label.
                    std::string m = g_installedFx[i].name;
                    auto colon = m.find(": ");
                    if (colon != std::string::npos && colon < 8)
                        m = m.substr(colon + 2);
                    auto paren = m.rfind(" (");
                    if (paren != std::string::npos) m = m.substr(0, paren);
                    while (!m.empty() && m.front() == ' ') m.erase(m.begin());
                    while (!m.empty() && m.back()  == ' ') m.pop_back();
                    std::strncpy(g_newMatch, m.c_str(),
                                 sizeof(g_newMatch) - 1);
                    g_newMatch[sizeof(g_newMatch) - 1] = '\0';
                    std::string s = deriveShortLabel_(g_installedFx[i].name);
                    std::strncpy(g_newDisplay, s.c_str(), 4);
                    g_newDisplay[4] = '\0';
                }
            }
            ImGui_EndChild(ctx);
        }

        ImGui_Spacing(ctx);
        ImGui_Separator(ctx);
        ImGui_Spacing(ctx);

        ImGui_Text(ctx, "Match (substring of FX name — auto-filled, editable):");
        ImGui_InputTextWithHint(ctx, "##fxl_new_match",
            "e.g. 'FabFilter Pro-Q 4'",
            g_newMatch, static_cast<int>(sizeof(g_newMatch)),
            nullptr, nullptr);

        ImGui_Spacing(ctx);
        ImGui_Text(ctx, "4-char display label (scribble-strip zone):");
        // displayShort caps at 4 chars; back the field with a 5-byte buf.
        ImGui_InputTextWithHint(ctx, "##fxl_new_short",
            "FFP4",
            g_newDisplay, 5, nullptr, nullptr);

        ImGui_Spacing(ctx);
        ImGui_Text(ctx, "Domain:");
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_RadioButton(ctx, "Channel-Strip##fxl_new_cs", g_newDomain == 1))
            g_newDomain = 1;
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_RadioButton(ctx, "Bus-Comp##fxl_new_bc", g_newDomain == 2))
            g_newDomain = 2;

        if (!g_newError.empty()) {
            ImGui_Spacing(ctx);
            ImGui_TextColored(ctx, 0xCC4444FF, g_newError.c_str());
        }

        ImGui_Spacing(ctx);
        ImGui_Separator(ctx);
        ImGui_Spacing(ctx);

        if (ImGui_Button(ctx, "Create##fxl_new_ok", nullptr, nullptr)) {
            g_newError.clear();

            std::string match = g_newMatch;
            std::string disp  = g_newDisplay;
            if (disp.size() > 4) disp.resize(4);

            // Trim leading/trailing whitespace from the match. Inner spaces
            // are preserved on purpose ('Pro-Q 4' is a real FX name).
            auto trim = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
            };
            trim(match);

            if (match.empty()) {
                g_newError = "Match string is required.";
            } else if (disp.empty()) {
                g_newError = "Display label is required (1..4 ASCII chars).";
            } else if (g_newDomain != 1 && g_newDomain != 2) {
                g_newError = "Pick a domain.";
            } else if (user_plugins::collidesWithBuiltin(match)) {
                g_newError = "That match string collides with a built-in plugin map.";
            } else {
                // Reject duplicate match within the catalog up-front so the
                // user gets a clear message instead of a silent overwrite.
                bool dup = false;
                for (const auto& existing : user_plugins::get().maps) {
                    if (existing.match == match) { dup = true; break; }
                }
                if (dup) {
                    g_newError = "A user map with that match already exists.";
                } else {
                    UserPluginMap m;
                    m.match        = match;
                    m.displayShort = disp;
                    m.domain       = (g_newDomain == 2) ? Domain::BusComp
                                                        : Domain::ChannelStrip;
                    m.isDefault    = false;
                    user_plugins::upsert(std::move(m));
                    persistAndReport_();
                    if (g_lastSaveError.empty())
                        ImGui_CloseCurrentPopup(ctx);
                    else
                        g_newError = g_lastSaveError;  // surfaced in popup too
                }
            }
        }
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Cancel##fxl_new_cancel", nullptr, nullptr)) {
            ImGui_CloseCurrentPopup(ctx);
        }
        ImGui_EndPopup(ctx);
    }

    // ---- Delete confirm popup --------------------------------------------
    if (ImGui_BeginPopupModal(ctx, "fxl_del_popup", nullptr, nullptr)) {
        ImGui_Text(ctx, "Delete user plugin map?");
        ImGui_Spacing(ctx);
        char line[256];
        std::snprintf(line, sizeof(line),
            "  match: %s", g_pendingDeleteMatch.c_str());
        ImGui_Text(ctx, line);
        ImGui_Spacing(ctx);
        ImGui_TextWrapped(ctx,
            "Tracks hosting this plug-in fall back to no mapping.");
        ImGui_Spacing(ctx);

        if (ImGui_Button(ctx, "Delete##fxl_del_ok", nullptr, nullptr)) {
            if (!g_pendingDeleteMatch.empty())
                user_plugins::removeByMatch(g_pendingDeleteMatch);
            g_pendingDeleteMatch.clear();
            persistAndReport_();
            ImGui_CloseCurrentPopup(ctx);
        }
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Cancel##fxl_del_cancel", nullptr, nullptr)) {
            g_pendingDeleteMatch.clear();
            ImGui_CloseCurrentPopup(ctx);
        }
        ImGui_EndPopup(ctx);
    }
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
