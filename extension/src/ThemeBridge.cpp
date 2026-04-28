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
    // REAPER theme indices that have stable meanings across versions
    // (reaper_plugin.h:1327+). The ones we use here:
    //   24 TRACKBG1  — main track background
    //   25 TRACKBG2  — alternating track background
    //   28 PEAKS1    — audio peak meter
    //   29 PEAKS2    — peak meter clip
    //   38 MARKER    — accent (timeline markers, also a reasonable Button highlight)
    constexpr int IDX_TRACKBG1 = 24;
    constexpr int IDX_TRACKBG2 = 25;
    constexpr int IDX_PEAKS1   = 28;
    constexpr int IDX_MARKER   = 38;

    const int bg     = themeRgba(IDX_TRACKBG1, 0x202020);
    const int bgAlt  = themeRgba(IDX_TRACKBG2, 0x282828);
    const int peak   = themeRgba(IDX_PEAKS1,   0x40C040);
    const int accent = themeRgba(IDX_MARKER,   0xC08040);

    int n = 0;
    auto push = [&](int colIdx, int rgba) {
        ImGui_PushStyleColor(ctx, colIdx, rgba);
        ++n;
    };

    push(ImGui_Col_WindowBg,      bg);
    push(ImGui_Col_ChildBg,       bgAlt);
    push(ImGui_Col_FrameBg,       shade(bgAlt, +12));
    push(ImGui_Col_FrameBgHovered, shade(bgAlt, +24));
    push(ImGui_Col_FrameBgActive,  shade(bgAlt, +36));
    push(ImGui_Col_Button,         shade(accent, -32));
    push(ImGui_Col_ButtonHovered,  accent);
    push(ImGui_Col_ButtonActive,   shade(accent, +24));
    push(ImGui_Col_PlotLines,      peak);
    push(ImGui_Col_PlotLinesHovered, shade(peak, +24));
    return n;
}

void ThemeBridge::popAll(ImGui_Context* ctx, int count)
{
    if (count > 0) ImGui_PopStyleColor(ctx, &count);
}

} // namespace uf8
