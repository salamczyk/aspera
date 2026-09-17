#pragma once
#include "aspera/voxel.hpp"

namespace aspera {
// Cursor and viewport dimensions are in the same logical window coordinates.
// Matches the renderer's full-window perspective projection (60 degree vertical FOV).
inline Vec3 perspectivePickRay(Vec3 forward, float x, float y, float width, float height) {
    forward = normalized(forward);
    if (width <= 0 || height <= 0)
        return forward;
    const auto right = normalized(cross(Vec3{0, 1, 0}, forward));
    const auto up = normalized(cross(forward, right));
    constexpr float tanHalfFov = .577350269f;
    return normalized(forward + right * ((2 * x / width - 1) * tanHalfFov * width / height) +
                      up * ((1 - 2 * y / height) * tanHalfFov));
}
} // namespace aspera
