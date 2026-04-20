//
// REAPER extension entry point. Glues ColorSync + UF8Device to REAPER's
// API. The visible-track resolution is the one piece that's REAPER-specific
// and deserves comment:
//
//   REAPER exposes the mixer view via TCP/MCP scroll state. Our extension
//   polls on a timer (33 ms) and resolves the 8 "currently visible" tracks
//   in the TCP order. For the first milestone we simply use CountTracks()
//   and show tracks 1..8 regardless of scroll — bank-shift hookup is a
//   follow-up once we confirm the pipe actually drives the hardware.
//

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include <cstdio>
#include <memory>

#include "ColorSync.h"
#include "MidiBridge.h"
#include "UF8Device.h"

namespace {

std::unique_ptr<uf8::UF8Device>   g_dev;
std::unique_ptr<uf8::ColorSync>   g_sync;
std::unique_ptr<uf8::MidiBridge>  g_midi;

// Very simple rolling log file so we can see what CSI sends us without
// needing REAPER's console plumbing. Written from the Core-MIDI thread —
// kept fopen/fprintf/fclose for simplicity; REAPER's MIDI rate is low.
void logMidi(std::span<const uint8_t> bytes)
{
    FILE* f = std::fopen("/tmp/reaper_uf8_midi.log", "a");
    if (!f) return;
    for (auto b : bytes) std::fprintf(f, "%02x ", b);
    std::fprintf(f, "\n");
    std::fclose(f);
}

uint32_t reaperColorForVisibleSlot(int slot)
{
    const int trackCount = CountTracks(nullptr);
    if (slot >= trackCount) return 0;
    MediaTrack* tr = GetTrack(nullptr, slot);
    if (!tr) return 0;
    // REAPER returns native color as int. Bit 0x1000000 is "color set";
    // low 24 bits are 0xBBGGRR on Windows, 0xRRGGBB on mac/Linux via the
    // "native" encoding. GetTrackColor wraps that — low 24 bits are what
    // we want, matching quantize()'s 0xRRGGBB expectation.
    const int c = GetTrackColor(tr);
    return static_cast<uint32_t>(c) & 0x00FFFFFFu;
}

void onTimer()
{
    if (g_sync) g_sync->refresh(reaperColorForVisibleSlot);
}

} // anonymous

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    if (!rec) {
        // Unload
        plugin_register("-timer", reinterpret_cast<void*>(onTimer));
        g_sync.reset();
        if (g_midi) g_midi->close();
        g_midi.reset();
        if (g_dev) g_dev->close();
        g_dev.reset();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

    // Open the virtual MIDI ports first — REAPER's control-surface layer
    // picks these up during startup, so they need to exist before the
    // user configures CSI to route to them.
    g_midi = std::make_unique<uf8::MidiBridge>();
    if (g_midi->open("reaper_uf8")) {
        g_midi->setIncomingHandler(logMidi);
    } else {
        ShowConsoleMsg("reaper_uf8: failed to open virtual MIDI ports\n");
    }

    g_dev = std::make_unique<uf8::UF8Device>();
    if (!g_dev->open()) {
        // Can't open UF8 — extension stays loaded so the user sees the
        // error via ReaScript / console, but does nothing.
        ShowConsoleMsg(("reaper_uf8: " + g_dev->lastError() + "\n").c_str());
        return 1;
    }
    g_sync = std::make_unique<uf8::ColorSync>(*g_dev);
    g_sync->invalidate();

    // Poll at ~30 Hz. REAPER's timer callback fires on the main thread.
    plugin_register("timer", reinterpret_cast<void*>(onTimer));
    return 1;
}
