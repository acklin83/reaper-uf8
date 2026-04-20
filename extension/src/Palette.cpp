#include "Palette.h"

#include <array>
#include <limits>

namespace uf8 {

namespace {

// Reference RGB for each palette index — identified by direct on-device
// probe (uf8_palette_probe, 2026-04-20). The UF8 exposes 12 usable colors;
// indices 0x0C..0x0F render as black/off and are excluded from quantization.
//
// RGB values are approximations of what the UF8 LCD shows — close enough
// for Euclidean-distance nearest-match. A lab-grade spectrometer sweep
// could refine these but the perceptual mapping works for the REAPER
// color picker's typical output.
constexpr std::array<Rgb, 16> kPalette{{
    {0xFF, 0x80, 0xFF},  // 0x00  hellviolet — light magenta
    {0xFF, 0x00, 0x00},  // 0x01  rot
    {0x00, 0xFF, 0x00},  // 0x02  grün
    {0x00, 0x00, 0xFF},  // 0x03  blau
    {0x00, 0xFF, 0xFF},  // 0x04  hellblau — cyan
    {0x80, 0x00, 0xFF},  // 0x05  violett — purple
    {0x80, 0xFF, 0x00},  // 0x06  hellgrün — lime
    {0xFF, 0x80, 0x00},  // 0x07  orange (dunkel)
    {0x40, 0x00, 0xFF},  // 0x08  blauviolet — deeper blue-purple
    {0x80, 0xFF, 0x80},  // 0x09  hellgrün (hell) — pale green
    {0xFF, 0x00, 0xFF},  // 0x0A  violet (magenta)
    {0x40, 0x80, 0xFF},  // 0x0B  blau (variant) — mid-light blue
    {0x00, 0x00, 0x00},  // 0x0C  OFF
    {0x00, 0x00, 0x00},  // 0x0D  OFF
    {0x00, 0x00, 0x00},  // 0x0E  OFF
    {0x00, 0x00, 0x00},  // 0x0F  OFF
}};

constexpr std::array<bool, 16> kHasEntry{{
    true,  true,  true,  true,  true,  true,
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
