#include "Palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace uf8 {

namespace {

// Project a colour onto the HSV chromatic plane: (S·cosH, S·sinH).
// Using HSV — not normalised RGB — because Euclidean distance in
// chromaticity-RGB treats "green tint" and "red tint" as equally far
// from pure blue, so r12/g25/b84 (slight green tint, hue 229°) ended
// up matching the red-tinted "blue-leaning violet" palette entry
// (hue 255°) instead of pure deep blue (hue 240°). Polar HSV
// coordinates put opposite tints on opposite sides, so hue distance
// behaves correctly. Saturation as the radius gives desaturated
// inputs a shorter pull on hue, which lets light/desaturated
// palette entries (e.g. light violet 0x01) win over saturated ones
// for low-saturation inputs without an extra weighting hack.
//
// hueDeg is carried alongside (x, y) so the family-affinity step
// below can pull entries in the same perceptual hue band toward the
// input — without it, hue 208° lands on cyan 180° instead of deep
// blue 240° because the angular gap is mathematically smaller.
struct ChromaXY { double x, y, hueDeg; bool grey; };

ChromaXY chromaXY(Rgb c)
{
    const int mx = std::max({c.r, c.g, c.b});
    const int mn = std::min({c.r, c.g, c.b});
    if (mx == 0 || mx == mn) return {0.0, 0.0, 0.0, true};
    const double chroma = static_cast<double>(mx - mn);
    const double s = chroma / mx;
    double h_deg = 0.0;
    if (mx == c.r) {
        h_deg = 60.0 * ((static_cast<double>(c.g) - c.b) / chroma);
        if (h_deg < 0) h_deg += 360.0;
    } else if (mx == c.g) {
        h_deg = 60.0 * (((static_cast<double>(c.b) - c.r) / chroma) + 2.0);
    } else {
        h_deg = 60.0 * (((static_cast<double>(c.r) - c.g) / chroma) + 4.0);
    }
    const double h_rad = h_deg * 3.14159265358979323846 / 180.0;
    return {s * std::cos(h_rad), s * std::sin(h_rad), h_deg, false};
}

// Perceptual "blue family" band — hues humans would unambiguously
// call blue. Cyan (180°) is on one side, violet (~270°) on the other.
// 195..260° covers everything between azure and indigo without
// poaching from cyan (deep cyan still reads cyan, deep violet still
// reads violet). Used as a soft prior in nearest-match: input AND
// palette-entry hues both inside the band → distance×0.6, pulling
// blue-family inputs toward blue-family palette entries even when
// cyan happens to be slightly closer in raw HSV-polar Euclidean.
//
// Only blue is biased — the other 10 palette anchors are well
// separated in hue, no other family needs the prior. If a future
// regression demands it, add bands here; keep them narrow enough
// that pure cyan / pure violet still match their own anchors.
constexpr double kBlueBandLo = 195.0;
constexpr double kBlueBandHi = 260.0;
constexpr double kFamilyMul  = 0.6;

bool inBlueBand(double hueDeg)
{
    return hueDeg >= kBlueBandLo && hueDeg <= kBlueBandHi;
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
    // Nearest-match in the HSV chromatic plane — see chromaXY() above
    // for the why.
    const ChromaXY cxy = chromaXY(c);
    const bool inputBlueFamily = !cxy.grey && inBlueBand(cxy.hueDeg);
    double bestDist = std::numeric_limits<double>::infinity();
    uint8_t bestIdx = 0x01;  // default if literally nothing matches

    for (uint8_t i = 0; i < 16; ++i) {
        if (!kHasEntry[i]) continue;
        const ChromaXY pxy = chromaXY(kPalette[i]);
        const double dx = cxy.x - pxy.x;
        const double dy = cxy.y - pxy.y;
        double d = dx * dx + dy * dy;
        // Soft pull toward the blue family for blue-family inputs.
        // Without this, hue 208° (between cyan 180° and deep blue
        // 240°) lands on cyan because the angular gap is smaller.
        if (inputBlueFamily && !pxy.grey && inBlueBand(pxy.hueDeg)) {
            d *= kFamilyMul;
        }
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
