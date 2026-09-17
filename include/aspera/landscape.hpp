#pragma once
#include "aspera/voxel.hpp"

namespace aspera {
// Bounds are half-open, object-local cells. Noise is sampled in this same space.
struct LandscapeSettings {
    uint32_t seed{1};
    IVec3 minimum{-64, -32, -64}, maximum{64, 64, 64};
    float surfaceHeight{12}, hillAmplitude{18}, featureScale{48};
    float caveThreshold{.63f}, overhangStrength{12};
    int grassDepth{1}, soilDepth{3}, bottomDepth{4};
    MaterialId grassMaterial{5}, soilMaterial{6}, rockMaterial{7};
};
struct LandscapeData {
    VoxelGrid grid;
    std::set<IVec3> anchors;
};
void validateLandscapeSettings(const LandscapeSettings &);
// Raw chunk generation is order-independent; full generation also removes islands.
VoxelGrid generateLandscapeChunk(const LandscapeSettings &, IVec3 coordinate);
LandscapeData generateLandscape(const LandscapeSettings &);
} // namespace aspera
