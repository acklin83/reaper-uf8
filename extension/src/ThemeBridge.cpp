#include "ThemeBridge.h"

namespace uf8 {

namespace {
uint32_t s_lastHash = 0;
}

void ThemeBridge::reapply()
{
    // Phase 2.6a:
    //   int sz = 0;
    //   void* theme = GetColorThemeStruct(&sz);
    //   if (theme && sz >= sizeof(ColorTheme)) {
    //       const ColorTheme* ct = static_cast<const ColorTheme*>(theme);
    //       auto& style = ImGui::GetStyle();
    //       style.Colors[ImGuiCol_WindowBg]   = fromBGR(ct->mixer_bg);
    //       style.Colors[ImGuiCol_FrameBg]    = fromBGR(ct->mcp_fader_bg);
    //       style.Colors[ImGuiCol_Text]       = fromBGR(ct->mcp_fadertext);
    //       style.Colors[ImGuiCol_PlotLines]  = fromBGR(GetColorTheme(28)); // PEAKS1
    //       … (full mapping table in plan §4)
    //   } else {
    //       applyIndexedFallback();   // GetColorTheme(idx) for ~5 slots
    //   }
}

uint32_t ThemeBridge::probeHash()
{
    // Phase 2.6a:
    //   int sz = 0;
    //   const uint8_t* p = static_cast<const uint8_t*>(GetColorThemeStruct(&sz));
    //   if (!p || sz <= 0) return 0;
    //   const int n = sz < 1024 ? sz : 1024;
    //   uint32_t h = 2166136261u;
    //   for (int i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    //   return h;
    return 0;
}

void ThemeBridge::tick()
{
    const uint32_t h = probeHash();
    if (h != s_lastHash) {
        s_lastHash = h;
        reapply();
    }
}

} // namespace uf8
