#pragma once
//
// ThemeBridge — pulls REAPER's current color theme and pushes it as ReaImGui
// style colors at frame entry. Adopts whatever theme the user has loaded
// (Reapertips, Default, custom) at runtime. No SSL trademark colors hardcoded.
//
// API model matches ReaImGui's Push/Pop convention: pushAll() at the start of
// each frame, popAll() at the end. Cheap (~10 calls per frame).
//
// Sources for theme data:
//   GetColorTheme(int idx)        — ~20 indexed colors (deprecated but stable),
//                                   indices in reaper_plugin.h:1327+
//   GetColorThemeStruct(int* sz)  — full struct, layout from icontheme.h
//                                   (not yet vendored — Phase 2.6a follow-up)
//
// Today this implementation pulls from the indexed API only. The structured
// API is a drop-in upgrade once icontheme.h is vendored.
//

class ImGui_Context; // ReaImGui opaque context pointer

namespace uf8 {

class ThemeBridge {
public:
    // Push REAPER-derived style colors onto the ImGui style stack.
    // Returns the number of colors pushed — pass back to popAll().
    static int pushAll(ImGui_Context* ctx);

    // Pop the colors pushed by the matching pushAll() call.
    static void popAll(ImGui_Context* ctx, int count);
};

} // namespace uf8
