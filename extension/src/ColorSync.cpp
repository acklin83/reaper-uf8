#include "ColorSync.h"

#include "Palette.h"

namespace uf8 {

void ColorSync::refresh(const std::function<uint32_t(int)>& getTrackColor)
{
    if (!dev_.isOpen()) return;

    std::array<uint8_t, kStripCount> indices{};
    for (int i = 0; i < static_cast<int>(kStripCount); ++i) {
        const uint32_t rgb = getTrackColor(i) & 0x00FFFFFFu;
        indices[i] = quantize(rgb);
    }

    if (lastPushedValid_ && indices == lastPushed_) return;

    dev_.send(buildColorCommand(indices));
    lastPushed_      = indices;
    lastPushedValid_ = true;
}

} // namespace uf8
