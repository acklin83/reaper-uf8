// Smoke test: parse the user's real CSI Home.zon and report stats.
// Built standalone (no REAPER, no libusb) — enough to verify the parser
// + translator land sensibly. Pass the surface dir as argv[1], or rely
// on the default macOS path.
//
// We stub out the bindings:: setBinding/clearBinding/etc. so the
// translator can call them without pulling in the full Bindings.cpp +
// REAPER API surface. CsiImport.cpp itself only uses GetResourcePath
// from REAPER, which we stub to nullptr below.

#include "CsiImport.h"
#include "Bindings.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// ---- REAPER API stubs ----
// CsiImport.cpp references GetResourcePath only. Definition must match
// the function-pointer typedef from reaper_plugin_functions.h, but we
// don't include that header in test land — declare a matching symbol.
extern "C" const char* (*GetResourcePath)() = nullptr;
extern "C" int (*NamedCommandLookup)(const char*) = nullptr;

// ---- Bindings stubs ----
// CsiImport calls setBinding / clearBinding when applying. The smoke
// test only runs preview() so these never fire — but the symbols still
// need to exist for the linker.
namespace uf8::bindings {
const char* toName(ButtonId) { return ""; }
void setBinding(int, ButtonId, const Binding&) {}
void clearBinding(int, ButtonId) {}
} // namespace uf8::bindings

int main(int argc, char** argv)
{
    std::string dir = (argc > 1) ? argv[1]
        : "/Users/stoersender/Library/Application Support/REAPER/CSI/Surfaces/SSLUF8";

    std::printf("CSI surface dir: %s\n\n", dir.c_str());
    auto r = uf8::csi_import::preview(dir);
    if (!r.loaded) {
        std::fprintf(stderr, "FAILED: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("Source : %s\n", r.zonePath.c_str());
    std::printf("Mapped : %d\n", r.appliedCount);
    std::printf("Skipped: %d\n", r.skippedCount);
    std::printf("Warning: %d\n\n", r.warningCount);

    for (const auto& e : r.entries) {
        const char* tag = e.applied ? (e.warning ? "WARN" : " OK ") : "skip";
        std::printf("[%s] %-14s | %s\n",
                    tag, e.widget.c_str(), e.mappedTo.c_str());
    }
    return 0;
}
