#include "SettingsScreen.h"

#include <cmath>
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
        default:                    return uf8::bindings::toName(id);
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
    // hardware-face rectangle, highlights on hover/select.
    auto drawHwBtn = [&](float x, float y, float w, float h,
                         ButtonId id, const char* label)
    {
        const bool hot      = inside(x, y, w, h);
        const bool selected = (id == sel);
        if (hot && canvasClicked && leftBtn == 0) sel = id;

        const uint32_t fill   = selected ? 0x4477CCFF
                                : hot     ? 0x3A4253FF
                                          : 0x252A33FF;
        const uint32_t border = selected ? 0xAACCFFFF : 0x4A5060FF;
        const uint32_t txt    = selected ? 0xFFFFFFFF : 0xD0D4DAFF;
        rect_(c, x, y, w, h, fill, border, /*rounding*/ 3.5);
        drawTextCentered_(c, x + w / 2.0f, y + h / 2.0f, txt, label);
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
    for (int i = 0; i < 8; ++i) {
        const float sx = kStripX0 + i * (kStripW + kStripGap);
        // Top soft-key — taller (22 px) so a future label-text frame can
        // fit. Stays locked in v1.
        rect_(c, sx + 6, 12, kStripW - 12, 22, 0x252A33FF, 0x4A5060FF, 2.0);
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
    char buf[8];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 2; ++col) {
            std::snprintf(buf, sizeof(buf), "%d", row * 2 + col + 1);
            drawLocked(13 + col * 42, 152 + row * 26, 38, 22, buf);
        }
    }

    // PLUGIN / CHANNEL — wider buttons so their silk-screen labels fit.
    // CHANNEL is locked in v1 (DAW-mode-only on the real UF8) but the
    // schematic shows it for layout fidelity.
    drawHwBtn(13, 260, 50, 22, ButtonId::PluginBtn, "PLUGIN");
    drawLocked(67, 260, 50, 22, "CHANNEL");

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
    // SOFT KEYS — 2x3 grid spans the full right-panel width now that
    // PAN / FINE moved underneath.
    drawGroupLabel(852, 6, "SOFT KEYS");
    drawLocked(852, 22, 42, 20, "V-POT");
    drawLocked(898, 22, 42, 20, "1");
    drawLocked(944, 22, 41, 20, "2");
    drawLocked(852, 46, 42, 20, "3");
    drawLocked(898, 46, 42, 20, "4");
    drawLocked(944, 46, 41, 20, "5");

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
    std::string*                midiDevice;
    int*                        midiChannel;
    int*                        midiMsgType;
    int*                        midiData1;
    int*                        midiData2;
};

bool drawActionPicker(ImGui_Context* ctx, const char* prefix,
                      ActionFieldsRef f)
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
                                    "e.g. 40044 (Track: Toggle FX bypass)",
                                    buf, sizeof(buf),
                                    /*flags*/ nullptr,
                                    /*callback*/ nullptr)) {
            *f.action = buf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
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

// Editor panel — two-column layout. Left: primary action + behavior.
// Right: long-press (only when primary behavior == Momentary).
// Auto-saves on every change.
void drawBindingEditor(ImGui_Context* ctx, int layer, ButtonId id)
{
    using namespace uf8::bindings;

    Binding bd    = getBinding(layer, id);
    bool    dirty = false;

    char header[80];
    std::snprintf(header, sizeof(header),
                  "Editing: %s   (Layer %d)", hwFaceLabel(id), layer + 1);
    ImGui_Text(ctx, header);
    ImGui_Separator(ctx);

    ImGui_PushID(ctx, uf8::bindings::toName(id));
    const int themePushed = pushBindingsTheme(ctx);

    // Two columns. Each child sized half the available width with
    // matching height so the bordered panels read as a pair.
    double availX = 0, availY = 0;
    ImGui_GetContentRegionAvail(ctx, &availX, &availY);
    double colW = (availX - 16) / 2.0;
    if (colW < 280) colW = 280;
    const double colH = 320;

    // ---- Left column: PRIMARY ACTION ----
    // child_flags = ChildFlags_Borders matches the previous "bool border=true"
    // intent under v0.1.1. v0.10 BeginChild's 5th arg is int* child_flags
    // (vendored header patched 2026-05-02 — see learnings rule 17).
    {
        double w = colW, h = colH;
        int childFlags = ImGui_ChildFlags_Borders;
        if (ImGui_BeginChild(ctx, "primary_col", &w, &h, &childFlags, nullptr)) {
            ImGui_Text(ctx, "PRIMARY ACTION");
            ImGui_Separator(ctx);

            ActionFieldsRef pri{
                &bd.type, &bd.action, &bd.param,
                &bd.midiDevice, &bd.midiChannel, &bd.midiMsgType,
                &bd.midiData1, &bd.midiData2,
            };
            if (drawActionPicker(ctx, "pri", pri)) dirty = true;

            ImGui_Spacing(ctx);
            ImGui_Separator(ctx);

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
        }
        ImGui_EndChild(ctx);
    }

    ImGui_SameLine(ctx, /*offset_from_start_x*/ nullptr,
                   /*spacing*/ nullptr);

    // ---- Right column: LONG-PRESS ACTION ----
    {
        double w = colW, h = colH;
        int childFlags = ImGui_ChildFlags_Borders;
        if (ImGui_BeginChild(ctx, "longpress_col", &w, &h, &childFlags, nullptr)) {
            ImGui_Text(ctx, "LONG-PRESS  (held > 0.5 s)");
            ImGui_Separator(ctx);

            if (bd.behavior != Behavior::Momentary) {
                ImGui_Text(ctx, "Long-press is only available for");
                ImGui_Text(ctx, "Momentary primary actions.");
                if (bd.hasLongPress) {
                    bd.hasLongPress = false;
                    dirty = true;
                }
            } else {
                bool en = bd.hasLongPress;
                if (ImGui_Checkbox(ctx, "Enable long-press action##lp_en", &en)) {
                    bd.hasLongPress = en;
                    if (en && bd.longPressType == ActionType::Noop) {
                        bd.longPressType = ActionType::Builtin;
                    }
                    dirty = true;
                }
                if (bd.hasLongPress) {
                    ImGui_Spacing(ctx);
                    ActionFieldsRef lp{
                        &bd.longPressType, &bd.longPressAction,
                        &bd.longPressParam,
                        &bd.longPressMidiDevice, &bd.longPressMidiChannel,
                        &bd.longPressMidiMsgType,
                        &bd.longPressMidiData1, &bd.longPressMidiData2,
                    };
                    if (drawActionPicker(ctx, "lp", lp)) dirty = true;
                }
            }
        }
        ImGui_EndChild(ctx);
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
