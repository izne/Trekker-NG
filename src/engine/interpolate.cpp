#include "interpolate.h"

#include <cmath>

namespace sde {

float interpLinear(float a, float b, float t) {
    return a + (b - a) * t;
}

float interpHermite(float y0, float y1, float y2, float y3, float t) {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    return ((c3 * t + c2) * t + c1) * t + y1;
}

float readFrame(const float* data,
                int64_t frameCount,
                int32_t channels,
                int32_t channel,
                double pos,
                InterpMode mode) {
    if (frameCount <= 0 || pos < 0.0 || pos >= static_cast<double>(frameCount)) {
        return 0.0f;
    }

    const int64_t i1 = static_cast<int64_t>(std::floor(pos));
    const float t = static_cast<float>(pos - static_cast<double>(i1));

    // Clamp neighbor taps at the buffer edges instead of padding with zeros:
    // a flat extrapolation keeps the interpolator continuous at the boundary.
    auto at = [&](int64_t i) -> float {
        if (i < 0) i = 0;
        if (i >= frameCount) i = frameCount - 1;
        return data[i * channels + channel];
    };

    if (mode == InterpMode::Linear) {
        return interpLinear(at(i1), at(i1 + 1), t);
    }
    return interpHermite(at(i1 - 1), at(i1), at(i1 + 1), at(i1 + 2), t);
}

} // namespace sde
