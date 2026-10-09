#pragma once
#include "render.hpp"
#include <array>
#include <span>
namespace aspera {
struct PtSettings {
    uint32_t samples{1}, bounces{6}, captureSamples{};
    bool denoise{true};
};
struct alignas(16) PtVertex {
    float position[4], normal[4], color[4];
};
struct alignas(16) PtMaterial {
    float baseRoughness[4], emissionMetallic[4];
};
struct alignas(16) PtInstance {
    float model[16], inverse[16], previous[16];
    uint32_t offsets[4];
};
struct alignas(16) PtLight {
    float positionRange[4], colorIntensity[4], directionType[4], cone[4];
};
struct alignas(16) PtEmitter {
    float aArea[4], bCdf[4], cPad[4], radiance[4];
};
struct alignas(16) PtParams {
    float inverseVP[16], viewProjection[16], previousVP[16];
    float eye[4], sunDirectionIntensity[4], sunColorRadius[4], skyColorIntensity[4];
    float post[4];         // exposure multiplier, bloom strength, reserved, reserved
    uint32_t sizeFrame[4]; // width, height, sample offset, history reset
    uint32_t counts[4];    // samples this frame, bounce limit, light count, emitter count
    uint32_t options[4];   // denoise enabled, stationary accumulation, previous sample count,
                           // instance count
};
static_assert(sizeof(PtVertex) == 48 && sizeof(PtInstance) == 208 && sizeof(PtParams) == 320);
struct PtGeometry {
    uint64_t id{}, key{};
    uint32_t vertexOffset{}, vertexCount{}, indexOffset{}, indexCount{};
};
struct PtScene {
    std::vector<PtVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<PtGeometry> geometry;
    std::vector<PtInstance> instances;
    std::vector<PtMaterial> materials;
    std::vector<PtLight> lights;
    std::vector<PtEmitter> emitters;
    uint64_t geometryRevision{}, transformRevision{}, lightingRevision{};
};
// Shader image indices are shared by Metal and Vulkan. All images are RGBA32F.
enum PtImage : uint32_t {
    PtRawDiffuse,
    PtRawSpecular,
    PtGuide,
    PtAlbedo,
    PtMotion,
    PtHistoryDiffuse,
    PtHistorySpecular,
    PtHistoryGuide,
    PtHistoryMotion,
    PtMoments,
    PtPreviousMoments,
    PtFilterA,
    PtFilterB,
    PtOutput,
    PtScratchA,
    PtScratchB,
    PtPreviousGuide,
    PtPreviousMotion,
    PtPreviousDiffuse,
    PtPreviousSpecular,
    PtImageCount
};
// Native backends own all GPU objects; no bgfx/native resource sharing.
class NativePt {
  public:
    virtual ~NativePt() = default;
    virtual bool init(SDL_Window *, uint32_t, uint32_t, std::string &reason) = 0;
    virtual void resize(uint32_t, uint32_t) = 0;
    virtual bool render(const PtScene &, const PtParams &) = 0;
    virtual void drawImGui(ImDrawData *) = 0;
    virtual void end() = 0;
    virtual void screenshot(const std::filesystem::path &) = 0;
    virtual void setVsync(bool) = 0;
    virtual RenderTimings timings() const = 0;
};
std::unique_ptr<NativePt> makeMetalPt();
std::unique_ptr<NativePt> makeVulkanPt();
std::filesystem::path ptShaderPath(const char *name);
class PathTracer {
  public:
    PathTracer();
    ~PathTracer();
    bool init(SDL_Window *, uint32_t, uint32_t, const PtSettings &, std::string &);
    void resize(uint32_t, uint32_t);
    void begin(Vec3, const float *);
    void drawWorld(const World &, bool, const std::map<PlayerId, PlayerState> &, PlayerId);
    void drawImGui(ImDrawData *);
    void invalidateMeshes();
    void setBloom(bool);
    void setVsync(bool);
    void setDebug(RenderDebug);
    RenderTimings timings() const;
    void end();
    void screenshot(const std::filesystem::path &);
    uint32_t accumulatedSamples() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace aspera
