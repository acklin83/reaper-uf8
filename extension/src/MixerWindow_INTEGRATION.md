# Mixer Window — main.cpp integration notes

The MixerWindow is fully implemented and compiles, but main.cpp has not been
touched yet on this branch. The integration is the last 15-line step;
deferred so it doesn't conflict with in-flight UF8 V-Pot work on `main`.

## What needs to land in main.cpp

```cpp
#include "MixerWindow.h"

namespace { uf8::MixerWindow g_mixerWindow; }

// In REAPER_PLUGIN_ENTRYPOINT, after REAPERAPI_LoadAPI() succeeds:
{
  static custom_action_register_t act = {
    /* section */ 0,
    /* id_str  */ "REA_SIXTY_TOGGLE_MIXER",
    /* name    */ "Rea-Sixty: Toggle Plugin Mixer Window",
    /* extra   */ nullptr,
  };
  static int s_mixerCmd = 0;
  s_mixerCmd = rec->Register("custom_action", &act);

  // Hookcommand catches custom_action invocations.
  static auto hook = +[](int command, int /*flag*/) -> bool {
    if (command == s_mixerCmd) { g_mixerWindow.toggle(); return true; }
    return false;
  };
  rec->Register("hookcommand", reinterpret_cast<void*>(hook));
}

// In your existing IReaperControlSurface::Run() implementation, append:
g_mixerWindow.onRunTick();
```

That's the whole wire-in.

## Required runtime dependency

User must have the **ReaImGui** extension installed via ReaPack
(ReaTeam Extensions repo). Without it, all ImGui_* function pointers
resolve to nullptr and the first call into the context crashes.

Add a runtime probe before creating the context:

```cpp
if (!plugin_getapi("ImGui_CreateContext")) {
    ShowMessageBox(
        "Install ReaImGui via ReaPack to enable the Plugin Mixer.",
        "Rea-Sixty", 0);
    return;
}
```

## Vendored header version

`vendor/reaimgui/reaper_imgui_functions.h` is generated for ReaImGui v0.1.1
(with a local `<utility>` include patch). v0.1.x has the API surface we need
for Phase 2.6a/b. Regenerate from cfillion's source (`tools/genbinding.cpp`)
once we want newer features (table API, plotting, font scaling).

## Open follow-ups (Phase 2.6a tail end)

- Vendor `icontheme.h` from upstream REAPER SDK to upgrade ThemeBridge from
  the indexed `GetColorTheme` API to the structured `GetColorThemeStruct`,
  unlocking MCP/mixer-specific theme slots.
- Pin a known-good ReaImGui version in user-facing docs so we don't ship
  against a moving target.
