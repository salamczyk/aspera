#pragma once
#include "aspera/engine.hpp"
#include <memory>

struct SDL_Window;
struct ImDrawData;

namespace aspera {
enum class RenderQuality { Balanced, Low };
enum class RenderDebug { Final, Material, Direct, Shadows, Indirect, AO, Voxels };
struct RenderTimings {
    double cpuMs{}, gpuMs{}, giMs{};
    uint64_t textureBytes{}, giBytes{};
    uint32_t pendingGi{}, drawCalls{};
    uint32_t pendingMeshJobs{}, queuedMeshJobs{}, rebuiltChunks{}, cachedChunks{};
    double meshPrepareMs{};
    uint64_t geometryBytes{};
    uint32_t gpuFrame{}, giFrame{};
    struct Pass {
        uint16_t id{};
        std::string name;
        double cpuMs{}, gpuMs{};
        uint32_t gpuFrame{};
    };
    std::vector<Pass> passes;
};
class Renderer {
  public:
    Renderer();
    ~Renderer();
    bool init(SDL_Window *, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);
    void begin(Vec3 eye, const float *view);
    void drawWorld(const World &, bool stressOverlay);
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
