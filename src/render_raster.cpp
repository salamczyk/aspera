#include "render_raster.hpp"
#include "render_gi.hpp"
#include "render_scene.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <imgui.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#ifndef ASPERA_SHADER_DIR
#define ASPERA_SHADER_DIR "assets/shaders"
#endif
namespace aspera {
namespace {
constexpr uint16_t Invalid = bgfx::kInvalidHandle;
constexpr bgfx::ViewId WorldView = 20, AoView = 145, LightView = 150, BloomView = 160,
                       ToneView = 180, StressView = 190, UiView = 200;
template <class H> bool valid(H h) {
    return bgfx::isValid(h);
}
template <class H> void release(H &h) {
    if (valid(h))
        bgfx::destroy(h);
    h = {Invalid};
}
float radians(float v) {
    return v * 0.01745329251994329577f;
}
float srgb(float v) {
    return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
struct V4 {
    float x, y, z, w;
};
V4 multiply(const float *m, V4 p) {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * p.w,
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * p.w,
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * p.w,
            m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15] * p.w};
}
class CaptureCallback final : public bgfx::CallbackI {
  public:
    void fatal(const char *file, uint16_t line, bgfx::Fatal::Enum, const char *message) override {
        std::fprintf(stderr, "bgfx fatal %s:%u: %s\n", file, line, message);
        std::abort();
    }
    void traceVargs(const char *, uint16_t, const char *format, va_list args) override {
        if (std::getenv("ASPERA_BGFX_TRACE"))
            std::vfprintf(stderr, format, args);
    }
    void profilerBegin(const char *, uint32_t, const char *, uint16_t) override {}
    void profilerBeginLiteral(const char *, uint32_t, const char *, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override {
        return 0;
    }
    bool cacheRead(uint64_t, void *, uint32_t) override {
        return false;
    }
    void cacheWrite(uint64_t, const void *, uint32_t) override {}
    void screenShot(const char *path, uint32_t width, uint32_t height, uint32_t pitch,
                    bgfx::TextureFormat::Enum, const void *data, uint32_t, bool yflip) override {
        std::filesystem::path output(path ? path : "aspera.ppm");
        output.replace_extension(".ppm");
        std::ofstream file(output, std::ios::binary);
        if (!file)
            return;
        file << "P6\n" << width << ' ' << height << "\n255\n";
        const auto *pixels = static_cast<const uint8_t *>(data);
        // bgfx's screenshot callback uses BGRA8 on all desktop renderers.
        for (uint32_t row = 0; row < height; ++row) {
            uint32_t y = yflip ? height - 1 - row : row;
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t *p = pixels + static_cast<size_t>(y) * pitch + x * 4;
                file.put(static_cast<char>(p[2]));
                file.put(static_cast<char>(p[1]));
                file.put(static_cast<char>(p[0]));
            }
        }
    }
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void *, uint32_t) override {}
};

struct Target {
    bgfx::FrameBufferHandle fb{Invalid};
    std::vector<bgfx::TextureHandle> textures;
    void reset() {
        release(fb);
        for (auto &t : textures)
            release(t);
        textures.clear();
    }
    bool create(uint16_t w, uint16_t h, std::initializer_list<bgfx::TextureFormat::Enum> formats) {
        reset();
        for (auto format : formats) {
            auto flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                         BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
            auto t = bgfx::createTexture2D(w, h, false, 1, format, flags);
            if (!valid(t)) {
                reset();
                return false;
            }
            textures.push_back(t);
        }
        fb = bgfx::createFrameBuffer(uint8_t(textures.size()), textures.data(), false);
        return valid(fb);
    }
};
uint64_t chunkMeshCacheKey(const VoxelGrid &grid, IVec3 chunk) {
    return grid.chunkMeshKey(chunk);
}

Mesh makeChunkMesh(const VoxelGrid &grid, IVec3 chunk, float voxelSize,
                   const std::vector<Material> &materials) {
    return prepareRenderChunk(grid, chunk, voxelSize, materials);
}

struct Geometry {
    bgfx::VertexBufferHandle vertices{Invalid};
    bgfx::IndexBufferHandle indices{Invalid};
    uint64_t revision{UINT64_MAX};
    uint32_t count{}, vertexCount{};
    Vec3 minimum{}, maximum{};
    void reset() {
        release(vertices);
        release(indices);
        count = vertexCount = 0;
        revision = UINT64_MAX;
    }
};

struct ObjectGeometry {
    std::map<IVec3, Geometry> chunks;
    uint64_t gridRevision{UINT64_MAX};
    float previousModel[16]{};
    bool history{};
    bool initialized{};
    void reset() {
        for (auto &[_, chunk] : chunks)
            chunk.reset();
        chunks.clear();
        gridRevision = UINT64_MAX;
        history = false;
        initialized = false;
    }
};
void modelMatrix(float *m, const Transform &t) {
    bx::mtxIdentity(m);
    const Vec3 x = rotate(t.rotation, {1, 0, 0}), y = rotate(t.rotation, {0, 1, 0}),
               z = rotate(t.rotation, {0, 0, 1});
    m[0] = x.x;
    m[1] = x.y;
    m[2] = x.z;
    m[4] = y.x;
    m[5] = y.y;
    m[6] = y.z;
    m[8] = z.x;
    m[9] = z.y;
    m[10] = z.z;
    m[12] = t.position.x;
    m[13] = t.position.y;
    m[14] = t.position.z;
}

