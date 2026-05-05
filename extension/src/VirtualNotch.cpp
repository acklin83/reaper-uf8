#include "VirtualNotch.h"

#include <algorithm>  // std::clamp
#include <cmath>

namespace uf8 {

namespace {
constexpr auto kIdleReset = std::chrono::milliseconds(300);
}

double applyVirtualNotch(double cur, double delta, double center,
                         double zone, NotchState& state,
                         double lo, double hi)
{
    auto now = std::chrono::steady_clock::now();
    if (state.lastT.time_since_epoch().count() != 0
        && now - state.lastT > kIdleReset) {
        state.accumDelta = 0.0;
    }
    state.lastT = now;

    // Sitting at the notch: accumulate deltas; release when the user
    // has rotated `zone` worth (= the same radius they had to cross
    // to enter — symmetric feel).
    if (std::abs(cur - center) < 1e-9) {
        // Direction reversal clears the accumulator so the user can
        // back out without fighting their own previous rotation.
        if ((delta > 0.0 && state.accumDelta < 0.0) ||
            (delta < 0.0 && state.accumDelta > 0.0)) {
            state.accumDelta = 0.0;
        }
        state.accumDelta += delta;
        if (std::abs(state.accumDelta) > zone) {
            const double exitVal = std::clamp(center + state.accumDelta, lo, hi);
            state.accumDelta = 0.0;
            return exitVal;
        }
        return center;
    }

    double next = std::clamp(cur + delta, lo, hi);

    // Crossing through the centre in one event (high-velocity rotation):
    // snap so a fast spin can't skip the notch entirely.
    if ((cur - center) * (next - center) < 0.0) {
        state.accumDelta = 0.0;
        return center;
    }

    // Entering the snap zone from outside.
    const bool curInZone  = std::abs(cur  - center) <= zone;
    const bool nextInZone = std::abs(next - center) <= zone;
    if (!curInZone && nextInZone) {
        state.accumDelta = 0.0;
        return center;
    }
    return next;
}

} // namespace uf8
