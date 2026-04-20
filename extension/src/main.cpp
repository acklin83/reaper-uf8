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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "ColorSync.h"
#include "HidDevice.h"
#include "MidiBridge.h"
#include "Protocol.h"
#include "UF8Device.h"

namespace {

std::unique_ptr<uf8::UF8Device>   g_dev;
std::unique_ptr<uf8::ColorSync>   g_sync;
std::unique_ptr<uf8::MidiBridge>  g_midi;
std::unique_ptr<uf8::HidDevice>   g_hid;

// Parse a UF8 vendor-USB IN report and, when it's an input event, convert
// to the equivalent MCU MIDI message and push it back up to REAPER via the
// virtual MIDI source. The UF8 firmware packs events into this EP 0x81 IN
// stream, inter-mingled with its continuous polling heartbeat (which we
// just skip).
//
// Observed event framing — all starting from a position past the 31 60
// session prefix (stripped here if present):
//
//   FF 21 03 <strip> <A> <B> CKSUM    — fader position (16-bit abs.)
//   FF 22 03 <id>    00 <s> CKSUM     — button press/release
//   FF 23 02 <strip> <s>    CKSUM     — fader touch
//   FF 24 02 <strip> <d>    CKSUM     — v-pot rotation (delta)
//   FF 33 02 <strip> <s>    CKSUM     — v-pot push toggle (tbc)
//
// Plus the "FF 04 02 9d 01 a4" and partner "FF 04 02 94 01 9b" polling
// packets which repeat at ~100 Hz — we ignore those.
// Per-strip touch state with debounce. The UF8 touch sensor bounces
// during a single sustained touch (~9 press+release pairs per second
// observed in captures). Emitting every one of those as MCU note-on/off
// would make REAPER's automation engine stutter (e.g. Touch mode would
// release-then-reacquire constantly). Instead:
//   - On any touch-press IN event: emit MCU Note-on immediately (if not
//     already "reported as touched"). Update last-press timestamp.
//   - On touch-release IN event: do NOTHING immediately.
//   - The REAPER timer (30 Hz) emits Note-off when (now - lastPress)
//     exceeds the hold threshold, which means the user really let go.
std::array<std::atomic<bool>, 8>          g_touchReported{};
std::array<std::chrono::steady_clock::time_point, 8> g_touchLastPress{};
constexpr auto kTouchHoldThreshold = std::chrono::milliseconds(300);

// De-dup for pitch-bend so REAPER's motor echo doesn't re-trigger us.
std::array<std::atomic<uint8_t>, 8> g_lastMsbOut{};
std::array<std::atomic<uint8_t>, 8> g_lastLsbOut{};

void onUf8Input(const uint8_t* data, size_t len)
{
    // Debug: log every non-trivial IN packet that reaches this handler,
    // including payloads that might be interesting (anything not a pure
    // 31 60 / poll pair).
    if (FILE* f = std::fopen("/tmp/reaper_uf8_in_dispatch.log", "a")) {
        std::fprintf(f, "[%zu] ", len);
        for (size_t k = 0; k < len && k < 32; ++k) std::fprintf(f, "%02x ", data[k]);
        std::fprintf(f, "\n");
        std::fclose(f);
    }

    if (!g_midi) return;

    // Walk past each FF frame in the buffer (multiple frames can arrive
    // concatenated, optionally preceded by a 31 60 / 31 00 session prefix).
    size_t i = 0;
    while (i < len) {
        // Session-prefix byte 0x31 + flag byte: skip both.
        if (data[i] == 0x31 && i + 1 < len) { i += 2; continue; }
        if (data[i] != 0xFF) { ++i; continue; }
        if (i + 2 >= len) break;  // need at least FF, cmd, something

        // Figure out frame size based on the command byte.
        const uint8_t cmd = data[i + 1];
        size_t frameSize = 0;
        switch (cmd) {
            case 0x04: frameSize = 7; break;   // poll (02 9d 01 a4) — ignore
            case 0x21: frameSize = 7; break;   // fader position
            case 0x22: frameSize = 7; break;   // button
            case 0x20: frameSize = 6; break;   // fader touch (capacitive)
            case 0x23: frameSize = 6; break;   // pressure sensor (TBD)
            case 0x24: frameSize = 6; break;   // v-pot rotation
            case 0x33: frameSize = 6; break;   // pressure sensor (TBD)
            default:   frameSize = 0; break;
        }
        if (frameSize == 0 || i + frameSize > len) { ++i; continue; }

        // Dispatch by command.
        if (cmd == 0x21 && data[i + 2] == 0x03) {
            // Fader position: FF 21 03 strip A B cksum
            // A = MCU LSB (high bit is a flag, masked), B = MCU MSB.
            // Only forward while the user is currently holding the fader
            // (debounced touch state) AND the value actually changed —
            // otherwise REAPER's motor-echo feedback loop would tank the
            // fader down.
            const uint8_t strip = data[i + 3];
            if (strip < 8 && g_touchReported[strip].load()) {
                const uint8_t lsb = data[i + 4] & 0x7F;
                const uint8_t msb = data[i + 5] & 0x7F;
                const uint8_t prevMsb = g_lastMsbOut[strip].load();
                const uint8_t prevLsb = g_lastLsbOut[strip].load();
                const int lsbDelta = std::abs(int(lsb) - int(prevLsb));
                if (msb != prevMsb || lsbDelta >= 4) {
                    g_lastMsbOut[strip].store(msb);
                    g_lastLsbOut[strip].store(lsb);
                    const uint8_t mcu[3] = {static_cast<uint8_t>(0xE0 | strip), lsb, msb};
                    g_midi->send(std::span<const uint8_t>(mcu, 3));
                }
            }
        } else if (cmd == 0x22 && data[i + 2] == 0x03) {
            // Button: FF 22 03 id 00 state cksum
            const uint8_t id    = data[i + 3];
            const uint8_t state = data[i + 5];
            const uint8_t mcu[3] = {0x90, id, state ? uint8_t{0x7F} : uint8_t{0x00}};
            g_midi->send(std::span<const uint8_t>(mcu, 3));
        } else if (cmd == 0x20 && data[i + 2] == 0x02) {
            // Fader touch: FF 20 02 strip state cksum
            //
            // Clean capacitive touch — hardware-debounced. On press we
            // (1) send MCU Note-on 0x68+strip to REAPER so CSI knows the
            //     fader is user-controlled,
            // (2) send FF 1D 02 strip 00 to the UF8 so its motor releases
            //     and the user's hand isn't fighting it.
            // On release, mirror both.
            const uint8_t strip = data[i + 3];
            const uint8_t state = data[i + 4];
            if (strip < 8) {
                g_touchReported[strip].store(state != 0);
                const uint8_t mcu[3] = {0x90, static_cast<uint8_t>(0x68 + strip),
                                        state ? uint8_t{0x7F} : uint8_t{0x00}};
                g_midi->send(std::span<const uint8_t>(mcu, 3));
                if (g_dev) g_dev->send(uf8::buildMotorEnable(strip, state == 0));
            }
        } else if (cmd == 0x24 && data[i + 2] == 0x02) {
            // V-pot rotation: FF 24 02 strip delta_raw cksum
            // Convert to MCU CC: B0 <0x10+strip> <dir<<6 | speed>
            // UF8 delta: low-nibble = speed; high-nibble presumably direction.
            // Provisional mapping until we capture more data: bit 7 = direction.
            const uint8_t strip = data[i + 3];
            const uint8_t raw   = data[i + 4];
            if (strip < 8) {
                const uint8_t speed = raw & 0x3F;
                const bool dir_down = (raw & 0x40) != 0;
                const uint8_t cc_val = dir_down ? (0x40 | speed) : speed;
                const uint8_t mcu[3] = {0xB0, static_cast<uint8_t>(0x10 + strip), cc_val};
                g_midi->send(std::span<const uint8_t>(mcu, 3));
            }
        }
        // cmd 0x33 (v-pot push?) skipped — need more samples to verify

        i += frameSize;
    }
}

// Log raw HID reports until we've reverse-engineered the report format.
void logHid(const uint8_t* data, size_t len)
{
    if (FILE* f = std::fopen("/tmp/reaper_uf8_hid.log", "a")) {
        for (size_t i = 0; i < len; ++i) std::fprintf(f, "%02x ", data[i]);
        std::fprintf(f, "\n");
        std::fclose(f);
    }
}

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