bool visible(const Geometry &g, const float *model, const float *viewProjection) {
    float mvp[16];
    bx::mtxMul(mvp, model, viewProjection);
    int outside[6]{};
    bool homogeneous = bgfx::getCaps()->homogeneousDepth;
    for (int k = 0; k < 8; ++k) {
        auto p =
            multiply(mvp, {k & 1 ? g.maximum.x : g.minimum.x, k & 2 ? g.maximum.y : g.minimum.y,
                           k & 4 ? g.maximum.z : g.minimum.z, 1});
        outside[0] += p.x < -p.w;
        outside[1] += p.x > p.w;
        outside[2] += p.y < -p.w;
        outside[3] += p.y > p.w;
        outside[4] += p.z < (homogeneous ? -p.w : 0);
        outside[5] += p.z > p.w;
    }
    for (int n : outside)
        if (n == 8)
            return false;
    return true;
}
bgfx::ShaderHandle loadShader(const char *name) {
    std::string backend;
    switch (bgfx::getRendererType()) {
    case bgfx::RendererType::Metal:
        backend = "metal";
        break;
    case bgfx::RendererType::Direct3D11:
        backend = "dx11";
        break;
    case bgfx::RendererType::Vulkan:
        backend = "spirv";
        break;
    default:
        backend = "glsl";
        break;
    }
    std::vector<std::filesystem::path> roots;
    if (const char *base = SDL_GetBasePath()) {
        roots.emplace_back(std::filesystem::path(base) / "shaders");
        roots.emplace_back(std::filesystem::path(base) / "../share/aspera/shaders");
    }
    roots.emplace_back(ASPERA_SHADER_DIR);
    for (const auto &root : roots) {
        std::ifstream file(root / backend / (std::string(name) + ".bin"),
                           std::ios::binary | std::ios::ate);
        if (!file)
            continue;
        auto size = file.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024)
            continue;
        auto *mem = bgfx::alloc(uint32_t(size));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(mem->data), size);
        return bgfx::createShader(mem);
    }
    SDL_Log("Missing rendering shader: %s", name);
    return {Invalid};
}
bgfx::ProgramHandle loadProgram(const char *vs, const char *fs) {
    auto a = loadShader(vs), b = loadShader(fs);
    if (!valid(a) || !valid(b)) {
        release(a);
        release(b);
        return {Invalid};
    }
    return bgfx::createProgram(a, b, true);
}
struct FullscreenVertex {
    float x, y, z, u, v;
};
float radicalInverse(uint32_t bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
    bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
    bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
    return float(bits) * 2.3283064365386963e-10f;
}
Vec3 sampleGgx(float u, float v, float rough) {
    float a = rough * rough, phi = 6.2831853f * u;
    float z = std::sqrt((1 - v) / (1 + (a * a - 1) * v)), r = std::sqrt(std::max(0.f, 1 - z * z));
    return {r * std::cos(phi), r * std::sin(phi), z};
}
Vec3 cubeDirection(int face, float u, float v) {
    switch (face) {
    case 0:
        return normalized({1, -v, -u});
    case 1:
        return normalized({-1, -v, u});
    case 2:
        return normalized({u, 1, v});
    case 3:
        return normalized({u, -1, -v});
    case 4:
        return normalized({u, -v, 1});
    default:
        return normalized({-u, -v, -1});
    }
}
Vec3 skyRadiance(Vec3 d, const EnvironmentSettings &e) {
    float mix = std::sqrt(std::max(d.y, 0.f));
    Vec3 tint = Vec3{.78f, .83f, .9f} * (1 - mix) + e.skyColor * mix;
    Vec3 sun = normalized(e.sunDirection) * -1;
    float halo = std::pow(std::max(dot(d, sun), 0.f), 24.f);
    float horizon = 1 - std::clamp(sun.y, 0.f, 1.f);
    tint = tint + e.sunColor * (halo * (.25f + .75f * horizon));
    float x = std::clamp((d.y + .12f) / .30f, 0.f, 1.f);
    x = x * x * (3 - 2 * x);
    return tint * (e.skyIntensity / 3.14159265f * (.25f + .75f * x));
}
} // namespace
struct RasterRenderer::Impl {
    static constexpr size_t MaxMeshWorkers = 4;
    static constexpr size_t MaxQueuedMeshJobs = 256;
    struct MeshSlot {
        EntityId entity{};
        IVec3 chunk{};
        auto operator<=>(const MeshSlot &) const = default;
    };
    struct MeshRequest {
        MeshSlot slot;
        uint64_t key{};
        uint64_t generation{};
        float voxelSize{};
        std::shared_ptr<const VoxelGrid> grid;
        std::shared_ptr<const std::vector<Material>> materials;
    };
    struct MeshResult {
        MeshSlot slot;
        uint64_t key{};
        uint64_t generation{};
        Mesh mesh;
        double workerMs{};
    };
    struct MeshJob {
        MeshRequest request;
        std::future<MeshResult> result;
    };
    struct RequestedMesh {
        uint64_t key{};
        uint64_t generation{};
        auto operator<=>(const RequestedMesh &) const = default;
    };
    SDL_Window *window{};
    bool alive{}, low{}, historyReset{true}, bloomEnabled{true}, vsync{true};
    RenderDebug debug{RenderDebug::Final};
    uint16_t width{1}, height{1}, shadowSize{2048};
    CaptureCallback capture;
    bgfx::VertexLayout voxelLayout, imguiLayout, screenLayout;
    bgfx::VertexBufferHandle screenTriangle{Invalid};
    bgfx::TextureHandle font{Invalid}, palette{Invalid}, tiles{Invalid}, sky{Invalid},
        brdfLut{Invalid};
    Target gbuffer, hdr, ao, bloomA, bloomB, shadows;
    bgfx::ProgramHandle geometryProgram{Invalid}, shadowProgram{Invalid}, lightingProgram{Invalid},
        aoProgram{Invalid}, bloomProgram{Invalid}, toneProgram{Invalid}, stressProgram{Invalid},
        imguiProgram{Invalid};
    std::map<std::string, bgfx::UniformHandle> uniforms;
    std::map<EntityId, ObjectGeometry> meshes;
    std::array<Geometry, 4> avatars;
    std::map<PlayerId, PlayerState> avatarStates;
    PlayerId localPlayer{};
    void drawAvatars(bgfx::ViewId viewId, bool shadow) {
        for (const auto &[id, state] : avatarStates) {
            if (id == localPlayer || state.spawnBlocked)
                continue;
            const auto &g = avatars[(id - 1) % 4];
            float model[16], previous[16];
            Transform pose{state.position,
                           {0, std::sin(state.yaw * .5f), 0, std::cos(state.yaw * .5f)}};
            modelMatrix(model, pose);
            if (!shadow) {
                texture(0, "s_palette", palette);
                vec("u_screenParams", screen);
                bx::mtxMul(previous, model, vp);
                matrix("u_previousMvp", previous);
            }
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, g.vertices);
            bgfx::setIndexBuffer(g.indices, 0, g.count);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                           BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW);
            bgfx::submit(viewId, shadow ? shadowProgram : geometryProgram);
        }
    }
    void prepareAvatars(const World &world) {
        if (avatarStates.empty() || avatars[0].count)
            return;
        const MaterialId colors[4]{3, 4, 5, 2};
        for (size_t i = 0; i < 4; ++i) {
            VoxelGrid grid;
            for (int x = -2; x < 2; ++x)
                for (int y = 0; y < 18; ++y)
                    for (int z = -2; z < 2; ++z)
                        if (y >= 8 || x == -2 || x == 1)
                            grid.set({x, y, z}, colors[i] < world.materials.size() ? colors[i] : 1);
            auto mesh = meshGrid(grid, .1f, world.materials);
            auto &g = avatars[i];
            g.vertices = bgfx::createVertexBuffer(
                bgfx::copy(mesh.vertices.data(), uint32_t(mesh.vertices.size() * sizeof(Vertex))),
                voxelLayout);
            g.indices = bgfx::createIndexBuffer(
                bgfx::copy(mesh.indices.data(), uint32_t(mesh.indices.size() * sizeof(uint32_t))),
                BGFX_BUFFER_INDEX32);
            g.count = uint32_t(mesh.indices.size());
        }
    }
    std::deque<MeshRequest> meshQueue;
    std::vector<MeshJob> meshJobs;
    std::map<MeshSlot, RequestedMesh> requestedMeshes;
    uint64_t meshGeneration{1};
    uint32_t rebuiltChunks{}, cachedChunks{};
    double meshPrepareMs{};
    uint64_t geometryBytes{};
    VoxelGi gi;
    float view[16]{}, projection[16]{}, vp[16]{}, inverseVP[16]{}, previousVP[16]{};
    Vec3 eye{}, previousEye{};
    float screen[4]{}, shadowMatrices[15][16]{}, shadowRects[15][4]{}, shadowData[4]{};
    float lightData[64 * 5 * 4]{};
    std::vector<Light> lights;
    std::map<LightId, int> shadowSlots;
    uint64_t lastTick{}, lastRevision{UINT64_MAX};
    float voxelSize{};
    std::vector<float> paletteData;
    EnvironmentSettings skyEnvironment{};
    bool skyDirty{true};
    std::chrono::steady_clock::time_point frameStart;
    double cpuMs{};
    bgfx::UniformHandle uniform(const char *name,
                                bgfx::UniformType::Enum type = bgfx::UniformType::Vec4,
                                uint16_t count = 1) {
        auto it = uniforms.find(name);
        if (it != uniforms.end())
            return it->second;
        auto h = bgfx::createUniform(name, type, count);
        uniforms.emplace(name, h);
        return h;
    }
    void vec(const char *name, const void *value, uint16_t count = 1) {
        bgfx::setUniform(uniform(name, bgfx::UniformType::Vec4, count), value, count);
    }
    void matrix(const char *name, const float *value, uint16_t count = 1) {
        bgfx::setUniform(uniform(name, bgfx::UniformType::Mat4, count), value, count);
    }
    void texture(uint8_t stage, const char *name, bgfx::TextureHandle t,
                 uint32_t flags = UINT32_MAX) {
        bgfx::setTexture(stage, uniform(name, bgfx::UniformType::Sampler), t, flags);
    }
    void fullscreen(bgfx::ViewId id, bgfx::ProgramHandle program) {
        bgfx::setVertexBuffer(0, screenTriangle);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::submit(id, program);
    }
    void configure(bgfx::ViewId id, const char *name, const Target &target, uint16_t w, uint16_t h,
                   uint32_t color = 0) {
        bgfx::setViewName(id, name);
        bgfx::setViewRect(id, 0, 0, w, h);
        bgfx::setViewFrameBuffer(id, target.fb);
        bgfx::setViewClear(id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, color, 1.f, 0);
        bgfx::touch(id);
    }
    bool targets() {
        uint16_t halfW = std::max(1, width / 2), halfH = std::max(1, height / 2);
        using F = bgfx::TextureFormat;
        bool ok =
            gbuffer.create(width, height, {F::RGBA8, F::RGBA16F, F::RGBA16F, F::RG16F, F::D32F}) &&
            hdr.create(width, height, {F::RGBA16F}) && ao.create(halfW, halfH, {F::R8}) &&
            bloomA.create(halfW, halfH, {F::RGBA16F}) &&
            bloomB.create(halfW, halfH, {F::RGBA16F}) &&
            shadows.create(shadowSize * 2, shadowSize * 2, {F::R32F, F::D32F});
        release(tiles);
        tiles = bgfx::createTexture2D(((width + 15) / 16) * 17, (height + 15) / 16, false, 1,
                                      F::RGBA32F,
                                      BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                          BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        gi.resize(width, height, low);
        historyReset = true;
        return ok && valid(tiles);
    }
    void resetMeshCaches() {
        for (auto &[_, object] : meshes)
            object.reset();
        meshes.clear();
        meshQueue.clear();
        requestedMeshes.clear();
        ++meshGeneration;
        if (!meshGeneration)
            meshGeneration = 1;
        rebuiltChunks = 0;
        cachedChunks = 0;
        meshPrepareMs = 0;
        geometryBytes = 0;
        historyReset = true;
    }
    void pollMeshJobs(const World &world) {
        for (size_t i = 0; i < meshJobs.size();) {
            auto &job = meshJobs[i];
            if (job.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++i;
                continue;
            }
            MeshRequest request = std::move(job.request);
            std::optional<MeshResult> result;
            try {
                result = job.result.get();
                if (result)
                    meshPrepareMs += result->workerMs;
            } catch (const std::exception &error) {
                SDL_Log("Chunk mesh preparation failed for entity %llu: %s",
                        static_cast<unsigned long long>(request.slot.entity), error.what());
            } catch (...) {
                SDL_Log("Chunk mesh preparation failed for entity %llu",
                        static_cast<unsigned long long>(request.slot.entity));
            }
            meshJobs.erase(meshJobs.begin() + static_cast<ptrdiff_t>(i));

            auto requested = requestedMeshes.find(request.slot);
            if (requested == requestedMeshes.end() || requested->second.key != request.key ||
                requested->second.generation != request.generation)
                continue; // A newer edit or scene has superseded this result.

            auto object = world.objects.find(request.slot.entity);
            if (object == world.objects.end()) {
                requestedMeshes.erase(requested);
                continue;
            }
            auto currentKey = chunkMeshCacheKey(object->second.grid, request.slot.chunk);
            if (!object->second.grid.chunks().contains(request.slot.chunk) ||
                currentKey != request.key || request.generation != meshGeneration) {
                requestedMeshes.erase(requested);
                continue;
            }

            auto &cache = meshes[request.slot.entity];
            auto &geometry = cache.chunks[request.slot.chunk];
            if (!result) {
                // Do not spin on a deterministic preparation error every frame.
                // Keep any previously published geometry visible and mark this
                // key as attempted until the grid changes again.
                geometry.revision = request.key;
                requestedMeshes.erase(requested);
                continue;
            }
            geometry.reset();
            geometry.revision = result->key;
            if (!result->mesh.vertices.empty() && !result->mesh.indices.empty()) {
                geometry.minimum =
                    geometry.maximum = {result->mesh.vertices[0].x, result->mesh.vertices[0].y,
                                        result->mesh.vertices[0].z};
                for (const auto &vertex : result->mesh.vertices) {
                    geometry.minimum = {std::min(geometry.minimum.x, vertex.x),
                                        std::min(geometry.minimum.y, vertex.y),
                                        std::min(geometry.minimum.z, vertex.z)};
                    geometry.maximum = {std::max(geometry.maximum.x, vertex.x),
                                        std::max(geometry.maximum.y, vertex.y),
                                        std::max(geometry.maximum.z, vertex.z)};
                }
                if (result->mesh.vertices.size() > UINT32_MAX / sizeof(Vertex) ||
                    result->mesh.indices.size() > UINT32_MAX / sizeof(uint32_t) ||
                    result->mesh.indices.size() > UINT32_MAX)
                    SDL_Log("Chunk mesh exceeds GPU index range for entity %llu",
                            static_cast<unsigned long long>(request.slot.entity));
                else {
                    geometry.vertices = bgfx::createVertexBuffer(
                        bgfx::copy(result->mesh.vertices.data(),
                                   uint32_t(result->mesh.vertices.size() * sizeof(Vertex))),
                        voxelLayout);
                    geometry.indices = bgfx::createIndexBuffer(
                        bgfx::copy(result->mesh.indices.data(),
                                   uint32_t(result->mesh.indices.size() * sizeof(uint32_t))),
                        BGFX_BUFFER_INDEX32);
                    geometry.count = uint32_t(result->mesh.indices.size());
                    geometry.vertexCount = uint32_t(result->mesh.vertices.size());
                    if (!valid(geometry.vertices) || !valid(geometry.indices)) {
                        geometry.reset();
                        geometry.revision = result->key;
                    }
                }
            }
            ++rebuiltChunks;
            historyReset = true;
            requestedMeshes.erase(requested);
        }
    }
    void launchMeshJobs() {
        while (meshJobs.size() < MaxMeshWorkers && !meshQueue.empty()) {
            MeshRequest request = std::move(meshQueue.front());
            meshQueue.pop_front();
            auto requested = requestedMeshes.find(request.slot);
            if (requested == requestedMeshes.end() || requested->second.key != request.key ||
                requested->second.generation != request.generation)
                continue;
            MeshJob job;
            job.request = request;
            try {
                job.result = std::async(std::launch::async, [request = std::move(request)] {
                    auto started = std::chrono::steady_clock::now();
                    Mesh mesh = makeChunkMesh(*request.grid, request.slot.chunk, request.voxelSize,
                                              *request.materials);
                    const double elapsed = std::chrono::duration<double, std::milli>(
                                               std::chrono::steady_clock::now() - started)
                                               .count();
                    return MeshResult{request.slot, request.key, request.generation,
                                      std::move(mesh), elapsed};
                });
            } catch (const std::exception &error) {
                SDL_Log("Could not start chunk mesh worker: %s", error.what());
                meshQueue.push_front(std::move(job.request));
                break;
            }
            meshJobs.push_back(std::move(job));
        }
    }
    void queueMissingMeshes(EntityId id, const Object &object, const World &world) {
        auto &cache = meshes[id];
        std::vector<IVec3> needed;
        needed.reserve(object.grid.chunks().size());
        for (auto it = cache.chunks.begin(); it != cache.chunks.end();) {
            if (!object.grid.chunks().contains(it->first)) {
                it->second.reset();
                it = cache.chunks.erase(it);
                historyReset = true;
            } else
                ++it;
        }
        for (const auto &[chunk, _] : object.grid.chunks()) {
            const uint64_t key = chunkMeshCacheKey(object.grid, chunk);
            auto &geometry = cache.chunks[chunk];
            if (geometry.revision == key)
                continue;
            MeshSlot slot{id, chunk};
            auto requested = requestedMeshes.find(slot);
            if (requested != requestedMeshes.end() && requested->second.key == key &&
                requested->second.generation == meshGeneration)
                continue;
            needed.push_back(chunk);
        }
        const size_t meshWork = meshQueue.size() + meshJobs.size();
        if (!needed.empty() && meshWork < MaxQueuedMeshJobs) {
            auto grid = std::make_shared<VoxelGrid>(object.grid);
            auto materials = std::make_shared<std::vector<Material>>(world.materials);
            for (auto chunk : needed) {
                if (meshQueue.size() + meshJobs.size() >= MaxQueuedMeshJobs)
                    break;
                MeshSlot slot{id, chunk};
                const uint64_t key = chunkMeshCacheKey(object.grid, chunk);
                requestedMeshes[slot] = {key, meshGeneration};
                meshQueue.push_back({slot, key, meshGeneration, voxelSize, grid, materials});
            }
        }
        cache.gridRevision = object.grid.contentKey();
        cache.initialized = true;
    }
    void updateGeometryBytes() {
        cachedChunks = 0;
        geometryBytes = 0;
        for (const auto &[_, object] : meshes)
            for (const auto &[__, geometry] : object.chunks)
                if (geometry.count) {
                    ++cachedChunks;
                    geometryBytes += uint64_t(geometry.vertexCount) * sizeof(Vertex) +
                                     uint64_t(geometry.count) * sizeof(uint32_t);
                }
    }
    void prepare(const World &world) {
        auto meshStart = std::chrono::steady_clock::now();
        meshPrepareMs = 0;
        if (world.tick < lastTick || world.revision < lastRevision || voxelSize != world.voxelSize)
            resetMeshCaches();
        voxelSize = world.voxelSize;
        lastTick = world.tick;
        lastRevision = world.revision;
        rebuiltChunks = 0;
        pollMeshJobs(world);
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (!world.objects.contains(it->first)) {
                it->second.reset();
                it = meshes.erase(it);
                historyReset = true;
            } else
                ++it;
        }
        for (const auto &[id, object] : world.objects)
            queueMissingMeshes(id, object, world);
        launchMeshJobs();
        updateGeometryBytes();
        meshPrepareMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - meshStart)
                .count();
        std::vector<float> values(4096 * 3 * 4, 0);
        for (size_t i = 0; i < std::min(size_t(4096), world.materials.size()); ++i) {
            const auto &m = world.materials[i];
            size_t at = i * 4;
            values[at] = srgb(float((m.rgba >> 24) & 255) / 255);
            values[at + 1] = srgb(float((m.rgba >> 16) & 255) / 255);
            values[at + 2] = srgb(float((m.rgba >> 8) & 255) / 255);
            values[at + 3] = m.metallic;
            at += 4096 * 4;
            values[at] = m.emissive.x;
            values[at + 1] = m.emissive.y;
            values[at + 2] = m.emissive.z;
            values[at + 3] = m.emissionIntensity;
            values[at + 4096 * 4] = m.roughness;
        }
        if (values != paletteData) {
            paletteData = std::move(values);
            bgfx::updateTexture2D(
                palette, 0, 0, 0, 0, 4096, 3,
                bgfx::copy(paletteData.data(), uint32_t(paletteData.size() * sizeof(float))));
            historyReset = true;
        }
    }
    void makeBrdf() {
        constexpr int size = 128, samples = 128;
        std::vector<float> data(size * size * 2);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                float nv = (x + .5f) / size, rough = (y + .5f) / size;
                Vec3 v{std::sqrt(1 - nv * nv), 0, nv};
                float A = 0, B = 0;
                for (int k = 0; k < samples; ++k) {
                    Vec3 h = sampleGgx(float(k) / samples, radicalInverse(k), rough);
                    Vec3 l = h * (2 * dot(v, h)) - v;
                    float nl = l.z, nh = h.z, vh = std::max(dot(v, h), 0.f);
                    if (nl <= 0)
                        continue;
                    float a = rough * rough;
                    float gv = nl * std::sqrt(nv * nv * (1 - a * a) + a * a);
                    float gl = nv * std::sqrt(nl * nl * (1 - a * a) + a * a);
                    float vis = 2 * nl * vh / std::max((gv + gl) * nh, 1e-6f),
                          Fc = std::pow(1 - vh, 5.f);
                    A += (1 - Fc) * vis;
                    B += Fc * vis;
                }
                data[(y * size + x) * 2] = A / samples;
                data[(y * size + x) * 2 + 1] = B / samples;
            }
        brdfLut = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RG32F,
                                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                        bgfx::copy(data.data(), uint32_t(data.size() * 4)));
    }
    void updateSky(const EnvironmentSettings &e) {
        if (!skyDirty && e.skyColor == skyEnvironment.skyColor &&
            e.skyIntensity == skyEnvironment.skyIntensity &&
            e.sunDirection == skyEnvironment.sunDirection && e.sunColor == skyEnvironment.sunColor)
            return;
        skyEnvironment = e;
        skyDirty = false;
        for (int face = 0; face < 6; ++face)
            for (int mip = 0; mip < 8; ++mip) {
                int size = 128 >> mip;
                float rough = float(mip) / 7;
                std::vector<float> pixels(size * size * 4);
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x) {
                        Vec3 n = cubeDirection(face, 2 * (x + .5f) / size - 1,
                                               2 * (y + .5f) / size - 1),
                             color{};
                        float weights = 0;
                        Vec3 tangent = normalized(
                                 cross(std::abs(n.z) < .99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, n)),
                             bitangent = cross(n, tangent);
                        int samples = mip ? 32 : 1;
                        for (int s = 0; s < samples; ++s) {
                            Vec3 h = sampleGgx(float(s) / samples, radicalInverse(s), rough);
                            h = tangent * h.x + bitangent * h.y + n * h.z;
                            Vec3 l = h * (2 * dot(n, h)) - n;
                            float weight = std::max(dot(n, l), 0.f);
                            color = color + skyRadiance(l, e) * weight;
                            weights += weight;
                        }
                        color = color / std::max(weights, .001f);
                        size_t i = (y * size + x) * 4;
                        pixels[i] = color.x;
                        pixels[i + 1] = color.y;
                        pixels[i + 2] = color.z;
                        pixels[i + 3] = 1;
                    }
                bgfx::updateTextureCube(sky, 0, uint8_t(face), uint8_t(mip), 0, 0, uint16_t(size),
                                        uint16_t(size),
                                        bgfx::copy(pixels.data(), uint32_t(pixels.size() * 4)));
            }
    }
    void shadowPass(int slot, const World &world, const float *shadowView,
                    const float *shadowProjection, uint16_t x, uint16_t y, uint16_t size) {
        float lightVP[16];
        bx::mtxMul(lightVP, shadowView, shadowProjection);
        float scale = 1.f / (shadowSize * 2), sy = bgfx::getCaps()->originBottomLeft ? 1.f : -1.f;
        float bias[16];
        bx::mtxIdentity(bias);
        bias[0] = .5f * size * scale;
        bias[5] = sy * .5f * size * scale;
        bias[10] = bgfx::getCaps()->homogeneousDepth ? .5f : 1.f;
        bias[12] = (x + size * .5f) * scale;
        bias[13] = (y + size * .5f) * scale;
        bias[14] = bgfx::getCaps()->homogeneousDepth ? .5f : 0;
        bx::mtxMul(shadowMatrices[slot], lightVP, bias);
        shadowRects[slot][0] = x * scale;
        shadowRects[slot][1] = y * scale;
        shadowRects[slot][2] = (x + size) * scale;
        shadowRects[slot][3] = (y + size) * scale;
        auto id = bgfx::ViewId(slot);
        bgfx::setViewName(id, slot < 3 ? "Sun shadow" : "Local shadow");
        bgfx::setViewRect(id, x, y, size, size);
        bgfx::setViewFrameBuffer(id, shadows.fb);
        bgfx::setViewTransform(id, shadowView, shadowProjection);
        bgfx::setViewClear(id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1, 0);
        bgfx::touch(id);
        for (const auto &[entity, o] : world.objects) {
            auto meshObject = meshes.find(entity);
            if (meshObject == meshes.end())
                continue;
            float m[16];
            modelMatrix(m, o.transform);
            for (const auto &[_, g] : meshObject->second.chunks) {
                if (!valid(g.vertices) || !g.count || !visible(g, m, lightVP))
                    continue;
                bgfx::setTransform(m);
                bgfx::setVertexBuffer(0, g.vertices);
                bgfx::setIndexBuffer(g.indices, 0, g.count);
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z |
                               BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW);
                bgfx::submit(id, shadowProgram);
            }
        }
        drawAvatars(id, true);
    }
    void drawShadows(const World &world) {
        Vec3 direction = normalized(world.environment.sunDirection);
        float inverseView[16];
        bx::mtxInverse(inverseView, view);
        const float splits[]{.05f, 8.f, 24.f, 64.f};
        for (int i = 0; i < 3; ++i) {
            Vec3 center{};
            std::array<Vec3, 8> corners;
            for (int k = 0; k < 8; ++k) {
                float z = splits[i + (k & 4 ? 1 : 0)], y = z * std::tan(radians(30)),
                      x = y * float(width) / height;
                auto q = multiply(inverseView, {k & 1 ? x : -x, k & 2 ? y : -y, z, 1});
                corners[k] = {q.x, q.y, q.z};
                center = center + corners[k] / 8;
            }
            float radius = 0;
            for (auto c : corners)
                radius = std::max(radius, length(c - center));
            radius = std::ceil(radius * 16) / 16;
            Vec3 lightEye = center - direction * (radius + 64);
            float lv[16], lp[16];
            Vec3 up = std::abs(direction.y) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            bx::mtxLookAt(lv, {lightEye.x, lightEye.y, lightEye.z}, {center.x, center.y, center.z},
                          {up.x, up.y, up.z});
            float texel = 2 * radius / shadowSize;
            lv[12] = std::floor(lv[12] / texel) * texel;
            lv[13] = std::floor(lv[13] / texel) * texel;
            bx::mtxOrtho(lp, -radius, radius, -radius, radius, 0, 2 * (radius + 64), 0,
                         bgfx::getCaps()->homogeneousDepth);
            shadowPass(i, world, lv, lp, uint16_t((i % 2) * shadowSize),
                       uint16_t((i / 2) * shadowSize), shadowSize);
        }
        shadowData[0] = splits[1];
        shadowData[1] = splits[2];
        shadowData[2] = splits[3];
        shadowData[3] = 1.f / (shadowSize * 2);
        shadowSlots.clear();
        if (low)
            return;
        std::vector<Light> candidates;
        for (auto &[_, light] : world.lights)
            if (light.shadows && light.intensity > 0)
                candidates.push_back(light);
        std::sort(candidates.begin(), candidates.end(), [](const Light &a, const Light &b) {
            return a.shadowPriority != b.shadowPriority ? a.shadowPriority > b.shadowPriority
                                                        : a.id < b.id;
        });
        int tile = 0;
        const Vec3 directions[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        const Vec3 ups[] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
        for (size_t i = 0; i < std::min(size_t(2), candidates.size()); ++i) {
            auto &light = candidates[i];
            shadowSlots[light.id] = 3 + tile;
            int faces = light.type == LightType::Point ? 6 : 1;
            for (int face = 0; face < faces; ++face) {
                Vec3 d = faces == 6 ? directions[face] : normalized(light.direction),
                     up = faces == 6 ? ups[face]
                                     : (std::abs(d.y) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
                Vec3 target = light.position + d;
                float lv[16], lp[16];
                bx::mtxLookAt(lv, {light.position.x, light.position.y, light.position.z},
                              {target.x, target.y, target.z}, {up.x, up.y, up.z});
                bx::mtxProj(lp, faces == 6 ? 90.f : light.outerAngle * 2, 1, .02f, light.range,
                            bgfx::getCaps()->homogeneousDepth);
                shadowPass(3 + tile, world, lv, lp, shadowSize + (tile % 4) * 512,
                           shadowSize + (tile / 4) * 512, 512);
                ++tile;
            }
        }
    }
    void updateLights(const World &world) {
        std::fill(std::begin(lightData), std::end(lightData), 0.f);
        lights.clear();
        for (auto &[_, light] : world.lights)
            if (lights.size() < 64)
                lights.push_back(light);
        int tx = (width + 15) / 16, ty = (height + 15) / 16;
        std::vector<float> list(size_t(tx) * ty * 68, 0);
        std::vector<int> counts(tx * ty, 0);
        for (size_t i = 0; i < lights.size(); ++i) {
            const auto &l = lights[i];
            float *p = lightData + i * 20;
            p[0] = l.position.x;
            p[1] = l.position.y;
            p[2] = l.position.z;
            p[3] = l.range;
            p[4] = l.color.x;
            p[5] = l.color.y;
            p[6] = l.color.z;
            float solidAngle = l.type == LightType::Point
                                   ? 12.5663706f
                                   : 2 * 3.14159265f * (1 - std::cos(radians(l.outerAngle)));
            p[7] = l.intensity / std::max(solidAngle, .01f);
            Vec3 dir = normalized(l.direction);
            p[8] = dir.x;
            p[9] = dir.y;
            p[10] = dir.z;
            p[11] = l.type == LightType::Spot ? 1 : 0;
            p[12] = std::cos(radians(l.innerAngle));
            p[13] = std::cos(radians(l.outerAngle));
            p[16] = shadowSlots.contains(l.id) ? float(shadowSlots.at(l.id)) : -1.f;
            V4 center = multiply(view, {l.position.x, l.position.y, l.position.z, 1});
            if (center.z + l.range < .05f)
                continue;
            int x0 = 0, x1 = tx - 1, y0 = 0, y1 = ty - 1;
            if (center.z > l.range + .05f) {
                // Project the containing view-space box. A radius-only screen estimate can
                // underestimate off-axis lights and remove illumination at tile boundaries.
                float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
                for (int corner = 0; corner < 8; ++corner) {
                    float z = center.z + (corner & 4 ? l.range : -l.range);
                    float x = (center.x + (corner & 1 ? l.range : -l.range)) / z * projection[0];
                    float y = (center.y + (corner & 2 ? l.range : -l.range)) / z * projection[5] *
                              screen[2];
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
                x0 = int(std::floor((std::clamp(minX, -1.f, 1.f) + 1) * .5f * (width - 1) / 16));
                x1 = int(std::floor((std::clamp(maxX, -1.f, 1.f) + 1) * .5f * (width - 1) / 16));
                y0 = int(std::floor((std::clamp(minY, -1.f, 1.f) + 1) * .5f * (height - 1) / 16));
                y1 = int(std::floor((std::clamp(maxY, -1.f, 1.f) + 1) * .5f * (height - 1) / 16));
            }
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    size_t at = (size_t(y) * tx + x) * 68;
                    int count = counts[y * tx + x]++;
                    list[at + 4 + count] = float(i);
                    list[at] = float(count + 1);
                }
        }
        bgfx::updateTexture2D(tiles, 0, 0, 0, 0, tx * 17, ty,
                              bgfx::copy(list.data(), uint32_t(list.size() * 4)));
    }
    void drawGeometry(const World &world) {
        configure(WorldView, "G-buffer", gbuffer, width, height);
        bgfx::setViewTransform(WorldView, view, projection);
        for (auto &[id, object] : world.objects) {
            auto meshObject = meshes.find(id);
            if (meshObject == meshes.end())
                continue;
            float model[16];
            modelMatrix(model, object.transform);
            float previous[16];
            auto &cache = meshObject->second;
            bx::mtxMul(previous, cache.history && !historyReset ? cache.previousModel : model,
                       cache.history && !historyReset ? previousVP : vp);
            bool submitted = false;
            for (const auto &[_, g] : cache.chunks) {
                if (!valid(g.vertices) || !g.count || !visible(g, model, vp))
                    continue;
                texture(0, "s_palette", palette);
                vec("u_screenParams", screen);
                matrix("u_previousMvp", previous);
                bgfx::setTransform(model);
                bgfx::setVertexBuffer(0, g.vertices);
                bgfx::setIndexBuffer(g.indices, 0, g.count);
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                               BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW);
                bgfx::submit(WorldView, geometryProgram);
                submitted = true;
            }
            std::memcpy(cache.previousModel, model, sizeof(model));
            cache.history = submitted;
        }
    }
    void illuminate(const World &world) {
        GiDirectLighting direct{shadows.textures[0],
                                &shadowMatrices[0][0],
                                &shadowRects[0][0],
                                shadowData,
                                lightData,
                                view,
                                uint32_t(lights.size())};
        gi.render(world, eye, inverseVP, previousVP, gbuffer.textures[4], gbuffer.textures[1],
                  gbuffer.textures[3], palette, direct, historyReset, int(debug) == 6);
        configure(AoView, "Contact AO", ao, std::max(1, width / 2), std::max(1, height / 2),
                  0xffffffff);
        texture(0, "s_depth", gbuffer.textures[4]);
        texture(1, "s_normal", gbuffer.textures[1]);
        matrix("u_inverseVP", inverseVP);
        vec("u_screenParams", screen);
        float params[] = {20.f, .5f, 0, 0};
        vec("u_aoParams", params);
        fullscreen(AoView, aoProgram);
        configure(LightView, "PBR lighting", hdr, width, height);
        texture(0, "s_base", gbuffer.textures[0]);
        texture(1, "s_normal", gbuffer.textures[1]);
        texture(2, "s_depth", gbuffer.textures[4]);
        texture(3, "s_emission", gbuffer.textures[2]);
        texture(4, "s_shadow", shadows.textures[0]);
        texture(5, "s_indirect", gi.texture(), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        texture(6, "s_ao", ao.textures[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        texture(7, "s_tiles", tiles);
        texture(8, "s_sky", sky);
        texture(9, "s_brdfLut", brdfLut);
        texture(10, "s_voxelDebug", gi.visibilityTexture());
        matrix("u_inverseVP", inverseVP);
        matrix("u_cameraView", view);
        matrix("u_shadowMatrices", &shadowMatrices[0][0], 15);
        vec("u_shadowRects", shadowRects, 15);
        vec("u_lightData", lightData, 320);
        Vec3 d = normalized(world.environment.sunDirection);
        float direction[] = {d.x, d.y, d.z, 0};
        const auto &e = world.environment;
        float sun[] = {e.sunColor.x, e.sunColor.y, e.sunColor.z, e.sunIntensity},
              skyColor[] = {e.skyColor.x, e.skyColor.y, e.skyColor.z, e.skyIntensity},
              position[] = {eye.x, eye.y, eye.z, world.voxelSize},
              render[] = {float(debug), 0, 0, 0};
        vec("u_sunDirection", direction);
        vec("u_sunColor", sun);
        vec("u_skyColor", skyColor);
        vec("u_eye", position);
        vec("u_screenParams", screen);
        vec("u_shadowParams", shadowData);
        vec("u_renderParams", render);
        fullscreen(LightView, lightingProgram);
        float exposure = 1 / (1.2f * std::exp2(e.exposure));
        if (!low && bloomEnabled && e.bloomStrength > 0) {
            configure(BloomView, "Bloom horizontal", bloomA, std::max(1, width / 2),
                      std::max(1, height / 2));
            texture(0, "s_source", hdr.textures[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            float a[] = {2.f / width, 0, 1 / exposure, 0};
            vec("u_postParams", a);
            fullscreen(BloomView, bloomProgram);
            configure(BloomView + 1, "Bloom vertical", bloomB, std::max(1, width / 2),
                      std::max(1, height / 2));
            texture(0, "s_source", bloomA.textures[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            float b[] = {0, 2.f / height, 0, 0};
            vec("u_postParams", b);
            fullscreen(BloomView + 1, bloomProgram);
        } else
            configure(BloomView + 1, "Bloom disabled", bloomB, std::max(1, width / 2),
                      std::max(1, height / 2));
        bgfx::setViewName(ToneView, "Tone mapping");
        bgfx::setViewRect(ToneView, 0, 0, width, height);
        bgfx::setViewFrameBuffer(ToneView, BGFX_INVALID_HANDLE);
        bgfx::setViewClear(ToneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, uint32_t(0), 1.f, 0);
        texture(0, "s_source", hdr.textures[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        texture(1, "s_bloom", bloomB.textures[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        float post[] = {exposure, (!low && bloomEnabled) ? e.bloomStrength : 0.f, 1.f / width,
                        1.f / height};
        vec("u_postParams", post);
        fullscreen(ToneView, toneProgram);
        historyReset = false;
        std::memcpy(previousVP, vp, sizeof(vp));
        previousEye = eye;
    }
    void drawStress(const World &world) {
        bgfx::setViewName(StressView, "Stress overlay");
        bgfx::setViewRect(StressView, 0, 0, width, height);
        bgfx::setViewTransform(StressView, view, projection);
        size_t remaining = 4096;
        constexpr uint16_t edges[] = {0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7,
                                      7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7};
        for (auto &[_, o] : world.objects) {
            uint32_t count = uint32_t(std::min(remaining, o.stressRegions.size()));
            if (!count)
                continue;
            if (bgfx::getAvailTransientVertexBuffer(count * 8, voxelLayout) < count * 8 ||
                bgfx::getAvailTransientIndexBuffer(count * 24) < count * 24)
                break;
            bgfx::TransientVertexBuffer vb;
            bgfx::TransientIndexBuffer ib;
            bgfx::allocTransientVertexBuffer(&vb, count * 8, voxelLayout);
            bgfx::allocTransientIndexBuffer(&ib, count * 24);
            auto *v = reinterpret_cast<Vertex *>(vb.data);
            auto *ix = reinterpret_cast<uint16_t *>(ib.data);
            for (uint32_t i = 0; i < count; ++i) {
                auto &r = o.stressRegions[i];
                for (int k = 0; k < 8; ++k)
                    v[i * 8 + k] = {k & 1 ? r.maximum.x : r.minimum.x,
                                    k & 2 ? r.maximum.y : r.minimum.y,
                                    k & 4 ? r.maximum.z : r.minimum.z,
                                    0,
                                    1,
                                    0,
                                    stressColor(r.utilization),
                                    0};
                for (int k = 0; k < 24; ++k)
                    ix[i * 24 + k] = uint16_t(i * 8 + edges[k]);
            }
            float model[16];
            modelMatrix(model, o.transform);
            float mode[] = {0, 1, 0, 0}, light[] = {0, -1, 0, 0};
            vec("u_stressTint", mode);
            vec("u_lightDir", light);
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, &vb);
            bgfx::setIndexBuffer(&ib);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_LINES);
            bgfx::submit(StressView, stressProgram);
            remaining -= count;
        }
    }
};

RasterRenderer::RasterRenderer() : p(std::make_unique<Impl>()) {}
RasterRenderer::~RasterRenderer() {
    shutdown();
}
bool RasterRenderer::init(SDL_Window *window, uint32_t width, uint32_t height) {
    p->window = window;
    p->width = uint16_t(std::clamp(width, 1u, 65535u));
    p->height = uint16_t(std::clamp(height, 1u, 65535u));
    bgfx::Init init;
#if defined(__APPLE__)
    init.type = bgfx::RendererType::Metal;
#elif defined(_WIN32)
    init.type = bgfx::RendererType::Direct3D11;
#else
    init.type = bgfx::RendererType::Vulkan;
#endif
    init.swapChain.width = width;
    init.swapChain.height = height;
    init.reset = BGFX_RESET_VSYNC;
    init.callback = &p->capture;
    init.profile = true;
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if defined(__APPLE__)
    init.swapChain.nwh =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(_WIN32)
    init.swapChain.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    // Vulkan receives the native pair supplied by SDL, for both X11 and Wayland.
    if (const char *driver = SDL_GetCurrentVideoDriver();
        driver && std::strcmp(driver, "wayland") == 0) {
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.swapChain.nwh =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        init.swapChain.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    } else {
        init.swapChain.nwh = reinterpret_cast<void *>(static_cast<uintptr_t>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
        init.swapChain.ndt =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    }
#endif
    bgfx::renderFrame();
    if (!bgfx::init(init))
        return false;
    p->alive = true;

    const auto *caps = bgfx::getCaps();
    const uint64_t required = BGFX_CAPS_COMPUTE | BGFX_CAPS_IMAGE_RW;
    if ((caps->supported & required) != required || caps->limits.maxFBAttachments < 5 ||
        caps->limits.maxTextureSamplers < 11) {
        SDL_Log("Renderer requires compute, 3D images, five framebuffer attachments, and eleven "
                "samplers");
        return false;
    }
    for (auto format :
         {bgfx::TextureFormat::RGBA8, bgfx::TextureFormat::RGBA16F, bgfx::TextureFormat::RG16F,
          bgfx::TextureFormat::R32F, bgfx::TextureFormat::D32F}) {
        if (!bgfx::isTextureValid(0, false, 1, format, BGFX_TEXTURE_RT)) {
            SDL_Log("Unsupported rendering target format: %d", int(format));
            return false;
        }
    }
    p->voxelLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord1, 1, bgfx::AttribType::Float)
        .end();
    p->imguiLayout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    p->screenLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    float bottom = caps->originBottomLeft ? 0.f : 1.f, top = caps->originBottomLeft ? 2.f : -1.f;
    FullscreenVertex triangle[] = {
        {-1, -1, 0, 0, bottom}, {3, -1, 0, 2, bottom}, {-1, 3, 0, 0, top}};
    p->screenTriangle =
        bgfx::createVertexBuffer(bgfx::copy(triangle, sizeof(triangle)), p->screenLayout);
    bgfx::setDebug(BGFX_DEBUG_PROFILER);
    p->geometryProgram = loadProgram("vs_gbuffer", "fs_gbuffer");
    p->shadowProgram = loadProgram("vs_shadow", "fs_shadow");
    p->lightingProgram = loadProgram("vs_fullscreen", "fs_lighting");
    p->aoProgram = loadProgram("vs_fullscreen", "fs_ao");
    p->bloomProgram = loadProgram("vs_fullscreen", "fs_bloom");
    p->toneProgram = loadProgram("vs_fullscreen", "fs_tonemap");
    p->stressProgram = loadProgram("vs_voxel", "fs_voxel");
    p->imguiProgram = loadProgram("vs_imgui", "fs_imgui");
    for (auto h : {p->geometryProgram, p->shadowProgram, p->lightingProgram, p->aoProgram,
                   p->bloomProgram, p->toneProgram, p->stressProgram, p->imguiProgram})
        if (!valid(h))
            return false;
    p->palette = bgfx::createTexture2D(4096, 3, false, 1, bgfx::TextureFormat::RGBA32F,
                                       BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                           BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    p->sky =
        bgfx::createTextureCube(128, true, 1, bgfx::TextureFormat::RGBA32F,
                                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
    p->makeBrdf();
    unsigned char *pixels = nullptr;
    int fw = 0, fh = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    if (pixels && fw && fh) {
        p->font =
            bgfx::createTexture2D(uint16_t(fw), uint16_t(fh), false, 1, bgfx::TextureFormat::RGBA8,
                                  BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                                  bgfx::copy(pixels, uint32_t(fw * fh * 4)));
        ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(p->font.idx));
    }
    if (!p->gi.init()) {
        SDL_Log("Voxel GI initialization failed");
        return false;
    }
    bx::mtxIdentity(p->previousVP);
    return valid(p->palette) && valid(p->sky) && valid(p->brdfLut) && valid(p->font) &&
           p->targets();
}
void RasterRenderer::shutdown() {
    if (!p || !p->alive)
        return;
    // Worker jobs only build CPU meshes, but drain them before releasing the
    // renderer state so their captured voxel snapshots cannot outlive a
    // shutdown/reinitialization cycle.
    p->meshQueue.clear();
    p->requestedMeshes.clear();
    p->meshJobs.clear();
    p->gi.shutdown();
    for (auto &[_, g] : p->meshes)
        g.reset();
    p->meshes.clear();
    for (auto &g : p->avatars)
        g.reset();
    p->avatarStates.clear();
    p->gbuffer.reset();
    p->hdr.reset();
    p->ao.reset();
    p->bloomA.reset();
    p->bloomB.reset();
    p->shadows.reset();
    release(p->palette);
    release(p->tiles);
    release(p->font);
    release(p->sky);
    release(p->brdfLut);
    release(p->screenTriangle);
    release(p->geometryProgram);
    release(p->shadowProgram);
    release(p->lightingProgram);
    release(p->aoProgram);
    release(p->bloomProgram);
    release(p->toneProgram);
    release(p->stressProgram);
    release(p->imguiProgram);
    for (auto &[_, h] : p->uniforms)
        release(h);
    p->uniforms.clear();
    bgfx::shutdown();
    p->alive = false;
}
void RasterRenderer::resize(uint32_t width, uint32_t height) {
    p->width = uint16_t(std::clamp(width, 1u, 65535u));
    p->height = uint16_t(std::clamp(height, 1u, 65535u));
    bgfx::SwapChain chain;
    chain.width = p->width;
    chain.height = p->height;
    bgfx::reset(p->vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE, &chain);
    if (!p->targets())
        throw std::runtime_error("Could not recreate rendering targets");
}
void RasterRenderer::begin(Vec3 eye, const float *view) {
    p->frameStart = std::chrono::steady_clock::now();
    p->eye = eye;
    std::memcpy(p->view, view, 16 * sizeof(float));
    bx::mtxProj(p->projection, 60.f, float(p->width) / p->height, .05f, 1000.f,
                bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(p->vp, p->view, p->projection);
    bx::mtxInverse(p->inverseVP, p->vp);
    p->screen[0] = p->width;
    p->screen[1] = p->height;
    p->screen[2] = bgfx::getCaps()->originBottomLeft ? 1.f : -1.f;
    p->screen[3] = bgfx::getCaps()->homogeneousDepth ? 2.f : 1.f;
    if (length(eye - p->previousEye) > 4)
        p->historyReset = true;
}
void RasterRenderer::drawWorld(const World &world, bool stress,
                         const std::map<PlayerId, PlayerState> &players, PlayerId localPlayer) {
    p->avatarStates = players;
    p->localPlayer = localPlayer;
    p->prepareAvatars(world);
    p->prepare(world);
    p->updateSky(world.environment);
    p->drawShadows(world);
    p->updateLights(world);
    p->drawGeometry(world);
    p->drawAvatars(WorldView, false);
    p->illuminate(world);
    if (stress)
        p->drawStress(world);
}
void RasterRenderer::invalidateMeshes() {
    p->resetMeshCaches();
    p->gi.invalidate();
}
void RasterRenderer::setQuality(RenderQuality value) {
    bool low = value == RenderQuality::Low;
    if (p->low == low)
        return;
    p->low = low;
    p->shadowSize = low ? 1024 : 2048;
    if (p->alive && !p->targets())
        throw std::runtime_error("Could not change lighting quality");
}
RenderQuality RasterRenderer::quality() const {
    return p->low ? RenderQuality::Low : RenderQuality::Balanced;
}
void RasterRenderer::setDebug(RenderDebug d) {
    p->debug = d;
}
RenderDebug RasterRenderer::debug() const {
    return p->debug;
}
void RasterRenderer::setBloom(bool enabled) {
    p->bloomEnabled = enabled;
}
void RasterRenderer::setVsync(bool value) {
    p->vsync = value;
    if (p->alive) {
        bgfx::SwapChain chain;
        chain.width = p->width;
        chain.height = p->height;
        bgfx::reset(value ? BGFX_RESET_VSYNC : BGFX_RESET_NONE, &chain);
    }
}
RenderTimings RasterRenderer::timings() const {
    RenderTimings result;
    result.gpuMs = -1;
    result.giMs = -1;
    result.cpuMs = p->cpuMs;
    result.giBytes = p->gi.memoryBytes();
    result.pendingGi = p->gi.pendingBricks();
    result.pendingMeshJobs = uint32_t(std::min<size_t>(p->meshJobs.size(), UINT32_MAX));
    result.queuedMeshJobs = uint32_t(std::min<size_t>(p->meshQueue.size(), UINT32_MAX));
    result.rebuiltChunks = p->rebuiltChunks;
    result.cachedChunks = p->cachedChunks;
    result.meshPrepareMs = p->meshPrepareMs;
    result.geometryBytes = p->geometryBytes;
    if (p->alive) {
        const auto *s = bgfx::getStats();
        result.gpuFrame = s->gpuFrameNum;
        if (s->gpuTimerFreq > 0 && s->gpuTimeEnd > s->gpuTimeBegin)
            result.gpuMs =
                1000. * double(s->gpuTimeEnd - s->gpuTimeBegin) / double(s->gpuTimerFreq);
        result.textureBytes = uint64_t(std::max(int64_t(0), s->textureMemoryUsed)) +
                              uint64_t(std::max(int64_t(0), s->rtMemoryUsed));
        result.drawCalls = s->numDraw;
        struct GiInterval {
            int64_t begin{INT64_MAX}, end{};
        };
        std::map<uint32_t, GiInterval> giIntervals;
        for (uint16_t i = 0; i < s->numViews; ++i) {
            const auto &v = s->viewStats[i];
            double cpu = s->cpuTimerFreq > 0 ? 1000. * double(v.cpuTimeEnd - v.cpuTimeBegin) /
                                                   double(s->cpuTimerFreq)
                                             : 0;
            double gpu =
                s->gpuTimerFreq > 0 && v.gpuTimeEnd >= v.gpuTimeBegin && v.gpuTimeBegin > 0 &&
                        v.gpuTimeEnd > v.gpuTimeBegin
                    ? 1000. * double(v.gpuTimeEnd - v.gpuTimeBegin) / double(s->gpuTimerFreq)
                    : -1;
            result.passes.push_back({v.view, v.name, cpu, gpu, v.gpuFrameNum});
            // Tile GPUs can execute a raster view's vertex stage ahead of its fragment stage.
            // Summing inclusive view intervals overcounts that overlap. Bound the GI chain by
            // its first occupancy/injection dispatch and its last resolve dispatch instead.
            if (v.view >= 30 && v.view <= 35 && gpu > 0)
                giIntervals[v.gpuFrameNum].begin =
                    std::min(giIntervals[v.gpuFrameNum].begin, v.gpuTimeBegin);
            if (v.view == 124 && gpu > 0)
                giIntervals[v.gpuFrameNum].end = v.gpuTimeEnd;
        }
        for (auto &[frame, interval] : giIntervals)
            if (interval.begin < interval.end && s->gpuTimerFreq > 0) {
                result.giMs =
                    1000. * double(interval.end - interval.begin) / double(s->gpuTimerFreq);
                result.giFrame = frame;
            }
    }
    return result;
}
void RasterRenderer::end() {
    p->cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p->frameStart)
            .count();
    bgfx::frame();
}
void RasterRenderer::screenshot(const std::filesystem::path &path) {
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path.string().c_str());
}
void RasterRenderer::drawImGui(ImDrawData *draw) {
    if (!draw || !draw->Valid || !valid(p->imguiProgram))
        return;
    const int fbw = static_cast<int>(draw->DisplaySize.x * draw->FramebufferScale.x);
    const int fbh = static_cast<int>(draw->DisplaySize.y * draw->FramebufferScale.y);
    if (fbw <= 0 || fbh <= 0)
        return;
    float ortho[16];
    bx::mtxOrtho(ortho, draw->DisplayPos.x, draw->DisplayPos.x + draw->DisplaySize.x,
                 draw->DisplayPos.y + draw->DisplaySize.y, draw->DisplayPos.y, 0.f, 1000.f, 0.f,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewRect(UiView, 0, 0, static_cast<uint16_t>(fbw), static_cast<uint16_t>(fbh));
    bgfx::setViewTransform(UiView, nullptr, ortho);
    bgfx::setViewMode(UiView, bgfx::ViewMode::Sequential);
    for (int n = 0; n < draw->CmdListsCount; ++n) {
        const ImDrawList *list = draw->CmdLists[n];
        uint32_t indexOffset = 0;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        if (bgfx::getAvailTransientVertexBuffer(list->VtxBuffer.Size, p->imguiLayout) <
                static_cast<uint32_t>(list->VtxBuffer.Size) ||
            bgfx::getAvailTransientIndexBuffer(list->IdxBuffer.Size, sizeof(ImDrawIdx) == 4) <
                static_cast<uint32_t>(list->IdxBuffer.Size))
            continue;
        bgfx::allocTransientVertexBuffer(&tvb, list->VtxBuffer.Size, p->imguiLayout);
        bgfx::allocTransientIndexBuffer(&tib, list->IdxBuffer.Size, sizeof(ImDrawIdx) == 4);
        std::memcpy(tvb.data, list->VtxBuffer.Data,
                    static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(tib.data, list->IdxBuffer.Data,
                    static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        for (const ImDrawCmd &command : list->CmdBuffer) {
            if (command.UserCallback) {
                command.UserCallback(list, &command);
                indexOffset += command.ElemCount;
                continue;
            }
            ImVec4 clip = command.ClipRect;
            clip.x = (clip.x - draw->DisplayPos.x) * draw->FramebufferScale.x;
            clip.y = (clip.y - draw->DisplayPos.y) * draw->FramebufferScale.y;
            clip.z = (clip.z - draw->DisplayPos.x) * draw->FramebufferScale.x;
            clip.w = (clip.w - draw->DisplayPos.y) * draw->FramebufferScale.y;
            if (clip.x >= fbw || clip.y >= fbh || clip.z <= 0 || clip.w <= 0) {
                indexOffset += command.ElemCount;
                continue;
            }
            const uint16_t sx = static_cast<uint16_t>(std::max(0.f, clip.x));
            const uint16_t sy = static_cast<uint16_t>(std::max(0.f, clip.y));
            const uint16_t sw = static_cast<uint16_t>(std::max(0.f, clip.z) - sx);
            const uint16_t sh = static_cast<uint16_t>(std::max(0.f, clip.w) - sy);
            bgfx::setScissor(sx, sy, sw, sh);
            const auto tex = bgfx::TextureHandle{static_cast<uint16_t>(command.GetTexID())};
            bgfx::setTexture(0, p->uniform("s_tex", bgfx::UniformType::Sampler),
                             valid(tex) ? tex : p->font);
            bgfx::setVertexBuffer(0, &tvb, command.VtxOffset,
                                  list->VtxBuffer.Size - command.VtxOffset);
            bgfx::setIndexBuffer(&tib, command.IdxOffset, command.ElemCount);
            bgfx::setState(
                BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            bgfx::submit(UiView, p->imguiProgram);
            indexOffset += command.ElemCount;
        }
    }
}

} // namespace aspera
