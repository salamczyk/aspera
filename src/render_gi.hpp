#pragma once
#include "aspera/engine.hpp"
#include <bgfx/bgfx.h>
#include <memory>

namespace aspera {
struct GiDirectLighting {
    bgfx::TextureHandle shadowAtlas{bgfx::kInvalidHandle};
    const float *shadowMatrices{}; // 15 matrices
    const float *shadowRects{};    // 15 vec4 tile bounds
    const float *shadowParams{};   // cascade distances, atlas texel size
    const float *lightData{};      // 64 lights, five vec4s each, radiometric units
    const float *view{};
    uint32_t lightCount{};
};
class VoxelGi {
  public:
    VoxelGi();
    ~VoxelGi();
    VoxelGi(const VoxelGi &) = delete;
    VoxelGi &operator=(const VoxelGi &) = delete;
    bool init();
    void resize(uint16_t width, uint16_t height, bool low);
    void invalidate();
    void render(const World &, Vec3 eye, const float *inverseViewProjection,
                const float *previousViewProjection, bgfx::TextureHandle depth,
                bgfx::TextureHandle normal, bgfx::TextureHandle motion, bgfx::TextureHandle palette,
                const GiDirectLighting &, bool resetHistory, bool debugVolume = false);
    bgfx::TextureHandle texture() const;
    bgfx::TextureHandle visibilityTexture() const;
    uint32_t pendingBricks() const;
    uint64_t memoryBytes() const;
    void shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace aspera
