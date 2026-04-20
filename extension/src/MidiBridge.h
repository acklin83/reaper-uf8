#pragma once
//
// MidiBridge — virtual MIDI port for REAPER / CSI.
//
// Hosts two virtual endpoints using Core MIDI (macOS) that together look
// like a bi-directional MIDI device to REAPER:
//   "reaper_uf8 in"   (MIDIDestination) — REAPER/CSI sends MCU frames here
//   "reaper_uf8 out"  (MIDISource)      — we send MCU frames up to REAPER
//
// On receive, we parse the MCU frame (scribble-strip SysEx, meter pressure,
// LED note-on, fader pitch-bend, v-pot CC, bank buttons) and translate to
// UF8 vendor-USB commands via the associated callback. On UF8 button-event
// input we build the matching MCU frame and push it out to REAPER.
//
// Windows/Linux ports will wrap teVirtualMIDI / ALSA behind the same API.
//

#include <cstdint>
#include <functional>
#include <span>

#if __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#endif

namespace uf8 {

class MidiBridge {
public:
    // Called on the Core-MIDI thread when REAPER sends us a packet.
    // Keep the callback short; no REAPER API from here.
    using IncomingHandler = std::function<void(std::span<const uint8_t>)>;

    MidiBridge() = default;
    ~MidiBridge();

    MidiBridge(const MidiBridge&) = delete;
    MidiBridge& operator=(const MidiBridge&) = delete;

    bool open(const char* clientName = "reaper_uf8");
    void close();
    bool isOpen() const;

    // Send a MIDI message upstream (UF8 → REAPER direction).
    void send(std::span<const uint8_t> bytes);

    void setIncomingHandler(IncomingHandler h) { handler_ = std::move(h); }

private:
#if __APPLE__
    static void readProc_(const MIDIPacketList* pktList, void* refCon, void* connRefCon);

    MIDIClientRef    client_ = 0;
    MIDIEndpointRef  source_ = 0;
    MIDIEndpointRef  dest_   = 0;
#endif
    IncomingHandler  handler_;
};

} // namespace uf8
