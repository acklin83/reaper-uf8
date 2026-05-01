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
    ImGui_Context* ctx = nullptr;
    int            selected = kSecMixer;

    void create()
    {
        if (ctx) return;
        // v0.10+ ImGui_CreateContext takes optional int* for all four
        // dimension args — passing raw ints (1280, 720) crashed because
        // the dylib's trampoline dereferenced them as pointers (= addr
        // 0x500). Must pass &int or nullptr. Vendored header patched in
        // tandem; see learnings.md rule 17.
        int sizeW = 1280;
        int sizeH = 720;
        ctx = ImGui_CreateContext(
            "Rea-Sixty",
            &sizeW, &sizeH,
            /*pos_x*/ nullptr, /*pos_y*/ nullptr);
    }

    void destroy()
    {
        if (!ctx) return;
        ImGui_DestroyContext(ctx);
        ctx = nullptr;
    }
};

MixerWindow::MixerWindow()  : impl_(new Impl) {}
MixerWindow::~MixerWindow() { if (impl_) impl_->destroy(); delete impl_; }

void MixerWindow::toggle()
{
    if (impl_->ctx) impl_->destroy();
    else            impl_->create();
}

bool MixerWindow::isOpen() const { return impl_->ctx != nullptr; }

void MixerWindow::onRunTick()
{
    if (!impl_->ctx) return;

    const int pushed = ThemeBridge::pushAll(impl_->ctx);

    bool open = true;
    if (ImGui_Begin(impl_->ctx, "Rea-Sixty", &open, /*flags*/ nullptr)) {

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

    // User clicked the close button → tear the context down on the
    // following tick (defer until *after* End to keep the API contract
    // clean).
    if (!open) impl_->destroy();
}

} // namespace uf8
