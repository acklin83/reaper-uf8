#include "SettingsScreen.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Bindings.h"
#include "CsiImport.h"
#include "Protocol.h"
#include "reaper_imgui_functions.h"

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
    for (int i = 0; i < 8; ++i) {
        const float sx = kStripX0 + i * (kStripW + kStripGap);
        // Top soft-key — clickable so the user can edit the per-strip
        // binding directly from the schematic.
        char tlbl[4];
        std::snprintf(tlbl, sizeof(tlbl), "%d", i + 1);
        drawHwBtn(sx + 6, 12, kStripW - 12, 22, kStripTsk[i], tlbl);
        // Scribble LCD with placeholder logo
        rect_(c, sx + 4, 40, kStripW - 8, 58, 0x080C12FF, 0x444A55FF, 2.0);
        drawTextCentered_(c, sx + kStripW / 2.0f, 60, 0x4488DDFF, "SSL");
        drawTextCentered_(c, sx + kStripW / 2.0f, 78, 0x4488DDFF, "UF8");
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
            for (auto& n : builtinNames()) {
                std::string lbl = builtinDisplayName(n);
                if (lbl != n) lbl += "   [" + n + "]";
                bool s = (n == *f.action);
                if (ImGui_Selectable(ctx, lbl.c_str(), &s,
                                     nullptr, nullptr, nullptr)) {
                    *f.action = n;
                    dirty = true;
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
            int sslBankIdx = -1;
            if (*f.action == "ssl_softkey") {
                sslBankIdx = 0;   // V-POT labels as a hint
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
            } else if (sslBankIdx >= 0) {
                // Build a combo from the bank's labels. Empty slots
                // (some banks have gaps in the SSL plug-in spec) show
                // as "(empty)" so the user sees the slot exists but
                // does nothing if pressed.
                const char* const* labels =
                    reasixty_softkeyStockLabels(/*domain*/ 0, sslBankIdx);
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
                    // Auto-fill the slot's display label with the SSL
                    // function name so it shows on the LCD without the
                    // user typing it in. The user can still override
                    // afterwards — we only auto-fill when the label
                    // either is empty or matches the previous slot's
                    // SSL name (i.e. wasn't a user override).
                    if (f.label && labels) {
                        const char* prevName =
                            labels[std::clamp(slot, 0, 7)];
                        // To detect "was previously the SSL name", we
                        // accept any of the bank's labels as a hint.
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

    static const char* kModNames[]   = { "Plain", "+ Shift / Fine",
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
