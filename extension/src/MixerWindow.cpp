#include "MixerWindow.h"

namespace uf8 {

struct MixerWindow::Impl {
    bool open = false;
};

MixerWindow::MixerWindow()  : impl_(new Impl) {}
MixerWindow::~MixerWindow() { delete impl_; }

void MixerWindow::toggle()
{
    impl_->open = !impl_->open;
    // Phase 2.6a: create/destroy the SWELL window + ImGui context here.
    //   - SWELL_CreateDialog or raw NSWindow/NSOpenGLView via swell-cocoa hook
    //   - DockWindowAddEx(hwnd, "Rea-Sixty Mixer", "rea_sixty_mixer", true)
    //   - ImGui::CreateContext(); ImGui_ImplOpenGL3_Init(...)
}

bool MixerWindow::isOpen() const { return impl_->open; }

void MixerWindow::onRunTick()
{
    if (!impl_->open) return;
    // Phase 2.6a:
    //   ThemeBridge::tick();          // re-pull on theme-hash change
    //   ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplOSPlatform_NewFrame();
    //   ImGui::NewFrame();
    //   MixerLayout::draw();
    //   ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(...);
}

} // namespace uf8
