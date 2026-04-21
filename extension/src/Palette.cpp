#include "Palette.h"

#include <array>
#include <limits>

namespace uf8 {

namespace {

// Reference RGB for each palette index — identified by direct on-device
// probe (uf8_palette_probe, re-run 2026-04-21 after the original sweep
// produced shifted mappings). 0x00 is off on this hardware; 0x01..0x0B
// are the 11 usable colors; 0x0C..0x0F render black/off and are skipped
// during quantization.
//
// RGB values are eyeballed approximations of what the UF8 LCD shows —
// accurate enough for Euclidean-distance nearest-match on the REAPER
// track-color picker's typical output.
constexpr std::array<Rgb, 16> kPalette{{
    {0x00, 0x00, 0x00},  // 0x00  OFF
    {0xA0, 0xA0, 0xFF},  // 0x01  light blue / light violet
    {0xFF, 0x00, 0x00},  // 0x02  red
    {0x00, 0xFF, 0x00},  // 0x03  bright green
    {0x00, 0x00, 0xFF},  // 0x04  deep blue
    {0x00, 0xFF, 0xFF},  // 0x05  cyan
    {0xC0, 0x00, 0xFF},  // 0x06  violet (purple)
    {0x80, 0xFF, 0x00},  // 0x07  lime green
    {0x80, 0x40, 0x00},  // 0x08  dark orange / brown
    {0x40, 0x00, 0xFF},  // 0x09  blue leaning violet
    {0xA0, 0xFF, 0xA0},  // 0x0A  pale light green
    {0xC0, 0x80, 0xFF},  // 0x0B  lighter violet
    {0x00, 0x00, 0x00},  // 0x0C  OFF
    {0x00, 0x00, 0x00},  // 0x0D  OFF
    {0x00, 0x00, 0x00},  // 0x0E  OFF
    {0x00, 0x00, 0x00},  // 0x0F  OFF
}};

constexpr std::array<bool, 16> kHasEntry{{
    false, true,  true,  true,  true,  true,
    true,  true,  true,  true,  true,  true,
    false, false, false, false
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
    uint8_t bestIdx = 0x01;  // default to red if literally nothing matches

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
