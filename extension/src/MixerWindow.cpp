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

struct MixerWindow::Impl {
    ImGui_Context* ctx = nullptr;

    void create()
    {
        if (ctx) return;
        // Title is also the OS window title. v0.1.1 ImGui_CreateContext
        // creates the context and an OS-level window in one call.
        ctx = ImGui_CreateContext(
            "Rea-Sixty",
            /*size_w*/ 1280, /*size_h*/ 720,
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
        // Two-tab layout: Mixer ⇄ Settings. ReaImGui's TabBar persists the
        // active tab automatically across frames, so no view-state field
        // is needed in our struct.
        if (ImGui_BeginTabBar(impl_->ctx, "main", /*flags*/ nullptr)) {
            if (ImGui_BeginTabItem(impl_->ctx, "Mixer", /*p_open*/ nullptr, /*flags*/ nullptr)) {
                MixerLayout::draw(impl_->ctx);
                ImGui_EndTabItem(impl_->ctx);
            }
            if (ImGui_BeginTabItem(impl_->ctx, "Settings", /*p_open*/ nullptr, /*flags*/ nullptr)) {
                SettingsScreen::draw(impl_->ctx);
                ImGui_EndTabItem(impl_->ctx);
            }
            ImGui_EndTabBar(impl_->ctx);
        }
    }
    ImGui_End(impl_->ctx);

    ThemeBridge::popAll(impl_->ctx, pushed);

    // User clicked the close button → tear the context down on the
    // following tick (defer until *after* End to keep the API contract
    // clean).
    if (!open) impl_->destroy();
}

} // namespace uf8
