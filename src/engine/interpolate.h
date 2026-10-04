#pragma once

#include <cstdint>

namespace tng {

// Interpolator selection. Hermite (Catmull-Rom) is the default; linear is a
// debug option (SPEC §4.3).
enum class InterpMode { Linear = 0, Hermite = 1 };

// Pure functions only - unit tested in M2 (SPEC §9).

// t in [0,1). a is sample at floor(pos), b at floor(pos)+1.
float interpLinear(float a, float b, float t);

// Catmull-Rom cubic Hermite: passes through y1 at t=0 and y2 at t=1, so at
// integer positions the output equals the input bit-exactly (SPEC §9.5).
float interpHermite(float y0, float y1, float y2, float y3, float t);

// Reads one channel from an interleaved buffer at fractional frame `pos`.
// Returns silence outside [0, frameCount); neighbor taps are clamped at the
// buffer edges so the first/last frame never reads out of bounds.
float readFrame(const float* data,
                int64_t frameCount,
                int32_t channels,
                int32_t channel,
                double pos,
                InterpMode mode);

} // namespace tng
