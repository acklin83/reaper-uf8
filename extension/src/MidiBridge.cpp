#include "MidiBridge.h"

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
    if (source_) { MIDIEndpointDispose(source_); source_ = 0; }
    if (dest_)   { MIDIEndpointDispose(dest_);   dest_   = 0; }
    if (client_) { MIDIClientDispose(client_);   client_ = 0; }
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
} // namespace uf8

#endif
