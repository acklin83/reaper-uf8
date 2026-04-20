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

// Translate an incoming MCU MIDI packet to UF8 vendor-USB commands.
// Currently handles the scribble-strip SysEx only — enough to prove the
// end-to-end pipe. Fuller mapping (meters, LEDs, V-pot, fader) is next.
void onMidiFromReaper(std::span<const uint8_t> bytes)
{
    logMidi(bytes);
    if (!g_dev || !g_dev->isOpen()) return;

    // Scribble-strip SysEx: F0 00 00 66 14 12 <pos> <up to 112 chars> F7
    // <pos> indexes into a 56-char-wide virtual display (7 chars * 8 strips),
    // row 0 at 0..0x37, row 1 at 0x38..0x6F. We only handle row 0 for now.
    if (bytes.size() >= 8
        && bytes[0] == 0xF0 && bytes[1] == 0x00 && bytes[2] == 0x00
        && bytes[3] == 0x66 && bytes[4] == 0x14 && bytes[5] == 0x12)
    {
        const uint8_t pos = bytes[6];
        if (pos >= 0x38) return;  // lower row — skip for now

        const uint8_t strip = pos / 7;
        if (strip >= 8) return;

        // Text bytes stretch from byte 7 until F7 (sysex end) or end of buffer.
        size_t textStart = 7;
        size_t textEnd   = bytes.size();
        if (bytes.back() == 0xF7) --textEnd;

        std::string_view text(
            reinterpret_cast<const char*>(bytes.data() + textStart),
            textEnd - textStart);

        // The SysEx may carry all 8 strips in one packet (starting at 0x00).
        // Walk 7 chars at a time, dispatching one UF8 command per strip.
        while (!text.empty() && strip < 8) {
            uint8_t s = strip + static_cast<uint8_t>((text.data() - reinterpret_cast<const char*>(bytes.data() + textStart)) / 7);
            if (s >= 8) break;
            auto chunk = text.substr(0, 7);
            g_dev->send(uf8::buildStripTextUpper(s, chunk));
            if (text.size() <= 7) break;
            text.remove_prefix(7);
        }
    }
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
        g_midi->setIncomingHandler(onMidiFromReaper);
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
