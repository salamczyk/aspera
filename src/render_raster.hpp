#pragma once
#include "render.hpp"
namespace aspera {
class RasterRenderer {
  public:
    RasterRenderer();
    ~RasterRenderer();
    bool init(SDL_Window *, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);
    void begin(Vec3 eye, const float *view);
    void drawWorld(const World &, bool stressOverlay,
                   const std::map<PlayerId, PlayerState> &players = {}, PlayerId localPlayer = 0);
    void drawImGui(ImDrawData *);
    void invalidateMeshes();
    void setQuality(RenderQuality);
    RenderQuality quality() const;
    void setDebug(RenderDebug);
    RenderDebug debug() const;
    void setBloom(bool);
    void setVsync(bool);
    RenderTimings timings() const;
    void end();
    void screenshot(const std::filesystem::path &);

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace aspera
