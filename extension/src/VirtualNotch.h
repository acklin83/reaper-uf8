#pragma once
//
// Virtual notch / detent helper for relative-encoder parameter writes.
//
// SSL 360°'s UC1 implementation makes pots "snap" to a parameter's
// neutral point (0 dB on EQ gains, 0 on pan) when the rotation passes
// near it: instead of grazing past 0 you land on it, and have to turn
// past a small dead-zone to leave again. This module replicates that
// feel for our UC1 + UF8 encoder paths.
//
// Caller owns one NotchState per control. The helper reads the current
// parameter value, the user's just-rotated delta, and the notch centre,
// and returns the value to write. `zone` defines both the snap radius
// (how close to centre you have to land for the snap to grab) and the
// rotation needed to leave once snapped.
//

#include <chrono>

namespace uf8 {

struct NotchState {
    double accumDelta = 0.0;
    std::chrono::steady_clock::time_point lastT{};
};

// Returns the value to write.
//   cur     — current parameter value
//   delta   — signed delta in same units as cur
//   center  — notch position (0.5 for normalized bipolar, 0.0 for pan -1..+1)
//   zone    — half-width of the snap radius (also the exit threshold)
//   state   — per-control mutable state (caller owns)
//   lo, hi  — clamp range
double applyVirtualNotch(double cur, double delta, double center,
                         double zone, NotchState& state,
                         double lo, double hi);

} // namespace uf8
