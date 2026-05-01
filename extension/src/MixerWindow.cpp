#include "MixerWindow.h"
#include "MixerLayout.h"
#include "SettingsScreen.h"
#include "ThemeBridge.h"

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
    impl_->visible = !impl_->visible;
}

bool MixerWindow::isOpen() const { return impl_->visible; }

void MixerWindow::onRunTick()
{
    if (!impl_->visible) return;
    impl_->ensureCtx();
    if (!impl_->ctx) return;  // CreateContext failed (ReaImGui not installed?)

    // Force a sane size + on-screen position the very first time the
    // user opens the window in this session, then let user-driven
    // moves / resizes stick afterwards. FirstUseEver means subsequent
    // toggles don't snap back to defaults.
    {
        int condFirst = ImGui_Cond_FirstUseEver;
        ImGui_SetNextWindowSize(impl_->ctx, /*w*/ 1100, /*h*/ 760,
                                &condFirst);
        ImGui_SetNextWindowPos(impl_->ctx, /*x*/ 80, /*y*/ 80,
                               &condFirst, /*pivot_x*/ nullptr,
                               /*pivot_y*/ nullptr);
    }

    const int pushed = ThemeBridge::pushAll(impl_->ctx);

    // Window display title hides the version suffix via "##" so the
    // user just reads "Rea-Sixty"; the suffix is the unique-id portion
    // ImGui uses internally and matches the renamed context — both
    // change in lock-step so old persisted state is fully bypassed.
    bool open = true;
    if (ImGui_Begin(impl_->ctx, "Rea-Sixty##v2", &open, /*flags*/ nullptr)) {

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

    // X-click sets *p_open=false during Begin. Mirror to our flag so
    // next tick early-returns and the OS window stays hidden.
    if (!open) impl_->visible = false;
}

} // namespace uf8
