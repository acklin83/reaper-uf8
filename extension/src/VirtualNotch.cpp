#include "VirtualNotch.h"

#include <algorithm>  // std::clamp
#include <cmath>

namespace uf8 {

double applyVirtualNotch(double cur, double delta, double center,
                         double zone, double lo, double hi)
{
    const double next = std::clamp(cur + delta, lo, hi);

    // Fast-spin: a single rotation crossed through the centre. Snap
    // so the user lands ON the notch instead of skipping past it.
    if ((cur - center) * (next - center) < 0.0) return center;

    // Slow approach: entering the zone from outside. First event that
    // lands in the zone snaps; once in, further events move normally.
    if (std::abs(cur - center) > zone && std::abs(next - center) <= zone)
        return center;

    return next;
}

} // namespace uf8
