#pragma once
#include "render_pt.hpp"
namespace aspera {
// CPU preparation is independent of GPU backend and includes boundary-neighbor keys.
Mesh prepareRenderChunk(const VoxelGrid &, IVec3, float, const std::vector<Material> &);
class RenderSceneBuilder {
  public:
    RenderSceneBuilder();
    ~RenderSceneBuilder();
    const PtScene &update(const World &, const std::map<PlayerId, PlayerState> &, PlayerId);
    void invalidate();
    bool ready() const;
    void statistics(RenderTimings &) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace aspera
