#include "Palette.h"

#include <algorithm>
#include <array>
#include <limits>

namespace uf8 {

namespace {

// Project a colour onto its chromaticity by scaling so the largest channel
// is 255. Removes brightness from the comparison — without this, dark
// inputs (e.g. REAPER's r12/g25/b84 dark blue) prefer the dark-orange
// palette entry over deep blue purely because raw RGB Euclidean distance
// rewards "small numbers" and our palette has no muted-blue entry.
Rgb chromaticity(Rgb c)
{
    const uint8_t m = std::max({c.r, c.g, c.b});
    if (m == 0) return c;
    return Rgb{
        static_cast<uint8_t>((c.r * 255 + m / 2) / m),
        static_cast<uint8_t>((c.g * 255 + m / 2) / m),
        static_cast<uint8_t>((c.b * 255 + m / 2) / m),
    };
}


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
    // Nearest-match in chromaticity space (each colour normalised so its
    // brightest channel == 255). Comparing raw RGB makes dark inputs
    // gravitate to whichever palette entry happens to have the lowest
    // total brightness — which is why r12/g25/b84 dark blue used to land
    // on the dark-orange entry instead of deep blue.
    const Rgb cn = chromaticity(c);
    int bestDist = std::numeric_limits<int>::max();
    uint8_t bestIdx = 0x01;  // default if literally nothing matches

    for (uint8_t i = 0; i < 16; ++i) {
        if (!kHasEntry[i]) continue;
        const Rgb pn = chromaticity(kPalette[i]);
        const int dr = static_cast<int>(cn.r) - pn.r;
        const int dg = static_cast<int>(cn.g) - pn.g;
        const int db = static_cast<int>(cn.b) - pn.b;
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