    // MCU meter forwarding DISABLED — the UF8 meter command layout
    // (FF 38 04 <X> 00 <Y> <Z>) isn't fully decoded yet. Empirically
    // the expected `X = strip * 3` mapping doesn't match the byte
    // values seen in non-fader captures. Correlated audio-playback
    // capture on Windows is the next step — then we can safely
    // translate MCU D0 events without sending garbage to the device.
    (void)bytes;

    // Scribble-strip SysEx: F0 00 00 66 14 12 <pos> <text> F7
    // <pos> indexes into a 56-char-wide virtual display (7 chars * 8 strips),
    // row 0 (upper) at 0..0x37, row 1 (lower) at 0x38..0x6F.
    if (bytes.size() >= 8
        && bytes[0] == 0xF0 && bytes[1] == 0x00 && bytes[2] == 0x00
        && bytes[3] == 0x66 && bytes[4] == 0x14 && bytes[5] == 0x12)
    {
        const uint8_t startPos = bytes[6];

        size_t textStart = 7;
        size_t textEnd   = bytes.size();
        if (bytes.back() == 0xF7) --textEnd;

        // Walk the payload 7 chars at a time, one UF8 command per strip.
        size_t cursor = textStart;
        uint8_t pos = startPos;
        while (cursor < textEnd) {
            const size_t chunkLen = (textEnd - cursor) < 7u ? (textEnd - cursor) : 7u;
            std::string_view chunk(
                reinterpret_cast<const char*>(bytes.data() + cursor), chunkLen);

            // Trim trailing spaces — SSL 360° sends upper-row at natural
            // (unpadded) length. Keeping the text unpadded matches the
            // captured command format.
            auto last = chunk.find_last_not_of(' ');
            std::string_view trimmed = (last == std::string_view::npos)
                                       ? chunk.substr(0, 0) : chunk.substr(0, last + 1);

            if (pos < 0x38) {
                // Upper row (track names)
                const uint8_t strip = pos / 7;
                if (strip < 8) g_dev->send(uf8::buildStripTextUpper(strip, trimmed));
            } else {
                // Lower row (v-pot / value display)
                const uint8_t strip = (pos - 0x38) / 7;
                if (strip < 8) g_dev->send(uf8::buildStripTextLower(strip, chunk));
            }

            cursor += chunkLen;
            pos    += 7;
        }
        return;
    }

