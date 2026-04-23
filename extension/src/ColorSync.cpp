#include "ColorSync.h"

#include "Palette.h"

#include <cstdio>

namespace uf8 {

void ColorSync::refresh(const std::function<uint32_t(int)>& getTrackColor)
{
    if (!dev_.isOpen()) return;

    // Palette index 0x00 is OFF on the UF8 firmware. (Palette.cpp also
    // lists 0x0C..0x0F as "OFF" in its comments, but direct hardware
    // test 2026-04-23 showed 0x0C rendering as a light blue — so those
    // entries are NOT reliably dark. 0x00 is the safe choice.) Used for
    // strips past the end of the track list so the color bar goes dark
    // instead of quantizing 0x000000 to some nearest-match palette
    // entry. Tracks with no custom color also report 0 from
    // GetTrackColor; they end up OFF too, which matches user
    // expectation better than an arbitrary filler color.
    constexpr uint8_t kPaletteOff = 0x00;
    std::array<uint8_t, kStripCount> indices{};
    for (int i = 0; i < static_cast<int>(kStripCount); ++i) {
        const uint32_t rgb = getTrackColor(i) & 0x00FFFFFFu;
        indices[i] = (rgb == 0) ? kPaletteOff : quantize(rgb);
    }

    if (lastPushedValid_ && indices == lastPushed_) return;

    if (FILE* f = std::fopen("/tmp/reaper_uf8_colors.log", "a")) {
        std::fprintf(f, "ColorSync push: ");
        for (int i = 0; i < static_cast<int>(kStripCount); ++i) {
            const uint32_t rgb = getTrackColor(i) & 0x00FFFFFFu;
            std::fprintf(f, "[%d] rgb=0x%06x -> idx=0x%02x  ",
                         i, rgb, indices[i]);
        }
        std::fprintf(f, "\n");
        std::fclose(f);
    }

    dev_.send(buildColorCommand(indices));
    lastPushed_      = indices;
    lastPushedValid_ = true;
}

} // namespace uf8
