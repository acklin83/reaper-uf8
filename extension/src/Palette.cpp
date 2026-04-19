#include "Palette.h"

#include <array>
#include <limits>

namespace uf8 {

namespace {

// Reference RGB for each mapped palette index.
//
// Only entries we've *directly captured* with a known input RGB are marked
// active. Screenshot guesses (Tracks 2/4/5/6/7/8 in the first session) are
// excluded — visual estimation of on-screen RGB is unreliable and can
// poison nearest-match quantization. A follow-up systematic palette sweep
// will fill the remaining 11 entries; for now an incomplete palette maps
// input colors to their closest measured index, which is safe.
constexpr std::array<Rgb, 16> kPalette{{
    {0x00, 0x00, 0x00},  // 0x00  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x01  UNMEASURED
    {0xFF, 0x00, 0x00},  // 0x02  RED   — cap02 Track 1 #FF0000
    {0x00, 0xFF, 0x00},  // 0x03  GREEN — cap03 Track 1 #00FF00
    {0x00, 0x00, 0xFF},  // 0x04  BLUE  — cap04 Track 1 #0000FF
    {0x00, 0x00, 0x00},  // 0x05  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x06  UNMEASURED (screenshot-only, too imprecise)
    {0x00, 0x00, 0x00},  // 0x07  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x08  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x09  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x0A  UNMEASURED
    {0xFF, 0x80, 0x00},  // 0x0B  ORANGE — cap05 Track 1 #FF8000
    {0x00, 0x00, 0x00},  // 0x0C  UNMEASURED
    {0x00, 0x00, 0x00},  // 0x0D  UNMEASURED
    {0xFF, 0xFF, 0xFF},  // 0x0E  WHITE  — cap05 Track 1 #FFFFFF
    {0x00, 0x00, 0x00},  // 0x0F  UNMEASURED
}};

constexpr std::array<bool, 16> kHasEntry{{
    false, false, true,  true,  true,  false,
    false, false, false, false, false, true,
    false, false, true,  false
}};

} // anonymous

std::optional<Rgb> paletteEntry(uint8_t index)
{
    if (index >= 16 || !kHasEntry[index]) return std::nullopt;
    return kPalette[index];
}

uint8_t quantize(Rgb c)
{
    // Nearest-match in RGB Euclidean space, restricted to mapped entries.
    // When we sweep the full palette we can either remove the restriction or
    // keep it (unmapped entries become "never chosen" which is safer).
    int bestDist = std::numeric_limits<int>::max();
    uint8_t bestIdx = 0x02;  // default to red if literally nothing matches

    for (uint8_t i = 0; i < 16; ++i) {
        if (!kHasEntry[i]) continue;
        const auto& p = kPalette[i];
        const int dr = static_cast<int>(c.r) - p.r;
        const int dg = static_cast<int>(c.g) - p.g;
        const int db = static_cast<int>(c.b) - p.b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}

uint8_t quantize(uint32_t rgb24)
{
    return quantize(Rgb{
        static_cast<uint8_t>((rgb24 >> 16) & 0xFF),
        static_cast<uint8_t>((rgb24 >>  8) & 0xFF),
        static_cast<uint8_t>((rgb24      ) & 0xFF)
    });
}

} // namespace uf8
