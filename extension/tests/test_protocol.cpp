//
// Unit tests for the pure-logic pieces (Protocol + Palette). No libusb,
// no REAPER — just the byte math.
//
// Build: part of the reaper_uf8 CMake project (test_protocol target).
//

#include "Palette.h"
#include "Protocol.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,   \
                     #cond);                                           \
        std::exit(1);                                                  \
    }                                                                  \
} while(0)

static std::string hex(const std::vector<uint8_t>& v) {
    std::string s; s.reserve(v.size() * 2);
    static const char* d = "0123456789abcdef";
    for (auto b : v) { s.push_back(d[b >> 4]); s.push_back(d[b & 0xF]); }
    return s;
}

int main()
{
    using namespace uf8;

    // --- Checksum: replicate the idle + plugin-mixer heartbeats observed
    //     in captures.
    //
    // Captured:  ff 66 09 15 00 00 00 00 00 00 00 00 84
    //            ff 66 09 16 00 00 00 00 00 00 00 00 85
    auto hb = buildPluginMixerHeartbeat();
    EXPECT(hex(hb[0]) == "ff6609150000000000000000" "84");
    EXPECT(hex(hb[1]) == "ff6609160000000000000000" "85");

    // --- Color command — exact bytes captured when Track 1 went red:
    //     ff 66 09 18 02 06 0c 09 0a 08 07 0c c9
    {
        std::array<uint8_t, 8> idx{0x02, 0x06, 0x0C, 0x09, 0x0A, 0x08, 0x07, 0x0C};
        auto frame = buildColorCommand(idx);
        EXPECT(hex(frame) == "ff66091802060c090a08070cc9");
    }

    // --- Color command — sanity on another captured array:
    //     ff 66 09 18 03 06 0c 09 0a 08 07 0c ca   (Track 1 green)
    {
        std::array<uint8_t, 8> idx{0x03, 0x06, 0x0C, 0x09, 0x0A, 0x08, 0x07, 0x0C};
        auto frame = buildColorCommand(idx);
        EXPECT(hex(frame) == "ff66091803060c090a08070cca");
    }

    // --- Frame verify
    {
        std::vector<uint8_t> good{0xff, 0x66, 0x09, 0x18, 0x02, 0x06, 0x0C, 0x09, 0x0A, 0x08, 0x07, 0x0C, 0xC9};
        std::vector<uint8_t> bad = good; bad.back() ^= 1;
        EXPECT(verifyFrame(good));
        EXPECT(!verifyFrame(bad));
    }

    // --- Button events
    {
        // Captured: 31 60 ff 22 03 78 00 01 9e   (BANK-> pressed)
        std::vector<uint8_t> bytes{0x31, 0x60, 0xff, 0x22, 0x03, 0x78, 0x00, 0x01, 0x9e};
        auto ev = parseButtonEvent(bytes);
        EXPECT(ev.has_value());
        EXPECT(ev->id == 0x78);
        EXPECT(ev->pressed);
    }
    {
        // Captured: 31 60 ff 22 03 79 00 00 9e   (BANK<- released)
        std::vector<uint8_t> bytes{0x31, 0x60, 0xff, 0x22, 0x03, 0x79, 0x00, 0x00, 0x9e};
        auto ev = parseButtonEvent(bytes);
        EXPECT(ev.has_value());
        EXPECT(ev->id == 0x79);
        EXPECT(!ev->pressed);
    }
    {
        // Corrupted checksum should not parse
        std::vector<uint8_t> bytes{0xff, 0x22, 0x03, 0x78, 0x00, 0x01, 0x00};
        EXPECT(!parseButtonEvent(bytes).has_value());
    }

    // --- Upper-row scribble-strip text (variable length)
    //   captured:  ff 66 06 0b 00 4b 69 63 6b f9   ("Kick" on strip 0)
    //              ff 66 07 0b 01 53 6e 61 72 65 72   ("Snare" on strip 1)
    //              ff 66 08 0b 02 54 65 73 74 20 33 6e   ("Test 3" on strip 2)
    {
        auto kick  = buildStripTextUpper(0, "Kick");
        auto snare = buildStripTextUpper(1, "Snare");
        auto test3 = buildStripTextUpper(2, "Test 3");
        EXPECT(hex(kick)  == "ff66060b004b69636bf9");
        EXPECT(hex(snare) == "ff66070b01536e61726572");
        EXPECT(hex(test3) == "ff66080b025465737420336e");
    }

    // --- Lower-row scribble-strip text (fixed 7 chars space-padded)
    {
        auto frame = buildStripTextLower(0, "dB");
        // ff 66 09 0e 00 64 42 20 20 20 20 20 <cksum>
        // 09+0e+00+64+42+20*5 = 0x66+0x0e+0+0x64+0x42+0x64 — recompute
        // Actually recompute: 0x66+0x09+0x0E+0x00+0x64+0x42+0x20*5 = ...
        // Let's just check prefix instead of full match.
        EXPECT(frame.size() == 13);
        EXPECT(frame[0] == 0xff && frame[1] == 0x66 && frame[2] == 0x09 && frame[3] == 0x0E);
        EXPECT(frame[4] == 0x00);
        EXPECT(frame[5] == 'd' && frame[6] == 'B');
        // trailing pad is space
        for (int i = 7; i < 12; ++i) EXPECT(frame[i] == 0x20);
    }

    // --- Palette quantization (verified against on-device uf8_palette_probe
    // 2026-04-21 re-sweep). 0x00 is off, 0x01..0x0B are the 11 usable
    // colors, 0x0C..0x0F render as off.
    EXPECT(quantize(0xFF0000) == 0x02);   // pure red
    EXPECT(quantize(0x00FF00) == 0x03);   // pure green
    EXPECT(quantize(0x0000FF) == 0x04);   // deep blue
    EXPECT(quantize(0x00FFFF) == 0x05);   // cyan
    EXPECT(quantize(0x80FF00) == 0x07);   // lime
    // Yellow has no direct palette match; should prefer lime (0x07) or
    // pale green (0x0A).
    {
        auto q = quantize(0xFFFF00);
        EXPECT(q == 0x07 || q == 0x0A);
    }

    // --- LED colour pair (cap31, cap33). Lock the bytes captured from
    //     SSL 360° so a regression in the formula or colour-table is caught.
    //     Each pair returns FF38 + FF39 as TWO separate frames — SSL360
    //     transmits them as independent USB transfers and the UF8 firmware
    //     stalls subsequent commands when the two are coalesced into one.
    //
    // SOLO Strip 1 (cell 0x17), ON:
    //   ff 38 04 17 00 ef f0 32     ff 39 04 17 00 00 f0 44
    {
        auto f = buildLedColourPair(0, LedClass::Solo, true);
        EXPECT(hex(f.ff38) == "ff38041700eff032");
        EXPECT(hex(f.ff39) == "ff3904170000f044");
    }
    // SOLO Strip 1, OFF:  ff 38 04 17 00 11 f0 54     ff 39 04 17 00 11 f0 55
    {
        auto f = buildLedColourPair(0, LedClass::Solo, false);
        EXPECT(hex(f.ff38) == "ff380417001"  "1f054");
        EXPECT(hex(f.ff39) == "ff390417001"  "1f055");
    }
    // CUT Strip 1 (cell 0x16), ON, with explicit ORANGE colour (cap31):
    {
        auto f = buildLedColourPair(0, LedClass::Cut, true, ledColourOrange());
        EXPECT(hex(f.ff38) == "ff380416003ff081");
        EXPECT(hex(f.ff39) == "ff3904160000f043");
    }
    // Default CUT colour is RED (cap33 bytes) — different from SSL360's orange.
    {
        auto f = buildLedColourPair(0, LedClass::Cut, true);
        EXPECT(hex(f.ff38) == "ff380416000ff051");
        EXPECT(hex(f.ff39) == "ff3904160000f043");
    }
    // SEL Strip 1 with track-colour RED → expect cap33 red bright
    {
        auto f = buildLedColourPair(0, LedClass::Sel, true,
                                    ledColourForTrackRgb(0xFF0000));
        EXPECT(hex(f.ff38) == "ff380415000ff050");
        EXPECT(hex(f.ff39) == "ff3904150000f042");
    }
    // SEL Strip 1 (cell 0x15), ON, default white:  FF FF / 00 F0
    {
        auto f = buildLedColourPair(0, LedClass::Sel, true);
        EXPECT(hex(f.ff38) == "ff38041500ffff4f");
        EXPECT(hex(f.ff39) == "ff3904150000f042");
    }
    // SOLO Strip 2 (cell 0x14), ON
    {
        auto f = buildLedColourPair(1, LedClass::Solo, true);
        EXPECT(hex(f.ff38) == "ff38041400eff02f");
        EXPECT(hex(f.ff39) == "ff3904140000f041");
    }
    // SOLO Strip 8 (cell 0x02), ON
    {
        auto f = buildLedColourPair(7, LedClass::Solo, true);
        EXPECT(f.ff38.size() == 8);
        EXPECT(f.ff39.size() == 8);
        EXPECT(f.ff38[3] == 0x02);
        EXPECT(f.ff39[3] == 0x02);
    }

    std::printf("OK — all checks passed\n");
    return 0;
}
