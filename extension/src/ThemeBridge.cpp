#include "ThemeBridge.h"

#include <cstdint>

#include "reaper_plugin_functions.h"
#include "reaper_imgui_functions.h"

namespace uf8 {

namespace {

// REAPER COLORREF (0x00BBGGRR, R at byte 0) → ReaImGui big-endian RGBA
// (0xRRGGBBAA, R at MSB). Confirmed against cfillion's Color::fromBigEndian
// in reaimgui/api/style.cpp.
inline int toRgba(intptr_t c, int alpha = 0xFF)
{
    const int r =  c        & 0xFF;
    const int g = (c >>  8) & 0xFF;
    const int b = (c >> 16) & 0xFF;
    return (r << 24) | (g << 16) | (b << 8) | (alpha & 0xFF);
}

// Pull theme color idx, fall back to `fallback` if REAPER returns 0
// (a black-on-black theme would be a degenerate case anyway).
inline int themeRgba(int idx, intptr_t fallback)
{
    const intptr_t c = GetColorTheme(idx, static_cast<int>(fallback));
    return toRgba(c == 0 ? fallback : c);
}

// Slight darken/lighten of an RGBA value. amount > 0 lightens, < 0 darkens.
// Used to derive Hovered/Active variants when REAPER's theme doesn't carry
// dedicated slots for them.
inline int shade(int rgba, int amount)
{
    auto adj = [&](int v) {
        v += amount;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        return v;
    };
    const int r = adj((rgba >> 24) & 0xFF);
    const int g = adj((rgba >> 16) & 0xFF);
    const int b = adj((rgba >>  8) & 0xFF);
    const int a =      rgba        & 0xFF;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

} // namespace

int ThemeBridge::pushAll(ImGui_Context* ctx)
{
    // Palette mirrors the UF8-schematic colours in drawUf8Vector so the
    // surrounding ImGui surface reads as one continuous mock-up rather
    // than "REAPER widget on top of a custom canvas". Hardware-button
    // tones come from drawHwBtn (kSelFill 0x4477CC, idle 0x252A33,
    // hover 0x3A4253, border 0x4A5060, txt 0xD0D4DA).
    constexpr int kBg          = 0x1A1E24FF;   // canvas background
    constexpr int kChild       = 0x202530FF;   // panel inset
    constexpr int kFrame       = 0x252A33FF;   // input fields, combos
    constexpr int kFrameHover  = 0x2E3440FF;
    constexpr int kFrameActive = 0x3A4253FF;
    constexpr int kBorder      = 0x4A5060FF;
    constexpr int kButton      = 0x2A3140FF;
    constexpr int kButtonHover = 0x3A4253FF;
    constexpr int kButtonAct   = 0x4477CCFF;   // schematic selection blue
    constexpr int kAccent      = 0x4477CCFF;   // headers / tabs / sliders
    constexpr int kAccentBri   = 0x6699EEFF;   // hover lift
    constexpr int kAccentDim   = 0x2A3F66FF;   // dimmed selection (inactive)
    constexpr int kText        = 0xD0D4DAFF;   // primary text
    constexpr int kTextDim     = 0x70747CFF;
    constexpr int kSeparator   = 0x383C44FF;

    int n = 0;
    auto push = [&](int colIdx, int rgba) {
        ImGui_PushStyleColor(ctx, colIdx, rgba);
        ++n;
    };

    // Conservative set — sticks to enum names that have been stable
    // across ImGui 1.89..1.91+. Skips the Tab* family (renamed to
    // TabSelected* etc. in 1.91, ReaImGui follows DearImGui), which
    // crashes here as a NULL function pointer when the deployed
    // dylib doesn't export the old names. Same caution for any
    // enum I can't verify against the user's installed version —
    // safer to undertheme than to take down REAPER on every tick.
    push(ImGui_Col_WindowBg,         kBg);
    push(ImGui_Col_ChildBg,          kChild);
    push(ImGui_Col_PopupBg,          kChild);
    push(ImGui_Col_Border,           kBorder);

    push(ImGui_Col_FrameBg,          kFrame);
    push(ImGui_Col_FrameBgHovered,   kFrameHover);
    push(ImGui_Col_FrameBgActive,    kFrameActive);

    push(ImGui_Col_Button,           kButton);
    push(ImGui_Col_ButtonHovered,    kButtonHover);
    push(ImGui_Col_ButtonActive,     kButtonAct);

    push(ImGui_Col_Header,           kAccentDim);
    push(ImGui_Col_HeaderHovered,    kAccentBri);
    push(ImGui_Col_HeaderActive,     kAccent);

    push(ImGui_Col_CheckMark,        kAccentBri);
    push(ImGui_Col_Separator,        kSeparator);

    push(ImGui_Col_Text,             kText);
    push(ImGui_Col_TextDisabled,     kTextDim);

    push(ImGui_Col_PlotLines,        0x40C040FF);
    return n;
}

void ThemeBridge::popAll(ImGui_Context* ctx, int count)
{
    if (count > 0) ImGui_PopStyleColor(ctx, &count);
}

} // namespace uf8
