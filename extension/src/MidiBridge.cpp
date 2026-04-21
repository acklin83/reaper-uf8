#include "MidiBridge.h"

#include <cctype>
#include <cstdio>
#include <string>

#if __APPLE__

namespace uf8 {

bool MidiBridge::open(const char* clientName)
{
    if (client_) return true;

    CFStringRef name = CFStringCreateWithCString(
        nullptr, clientName, kCFStringEncodingUTF8);

    OSStatus s = MIDIClientCreate(name, nullptr, nullptr, &client_);
    if (s != noErr) { CFRelease(name); return false; }

    CFStringRef destName = CFStringCreateWithCString(
        nullptr, "reaper_uf8 in", kCFStringEncodingUTF8);
    s = MIDIDestinationCreate(client_, destName,
                              &MidiBridge::readProc_, this, &dest_);
    CFRelease(destName);
    if (s != noErr) {
        MIDIClientDispose(client_);
        client_ = 0;
        CFRelease(name);
        return false;
    }

    CFStringRef srcName = CFStringCreateWithCString(
        nullptr, "reaper_uf8 out", kCFStringEncodingUTF8);
    s = MIDISourceCreate(client_, srcName, &source_);
    CFRelease(srcName);
    if (s != noErr) {
        MIDIEndpointDispose(dest_);
        MIDIClientDispose(client_);
        client_ = 0; dest_ = 0;
        CFRelease(name);
        return false;
    }

    CFRelease(name);
    return true;
}

void MidiBridge::close()
{
    if (uf8OutPort_) { MIDIPortDispose(uf8OutPort_); uf8OutPort_ = 0; }
    uf8OutDest_ = 0;
    if (source_) { MIDIEndpointDispose(source_); source_ = 0; }
    if (dest_)   { MIDIEndpointDispose(dest_);   dest_   = 0; }
    if (client_) { MIDIClientDispose(client_);   client_ = 0; }
}

bool MidiBridge::openUf8Output()
{
    if (!client_) return false;
    if (uf8OutDest_) return true;

    // Walk CoreMIDI destinations, match by name. "SSL V-MIDI Port 1" is
    // what SSL 360° installs on Windows; macOS may expose the raw UF8
    // HID-MIDI as "SSL UF8" or similar. Case-insensitive substring match
    // on "UF8" catches both. Log everything we see so the user can
    // diagnose if the match fails.
    ItemCount n = MIDIGetNumberOfDestinations();
    MIDIEndpointRef match = 0;
    FILE* log = std::fopen("/tmp/reaper_uf8_midi_dests.log", "w");
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        CFStringRef name = nullptr;
        MIDIObjectGetStringProperty(d, kMIDIPropertyDisplayName, &name);
        if (!name) MIDIObjectGetStringProperty(d, kMIDIPropertyName, &name);
        char buf[256] = {0};
        if (name) {
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
        }
        if (log) std::fprintf(log, "[%lu] %s\n", (unsigned long)i, buf);

        if (!match) {
            std::string s(buf);
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Skip our own virtual port so we don't loop back.
            if (s.find("REAPER_UF8") != std::string::npos) continue;
            if (s.find("UF8") != std::string::npos) match = d;
        }
    }
    if (log) std::fclose(log);

    if (!match) return false;

    CFStringRef portName = CFStringCreateWithCString(
        nullptr, "reaper_uf8 -> UF8", kCFStringEncodingUTF8);
    OSStatus s = MIDIOutputPortCreate(client_, portName, &uf8OutPort_);
    CFRelease(portName);
    if (s != noErr) return false;
    uf8OutDest_ = match;
    return true;
}

bool MidiBridge::uf8OutputOpen() const { return uf8OutDest_ != 0; }

void MidiBridge::sendToUf8(std::span<const uint8_t> bytes)
{
    if (!uf8OutPort_ || !uf8OutDest_ || bytes.empty()) return;
    Byte buffer[64];
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(buffer);
    MIDIPacket* pkt = MIDIPacketListInit(list);
    pkt = MIDIPacketListAdd(list, sizeof(buffer), pkt, 0,
                            bytes.size(), bytes.data());
    if (!pkt) return;
    MIDISend(uf8OutPort_, uf8OutDest_, list);
}

bool MidiBridge::isOpen() const { return client_ != 0; }

MidiBridge::~MidiBridge() { close(); }

void MidiBridge::send(std::span<const uint8_t> bytes)
{
    if (!source_ || bytes.empty()) return;

    // Debug: log every outgoing MIDI we push up to REAPER.
    if (FILE* f = std::fopen("/tmp/reaper_uf8_midi_out.log", "a")) {
        for (auto b : bytes) std::fprintf(f, "%02x ", b);
        std::fprintf(f, "\n");
        std::fclose(f);
    }

    // MIDIPacketList with one packet. 256 B enough for all MCU frames except
    // large SysEx scribble updates — those we'd split, but for now keep it
    // simple.
    Byte buffer[256 + 64];
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(buffer);
    MIDIPacket* pkt = MIDIPacketListInit(list);
    pkt = MIDIPacketListAdd(list, sizeof(buffer), pkt, 0,
                            bytes.size(), bytes.data());
    if (!pkt) return;  // buffer too small

    MIDIReceived(source_, list);
}

void MidiBridge::readProc_(const MIDIPacketList* pktList, void* refCon,
                           void* /*connRefCon*/)
{
    auto* self = static_cast<MidiBridge*>(refCon);
    if (!self->handler_) return;

    const MIDIPacket* pkt = &pktList->packet[0];
    for (UInt32 i = 0; i < pktList->numPackets; ++i) {
        self->handler_(std::span<const uint8_t>(pkt->data, pkt->length));
        pkt = MIDIPacketNext(pkt);
    }
}

} // namespace uf8

#else // non-macOS stubs

namespace uf8 {
bool MidiBridge::open(const char*) { return false; }
void MidiBridge::close() {}
bool MidiBridge::isOpen() const { return false; }
MidiBridge::~MidiBridge() = default;
void MidiBridge::send(std::span<const uint8_t>) {}
bool MidiBridge::openUf8Output() { return false; }
bool MidiBridge::uf8OutputOpen() const { return false; }
void MidiBridge::sendToUf8(std::span<const uint8_t>) {}
} // namespace uf8

#endif
