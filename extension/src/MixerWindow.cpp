#include "MixerWindow.h"
#include "MixerLayout.h"
#include "SettingsScreen.h"
#include "ThemeBridge.h"

#include <cstdio>

// One translation unit must define REAIMGUIAPI_IMPLEMENT before including the
// header — this materialises the storage for the lazy-resolved ReaImGuiFunc
// instances. Every other TU just `#include "reaper_imgui_functions.h"` and
// uses them as if they were free functions.
#define REAIMGUIAPI_IMPLEMENT
#include "reaper_imgui_functions.h"

namespace uf8 {

namespace {

// Left-rail navigation. Mixer sits at the top; settings sections follow,
// separated by a divider. Order chosen to put the high-frequency view
// (Mixer) first and the static info (About) last.
enum Section : int {
    kSecMixer = 0,
    kSecDevice,
    kSecBindings,
    kSecSoftKeyBanks,
    kSecModes,
    kSecSelectionSets,
    kSecAbout,
    kSecCount,
};

struct RailEntry {
    const char* label;
    Section     section;
    bool        separatorBefore;
    void (*draw)(ImGui_Context*);
};

constexpr RailEntry kRail[] = {
    { "Mixer",          kSecMixer,         false, &MixerLayout::draw                 },
    { "Device",         kSecDevice,        true,  &SettingsScreen::drawDevice        },
    { "Bindings",       kSecBindings,      false, &SettingsScreen::drawBindings      },
    { "Soft-Key Banks", kSecSoftKeyBanks,  false, &SettingsScreen::drawSoftKeyBanks  },
    { "Modes",          kSecModes,         false, &SettingsScreen::drawModes         },
    { "Selection Sets", kSecSelectionSets, false, &SettingsScreen::drawSelectionSets },
    { "About",          kSecAbout,         true,  &SettingsScreen::drawAbout         },
};

constexpr double kRailWidthPx = 160.0;

} // namespace

struct MixerWindow::Impl {
    // v0.10+ owns context lifetime itself: ImGui_CreateContext returns a
    // context that auto-destroys when the calling extension unloads. There
    // is no ImGui_DestroyContext export in v0.10 — calling the vendored
    // binding for it crashes with PC=0 (plugin_getapi returns null for the
    // missing symbol). So we never destroy. Toggle instead flips a
    // visibility flag; when invisible, we skip Begin/End entirely and
    // ReaImGui closes the OS window. Re-toggling resumes drawing.
    ImGui_Context* ctx = nullptr;
    bool           visible = false;
    int            selected = kSecMixer;
    // Session counter — bumped on every closed→open transition. Used to
    // suffix the Begin window-id so each session is a fresh ImGui
    // window object. Required because ReaImGui v0.10 retains stale
    // window state (collapsed/closed/off-screen pose) under the old
    // id and refuses to re-show after a single open/close cycle. New
    // id = no carried-over state.
    int            sessionGen = 0;

    void ensureCtx()
    {
        if (ctx) return;
        // v0.10+ ImGui_CreateContext takes optional int* for all four
        // dimension args — passing raw ints (1280, 720) crashed because
        // the dylib's trampoline dereferenced them as pointers (= addr
        // 0x500). Must pass &int or nullptr. See learnings.md rule 17.
        //
        // Context name carries a version suffix so a fresh ReaImGui
        // state file is allocated. Stale persisted state under the bare
        // "Rea-Sixty" key prevented the window from reopening across
        // recent debugging sessions — bumping the suffix forces v0.10
        // to treat us as a brand-new context with no carried-over
        // collapsed / off-screen / closed pose.
        int sizeW = 1280;
        int sizeH = 720;
        ctx = ImGui_CreateContext(
            "Rea-Sixty v2",
            &sizeW, &sizeH,
            /*pos_x*/ nullptr, /*pos_y*/ nullptr);
    }
};

MixerWindow::MixerWindow()  : impl_(new Impl) {}
MixerWindow::~MixerWindow() { delete impl_; }

void MixerWindow::toggle()
{
    const bool wasOpen = impl_->visible;
    impl_->visible = !wasOpen;
    if (impl_->visible) ++impl_->sessionGen;
}

bool MixerWindow::isOpen() const { return impl_->visible; }

void MixerWindow::onRunTick()
{
    impl_->ensureCtx();
    if (!impl_->ctx) return;  // CreateContext failed (ReaImGui not installed?)

    // ReaImGui v0.10 garbage-collects unused objects between defer
    // cycles ("valid as long as it is used in each defer cycle unless
    // attached to a context" — verified via the dylib's embedded docs).
    // Skipping Begin/End on closed frames had the GC reap our context,
    // so the next open silently rendered nothing. We now ALWAYS call
    // Begin/End to keep the context "in use"; *p_open toggles whether
    // the OS window is shown. End must follow Begin regardless.
    //
    // Pin a sane size + position on the very first frame the window
    // becomes visible (Cond_Appearing reapplies on every closed→open),
    // so a stuck-off-screen pose can't trap us.
    int condAppearing = ImGui_Cond_Appearing;
    ImGui_SetNextWindowSize(impl_->ctx, /*w*/ 1100, /*h*/ 760,
                            &condAppearing);
    ImGui_SetNextWindowPos(impl_->ctx, /*x*/ 80, /*y*/ 80,
                           &condAppearing, /*pivot_x*/ nullptr,
                           /*pivot_y*/ nullptr);

    const int pushed = ThemeBridge::pushAll(impl_->ctx);

    // Window display title is just "Rea-Sixty"; the "##session_N"
    // suffix is the ImGui id-only tail and bumps every closed→open so
    // each session is a brand-new ImGui window. Together with always-
    // Begin this gives a clean re-open path even after the user closes
    // via the title bar X.
    char winId[64];
    std::snprintf(winId, sizeof(winId),
                  "Rea-Sixty##session_%d", impl_->sessionGen);
    bool open = impl_->visible;
    if (ImGui_Begin(impl_->ctx, winId, &open, /*flags*/ nullptr)
        && impl_->visible) {

        // -- Left rail: section list -------------------------------------
        double railW = kRailWidthPx;
        if (ImGui_BeginChild(impl_->ctx, "rail", &railW, /*size_h*/ nullptr,
                             /*border*/ nullptr, /*flags*/ nullptr)) {
            for (const RailEntry& e : kRail) {
                if (e.separatorBefore) ImGui_Separator(impl_->ctx);
                bool isSelected = (impl_->selected == e.section);
                if (ImGui_Selectable(impl_->ctx, e.label, &isSelected,
                                     /*flags*/ nullptr,
                                     /*size_w*/ nullptr, /*size_h*/ nullptr)) {
                    impl_->selected = e.section;
                }
            }
        }
        ImGui_EndChild(impl_->ctx);

        ImGui_SameLine(impl_->ctx, /*offset_from_start_x*/ nullptr,
                       /*spacing*/ nullptr);

        // -- Right content pane ------------------------------------------
        if (ImGui_BeginChild(impl_->ctx, "content", /*size_w*/ nullptr,
                             /*size_h*/ nullptr, /*border*/ nullptr,
                             /*flags*/ nullptr)) {
            for (const RailEntry& e : kRail) {
                if (e.section == impl_->selected) { e.draw(impl_->ctx); break; }
            }
        }
        ImGui_EndChild(impl_->ctx);
    }
    ImGui_End(impl_->ctx);

    ThemeBridge::popAll(impl_->ctx, pushed);

    // X-click in the title bar sets *p_open=false during Begin. Mirror
    // back to our visibility flag so the next 360-toggle correctly
    // moves false→true.
    impl_->visible = open;
}

} // namespace uf8