    // Fader pitch-bend: E<ch> <LSB> <MSB>  (3 bytes, channel 0..7 = strip)
    if (bytes.size() == 3 && (bytes[0] & 0xF0) == 0xE0) {
        const uint8_t strip = bytes[0] & 0x07;
        if (strip < 8) {
            g_dev->send(uf8::buildFaderPosition(strip, bytes[1], bytes[2]));
        }
        return;
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

// If the track hosts an SSL 360°-enabled plug-in, return the short label
// the UF8 would display in Plug-in Mixer / Channel Strip Mode's "Channel
// Strip Type" zone ("CS 2", "4K B", "4K E", "BusComp"). Empty string
// means "no SSL plug-in on this track" — caller falls back to REAPER-native
// rendering.
//
// Detection is name-based: the VST3 plug-ins SSL ships expose their
// identity via TrackFX_GetFXName, prefixed with "VST3: SSL Native ...".
// We match the substring rather than the exact name to be forgiving of
// version bumps.
std::string sslPluginShortName(MediaTrack* tr)
{
    if (!tr) return {};
    const int fxCount = TrackFX_GetCount(tr);
    char buf[256];
    for (int fx = 0; fx < fxCount; ++fx) {
        if (!TrackFX_GetFXName(tr, fx, buf, sizeof(buf))) continue;
        std::string n(buf);
        // TrackFX_GetFXName returns e.g. "VST3: SSL Native Channel Strip 2 (SSL)"
        if (n.find("Channel Strip 2") != std::string::npos) return "CS 2";
        if (n.find("Channel Strip")   != std::string::npos) return "CS";
        if (n.find("4K B")            != std::string::npos) return "4K B";
        if (n.find("4K E")            != std::string::npos) return "4K E";
        if (n.find("Bus Compressor")  != std::string::npos) return "BusComp";
        if (n.find("360 Link")        != std::string::npos) return "360 Link";
    }
    return {};
}

// Label shown in the "Currently Selected Parameter" zone (`FF 66 <n> 04
// <strip>`) — also the command that keeps the color bar rendered.
//
// Two-mode logic (matches the user's request 2026-04-20):
//   - Track hosts SSL 360° plug-in → show plug-in short name (e.g. "CS 2")
//     so the UF8 displays the plug-in identity the same way SSL 360° would.
//   - Otherwise → show the REAPER track name (truncated to 12 chars, as
//     that's the widest this zone renders cleanly). Empty track name →
//     "CH N" fallback so the slot still reads as populated.
std::string slotLabelForVisibleSlot(int slot)
{
    const int trackCount = CountTracks(nullptr);
    if (slot >= trackCount) {
        char fallback[8];
        std::snprintf(fallback, sizeof(fallback), "CH %d", slot + 1);
        return fallback;
    }
    MediaTrack* tr = GetTrack(nullptr, slot);
    if (!tr) return "";

    if (auto ssl = sslPluginShortName(tr); !ssl.empty()) return ssl;

    // REAPER track name via P_NAME config parm.
    char name[256] = {0};
    if (GetSetMediaTrackInfo_String(tr, "P_NAME", name, false) && name[0] != 0) {
        std::string s(name);
        if (s.size() > 12) s.resize(12);
        return s;
    }
    char fallback[8];
    std::snprintf(fallback, sizeof(fallback), "CH %d", slot + 1);
    return fallback;
}

// Convert REAPER linear-amplitude volume (0..~4) to a dB string that
// fits the O/PdB zone's 4-char value slot: "-inf", "-6.0", "0.0", "12.0".
//
// REAPER stores fader position as a linear multiplier — 1.0 = 0 dB.
// Below ~10^-5 we call it "-inf" to match what the SSL LCD shows at
// the fader bottom.
std::string formatDbReadout(double linearAmp)
{
    if (linearAmp < 1e-5) return "-inf";
    const double dB = 20.0 * std::log10(linearAmp);
    char buf[8];
    if (std::abs(dB) < 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1f", dB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", dB);
    }
    std::string s(buf);
    if (s.size() > 4) s.resize(4);
    return s;
}

// Compose the Value Line (19 chars) — e.g. "Vol        -6.0dB".
// Left-justified label, right-justified value. Truncates both if needed
// so the total fits within 19 chars.
std::string composeValueLine(std::string_view label, std::string_view value)
{
    constexpr size_t kWidth = 19;
    if (label.size() + value.size() + 1 > kWidth) {
        // Prefer to show full value; trim label.
        const size_t labelMax = kWidth - value.size() - 1;
        label = label.substr(0, labelMax);
    }
    std::string out(label);
    const size_t padding = kWidth - label.size() - value.size();
    out.append(padding, ' ');
    out.append(value);
    return out;
}

// Slot-level state caches — push only on change to avoid hammering the
// OUT endpoint 30× per second.
std::array<std::string, 8> g_lastSlotLabel{};
std::array<std::string, 8> g_lastCsType{};
std::array<std::string, 8> g_lastValueLine{};
std::array<std::string, 8> g_lastFaderDb{};

void pushZonesForVisibleSlots()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);

    for (int s = 0; s < 8; ++s) {
        MediaTrack* tr = (s < trackCount) ? GetTrack(nullptr, s) : nullptr;

        // Channel Strip Type zone: SSL plug-in short name if present, else
        // a 4-char REAPER mnemonic ("RPR ").
        std::string csType = tr ? sslPluginShortName(tr) : std::string{};
        if (csType.empty()) csType = "RPR ";
        csType.resize(std::min<size_t>(csType.size(), 4), ' ');
        while (csType.size() < 4) csType += ' ';
        if (csType != g_lastCsType[s]) {
            g_lastCsType[s] = csType;
            g_dev->send(uf8::buildChannelStripType(static_cast<uint8_t>(s), csType));
        }

        // Parameter Label (Currently Selected Parameter zone) — already
        // doubled as the "populate slot" activation flag; drive it with
        // the track name or SSL plug-in short name.
        std::string label = slotLabelForVisibleSlot(s);
        if (label != g_lastSlotLabel[s]) {
            g_lastSlotLabel[s] = label;
            g_dev->send(uf8::buildPluginSlotName(static_cast<uint8_t>(s), label));
        }

        if (!tr) continue;

        // O/PdB Fader Readout — track volume in dB.
        const double volLin = GetMediaTrackInfo_Value(tr, "D_VOL");
        std::string dbStr = formatDbReadout(volLin);
        if (dbStr != g_lastFaderDb[s]) {
            g_lastFaderDb[s] = dbStr;
            g_dev->send(uf8::buildFaderDbReadout(static_cast<uint8_t>(s), dbStr));
        }

        // Value Line — "Vol         -6.0dB" format, single combined row.
        std::string valLine = composeValueLine("Vol",
            dbStr == "-inf" ? std::string("-inf dB") : dbStr + "dB");
        if (valLine != g_lastValueLine[s]) {
            g_lastValueLine[s] = valLine;
            g_dev->send(uf8::buildValueLine(static_cast<uint8_t>(s), valLine));
        }
    }
}

void onTimer()
{
    if (g_sync) g_sync->refresh(reaperColorForVisibleSlot);
    pushZonesForVisibleSlots();
}

} // anonymous

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    if (!rec) {
        // Unload
        plugin_register("-timer", reinterpret_cast<void*>(onTimer));
        g_sync.reset();
        if (g_hid) g_hid->close();
        g_hid.reset();
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

    // Route every vendor-USB IN packet through our MCU translator.
    g_dev->setRawInputHandler(onUf8Input);

    // HID is locked behind Input Monitoring on macOS and the fader/v-pot
    // inputs we need are actually on vendor-USB EP 0x81 (parsed above).
    // Keep the HidDevice class around for eventual cross-platform use but
    // don't open it for now.

    // Poll at ~30 Hz. REAPER's timer callback fires on the main thread.
    plugin_register("timer", reinterpret_cast<void*>(onTimer));
    return 1;
}
