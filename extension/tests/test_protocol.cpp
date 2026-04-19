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

    // --- Palette quantization (exact matches we have captures for)
    EXPECT(quantize(0xFF0000) == 0x02);   // pure red
    EXPECT(quantize(0x00FF00) == 0x03);   // pure green
    EXPECT(quantize(0x0000FF) == 0x04);   // pure blue
    EXPECT(quantize(0xFF8000) == 0x0B);   // bright orange
    EXPECT(quantize(0xFFFFFF) == 0x0E);   // white
    // Nearest-match on an off-palette input: reddish-orange should land on
    // whichever measured entry is closest. Accept {red, orange}.
    {
        auto q = quantize(0xFF4000);
        EXPECT(q == 0x02 || q == 0x0B);
    }

    std::printf("OK — %d checks passed\n", 14);
    return 0;
}
