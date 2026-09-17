#include "render_gi.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#ifndef ASPERA_SHADER_DIR
#define ASPERA_SHADER_DIR "assets/shaders"
#endif
namespace aspera {
namespace {
constexpr uint16_t Invalid = bgfx::kInvalidHandle;
constexpr int Brick = 8, Levels = 7;
template <class H> bool valid(H h) {
    return bgfx::isValid(h);
}
template <class H> void release(H &h) {
    if (valid(h))
        bgfx::destroy(h);
    h = {Invalid};
}
struct Bounds {
    Vec3 lo{}, hi{};
};
struct Box {
    Vec3 center{}, half{};
    MaterialId material{};
};
struct GpuBox {
    float center[4], x[4], y[4], z[4];
};
struct FullscreenVertex {
    float x, y, z, u, v;
};
bgfx::ShaderHandle shader(const char *name) {
    const char *backend = bgfx::getRendererType() == bgfx::RendererType::Metal        ? "metal"
                          : bgfx::getRendererType() == bgfx::RendererType::Direct3D11 ? "dx11"
                                                                                      : "spirv";
    std::vector<std::filesystem::path> roots;
    if (const char *base = SDL_GetBasePath()) {
        roots.emplace_back(std::filesystem::path(base) / "shaders");
        roots.emplace_back(std::filesystem::path(base) / "../share/aspera/shaders");
    }
    roots.emplace_back(ASPERA_SHADER_DIR);
    for (auto &root : roots) {
        std::ifstream f(root / backend / (std::string(name) + ".bin"),
                        std::ios::binary | std::ios::ate);
        if (!f)
            continue;
        auto n = f.tellg();
        if (n <= 0 || n > 64 * 1024 * 1024)
            continue;
        auto *m = bgfx::alloc(uint32_t(n));
        f.seekg(0);
        f.read(reinterpret_cast<char *>(m->data), n);
        return bgfx::createShader(m);
    }
    SDL_Log("Missing GI shader: %s", name);
    return {Invalid};
}
bgfx::ProgramHandle compute(const char *name) {
    auto s = shader(name);
    return valid(s) ? bgfx::createProgram(s, true) : bgfx::ProgramHandle{Invalid};
}
bgfx::ProgramHandle program(const char *fragment) {
    auto a = shader("vs_fullscreen"), b = shader(fragment);
    if (!valid(a) || !valid(b)) {
        release(a);
        release(b);
        return {Invalid};
    }
    return bgfx::createProgram(a, b, true);
}
bool same(const Transform &a, const Transform &b) {
    return a.position == b.position && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w;
}
Bounds bounds(const GpuBox &b) {
    Vec3 extent{std::abs(b.x[0]) + std::abs(b.y[0]) + std::abs(b.z[0]),
                std::abs(b.x[1]) + std::abs(b.y[1]) + std::abs(b.z[1]),
                std::abs(b.x[2]) + std::abs(b.y[2]) + std::abs(b.z[2])};
    Vec3 center{b.center[0], b.center[1], b.center[2]};
    return {center - extent, center + extent};
}
struct CachedChunk {
    uint64_t revision{UINT64_MAX};
    std::vector<Box> boxes;
    std::vector<Bounds> worldBounds;
};
struct CachedObject {
    uint64_t revision{UINT64_MAX};
    Transform transform{};
    std::map<IVec3, CachedChunk> chunks;
    bool transformed{};
};
uint64_t radianceBytes(int resolution) {
    uint64_t total = 0;
    for (int dim = resolution / 2; dim; dim /= 2)
        total += uint64_t(dim) * dim * dim * 6 * 8;
    return total;
}
} // namespace
struct VoxelGi::Impl {
    struct Volume {
        int resolution{}, bricks{}, levelCount{};
        float cell{};
        Vec3 origin{};
        bool reset{true};
        bgfx::DynamicIndexBufferHandle owners{Invalid}, ranges{Invalid}, indices{Invalid},
            dirty{Invalid};
        uint32_t indexCapacity{}, dirtyCount{};
        std::array<bgfx::TextureHandle, Levels> radiance;
        std::vector<uint8_t> dirtyFlags;
        Volume() {
            for (auto &t : radiance)
                t = {Invalid};
        }
        void destroy() {
            release(owners);
            release(ranges);
            release(indices);
            release(dirty);
            for (auto &t : radiance)
                release(t);
            *this = Volume{};
        }
    } volumes[2];
    bool ready{}, low{}, invalid{true};
    uint16_t sourceWidth{}, sourceHeight{}, width{}, height{};
    uint32_t pending{}, boxCapacity{};
    uint64_t memory{}, lightSignature{};
    std::map<EntityId, CachedObject> objects;
    std::vector<GpuBox> boxes;
    bgfx::VertexLayout boxLayout, screenLayout;
    bgfx::DynamicVertexBufferHandle boxBuffer{Invalid};
    bgfx::VertexBufferHandle triangle{Invalid};
    bgfx::ProgramHandle voxelProgram{Invalid}, injectProgram{Invalid}, mipProgram{Invalid},
        traceProgram{Invalid}, temporalProgram{Invalid}, upsampleProgram{Invalid},
        volumeDebugProgram{Invalid};
    bgfx::TextureHandle raw{Invalid}, output{Invalid}, volumeDebug{Invalid};
    std::array<bgfx::TextureHandle, 2> history{{{Invalid}, {Invalid}}},
        historyGeometry{{{Invalid}, {Invalid}}};
    bgfx::FrameBufferHandle rawFb{Invalid};
    uint8_t writeHistory{};
    std::map<std::string, bgfx::UniformHandle> uniforms;
    void clearScreen() {
        release(rawFb);
        release(raw);
        release(output);
        release(volumeDebug);
        for (auto &t : history)
            release(t);
        for (auto &t : historyGeometry)
            release(t);
    }
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
    void vec(const char *name, const void *data, uint16_t count = 1) {
        bgfx::setUniform(uniform(name, bgfx::UniformType::Vec4, count), data, count);
    }
    void matrix(const char *name, const float *data, uint16_t count = 1) {
        bgfx::setUniform(uniform(name, bgfx::UniformType::Mat4, count), data, count);
    }
    void tex(uint8_t slot, const char *name, bgfx::TextureHandle handle) {
        bgfx::setTexture(slot, uniform(name, bgfx::UniformType::Sampler), handle);
    }
    void allocate(Volume &v, int resolution, float cell) {
        v.destroy();
        v.resolution = resolution;
        v.bricks = resolution / Brick;
        v.cell = cell;
        v.dirtyFlags.resize(v.bricks * v.bricks * v.bricks, 1);
        v.owners =
            bgfx::createDynamicIndexBuffer(resolution * resolution * resolution,
                                           BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
        v.ranges = bgfx::createDynamicIndexBuffer(uint32_t(v.dirtyFlags.size() * 2),
                                                  BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
        v.dirty = bgfx::createDynamicIndexBuffer(uint32_t(v.dirtyFlags.size()),
                                                 BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
        for (int dim = resolution / 2, level = 0; dim; dim /= 2, ++level) {
            v.radiance[level] =
                bgfx::createTexture3D(uint16_t(dim), uint16_t(dim * 6), uint16_t(dim), false,
                                      bgfx::TextureFormat::RGBA16F,
                                      BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_U_CLAMP |
                                          BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
            ++v.levelCount;
        }
        for (int level = v.levelCount; level < Levels; ++level)
            v.radiance[level] = {Invalid};
        if (!valid(v.owners) || !valid(v.ranges) || !valid(v.dirty))
            throw std::runtime_error("GI occupancy allocation failed");
        for (int level = 0; level < v.levelCount; ++level)
            if (!valid(v.radiance[level]))
                throw std::runtime_error("GI radiance allocation failed");
    }
    void forBricks(Volume &v, const Bounds &b, const auto &function) {
        float size = v.cell * Brick;
        Vec3 relativeLo = (b.lo - v.origin) / size, relativeHi = (b.hi - v.origin) / size;
        if (relativeHi.x < 0 || relativeHi.y < 0 || relativeHi.z < 0 || relativeLo.x >= v.bricks ||
            relativeLo.y >= v.bricks || relativeLo.z >= v.bricks)
            return;
        auto bounded = [&](float value) { return std::clamp(value, 0.f, float(v.bricks - 1)); };
        relativeLo = {bounded(relativeLo.x), bounded(relativeLo.y), bounded(relativeLo.z)};
        relativeHi = {bounded(relativeHi.x), bounded(relativeHi.y), bounded(relativeHi.z)};
        int lo[3] = {int(std::floor(relativeLo.x)), int(std::floor(relativeLo.y)),
                     int(std::floor(relativeLo.z))};
        int hi[3] = {int(std::floor(relativeHi.x)), int(std::floor(relativeHi.y)),
                     int(std::floor(relativeHi.z))};
        for (int axis = 0; axis < 3; ++axis) {
            if (hi[axis] < 0 || lo[axis] >= v.bricks)
                return;
            lo[axis] = std::max(0, lo[axis]);
            hi[axis] = std::min(v.bricks - 1, hi[axis]);
        }
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    function(x + v.bricks * (y + v.bricks * z));
    }
    void mark(const Bounds &b) {
        for (auto &v : volumes)
            forBricks(v, b, [&](int index) { v.dirtyFlags[index] = 1; });
    }
    bool collect(const World &world) {
        bool changed = false;
        for (auto it = objects.begin(); it != objects.end();)
            if (!world.objects.contains(it->first)) {
                for (const auto &[_, chunk] : it->second.chunks)
                    for (const auto &b : chunk.worldBounds)
                        mark(b);
                it = objects.erase(it);
                changed = true;
            } else
                ++it;
        boxes.clear();
        for (auto &[id, o] : world.objects) {
            auto &cache = objects[id];
            bool moved = !cache.transformed || !same(cache.transform, o.transform);
            std::set<IVec3> changedChunks;
            if (moved) {
                for (const auto &[_, chunk] : cache.chunks)
                    for (const auto &b : chunk.worldBounds)
                        mark(b);
                changed = true;
            }
            for (auto it = cache.chunks.begin(); it != cache.chunks.end();) {
                if (!o.grid.chunks().contains(it->first)) {
                    for (const auto &b : it->second.worldBounds)
                        mark(b);
                    it = cache.chunks.erase(it);
                    changed = true;
                } else
                    ++it;
            }
            for (const auto &[coordinate, _] : o.grid.chunks()) {
                auto &chunk = cache.chunks[coordinate];
                const uint64_t key = o.grid.chunkContentKey(coordinate);
                if (chunk.revision == key)
                    continue;
                for (const auto &b : chunk.worldBounds)
                    mark(b);
                chunk.boxes.clear();
                for (const auto &b : collisionBoxesChunk(o.grid, coordinate, world.voxelSize)) {
                    IVec3 cell{int(std::floor(b.center.x / world.voxelSize)),
                               int(std::floor(b.center.y / world.voxelSize)),
                               int(std::floor(b.center.z / world.voxelSize))};
                    auto material = o.grid.get(cell);
                    if (material)
                        chunk.boxes.push_back({b.center, b.halfExtent, material});
                }
                chunk.revision = key;
                changedChunks.insert(coordinate);
                changed = true;
            }

            for (auto &[_, chunk] : cache.chunks) {
                const bool geometry = changedChunks.contains(_);
                if (geometry || moved)
                    chunk.worldBounds.clear();
                for (const auto &b : chunk.boxes) {
                    auto center = o.transform.position + rotate(o.transform.rotation, b.center);
                    auto x = rotate(o.transform.rotation, {b.half.x, 0, 0}),
                         y = rotate(o.transform.rotation, {0, b.half.y, 0}),
                         z = rotate(o.transform.rotation, {0, 0, b.half.z});
                    GpuBox gpu{{center.x, center.y, center.z, float(b.material)},
                               {x.x, x.y, x.z, 0},
                               {y.x, y.y, y.z, 0},
                               {z.x, z.y, z.z, 0}};
                    if (geometry || moved)
                        chunk.worldBounds.push_back(bounds(gpu));
                    boxes.push_back(gpu);
                }
                if (geometry || moved)
                    for (const auto &b : chunk.worldBounds)
                        mark(b);
            }
            cache.revision = o.grid.contentKey();
            cache.transform = o.transform;
            cache.transformed = true;
        }
        if (boxes.size() > UINT32_MAX / sizeof(GpuBox))
            throw std::runtime_error("GI geometry buffer exceeds GPU address range");
        if (!valid(boxBuffer) || boxCapacity < std::max(size_t(1), boxes.size())) {
            release(boxBuffer);
            boxCapacity = uint32_t(std::max(size_t(1), boxes.size()));
            boxBuffer =
                bgfx::createDynamicVertexBuffer(boxCapacity, boxLayout, BGFX_BUFFER_COMPUTE_READ);
        }
        if (!boxes.empty())
            bgfx::update(boxBuffer, 0,
                         bgfx::copy(boxes.data(), uint32_t(boxes.size() * sizeof(GpuBox))));
        return changed;
    }
    void occupancy(Volume &v, int cascade) {
        if (v.reset) {
            std::fill(v.dirtyFlags.begin(), v.dirtyFlags.end(), 1);
            v.reset = false;
        }
        std::vector<uint32_t> dirty;
        for (uint32_t i = 0; i < v.dirtyFlags.size(); ++i)
            if (v.dirtyFlags[i])
                dirty.push_back(i);
        v.dirtyCount = uint32_t(dirty.size());
        pending += v.dirtyCount;
        if (dirty.empty())
            return;
        std::vector<std::vector<uint32_t>> bins(v.dirtyFlags.size());
        for (uint32_t i = 0; i < boxes.size(); ++i)
            forBricks(v, bounds(boxes[i]), [&](int index) {
                if (v.dirtyFlags[index])
                    bins[index].push_back(i);
            });
        std::vector<uint32_t> ranges(v.dirtyFlags.size() * 2), indices;
        for (uint32_t index : dirty) {
            ranges[index * 2] = uint32_t(indices.size());
            ranges[index * 2 + 1] = uint32_t(bins[index].size());
            indices.insert(indices.end(), bins[index].begin(), bins[index].end());
        }
        if (!valid(v.indices) || v.indexCapacity < std::max(size_t(1), indices.size())) {
            release(v.indices);
            v.indexCapacity = uint32_t(std::max(size_t(1), indices.size()));
            v.indices = bgfx::createDynamicIndexBuffer(
                v.indexCapacity, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
        }
        bgfx::update(v.ranges, 0, bgfx::copy(ranges.data(), uint32_t(ranges.size() * 4)));
        bgfx::update(v.dirty, 0, bgfx::copy(dirty.data(), uint32_t(dirty.size() * 4)));
        if (!indices.empty())
            bgfx::update(v.indices, 0, bgfx::copy(indices.data(), uint32_t(indices.size() * 4)));
        float params[] = {v.origin.x, v.origin.y, v.origin.z, v.cell},
              dimensions[] = {float(v.resolution), float(v.bricks), float(dirty.size()), 0};
        vec("u_giOrigin", params);
        vec("u_giDimensions", dimensions);
        bgfx::setBuffer(0, boxBuffer, bgfx::Access::Read);
        bgfx::setBuffer(1, v.ranges, bgfx::Access::Read);
        bgfx::setBuffer(2, v.indices, bgfx::Access::Read);
        bgfx::setBuffer(3, v.dirty, bgfx::Access::Read);
        bgfx::setBuffer(4, v.owners, bgfx::Access::Write);
        bgfx::setViewName(bgfx::ViewId(30 + cascade),
                          cascade ? "GI far occupancy" : "GI near occupancy");
        bgfx::dispatch(bgfx::ViewId(30 + cascade), voxelProgram, uint32_t(dirty.size()));
        std::fill(v.dirtyFlags.begin(), v.dirtyFlags.end(), 0);
    }
    void inject(Volume &v, int cascade, const World &world, bgfx::TextureHandle palette,
                const GiDirectLighting &direct) {
        float origin[] = {v.origin.x, v.origin.y, v.origin.z, v.cell},
              dimensions[] = {float(v.resolution), float(direct.lightCount), 0, 0};
        vec("u_giOrigin", origin);
        vec("u_giDimensions", dimensions);
        matrix("u_giView", direct.view);
        matrix("u_giShadowMatrices", direct.shadowMatrices, 15);
        vec("u_giShadowRects", direct.shadowRects, 15);
        vec("u_giShadowParams", direct.shadowParams);
        vec("u_giLights", direct.lightData, 320);
        auto d = normalized(world.environment.sunDirection);
        float direction[] = {d.x, d.y, d.z, 0},
              sun[] = {world.environment.sunColor.x, world.environment.sunColor.y,
                       world.environment.sunColor.z, world.environment.sunIntensity};
        vec("u_giSunDirection", direction);
        vec("u_giSun", sun);
        bgfx::setBuffer(0, v.owners, bgfx::Access::Read);
        tex(1, "s_giPalette", palette);
        tex(2, "s_giShadow", direct.shadowAtlas);
        bgfx::setImage(3, v.radiance[0], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        uint32_t dim = uint32_t(v.resolution / 2);
        auto view = bgfx::ViewId(34 + cascade);
        bgfx::setViewName(view, cascade ? "GI far injection" : "GI near injection");
        bgfx::dispatch(view, injectProgram, (dim + 3) / 4, (dim * 6 + 3) / 4, (dim + 3) / 4);
        for (int level = 1; level < v.levelCount; ++level) {
            dim /= 2;
            float params[] = {float(dim), float(dim * 2), 0, 0};
            vec("u_giMip", params);
            tex(0, "s_giMipSource", v.radiance[level - 1]);
            bgfx::setImage(1, v.radiance[level], 0, bgfx::Access::Write,
                           bgfx::TextureFormat::RGBA16F);
            view = bgfx::ViewId(40 + level * 2 + cascade);
            bgfx::setViewName(view, cascade ? "GI far mip" : "GI near mip");
            bgfx::dispatch(view, mipProgram, (dim + 3) / 4, (dim * 6 + 3) / 4, (dim + 3) / 4);
        }
    }
    uint64_t signature(const World &w) {
        uint64_t h = 1469598103934665603ull;
        auto bytes = [&](const void *p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                h ^= static_cast<const uint8_t *>(p)[i];
                h *= 1099511628211ull;
            }
        };
        bytes(&w.environment, sizeof(w.environment));
        for (auto &m : w.materials) {
            bytes(&m.rgba, sizeof(m.rgba));
            bytes(&m.metallic, sizeof(m.metallic));
            bytes(&m.emissive, sizeof(m.emissive));
            bytes(&m.emissionIntensity, sizeof(m.emissionIntensity));
        }
        for (auto &[id, l] : w.lights) {
            bytes(&id, sizeof(id));
            bytes(&l.type, sizeof(l.type));
            bytes(&l.shadows, sizeof(l.shadows));
            bytes(&l.shadowPriority, sizeof(l.shadowPriority));
            bytes(&l.position, sizeof(l.position));
            bytes(&l.direction, sizeof(l.direction));
            bytes(&l.color, sizeof(l.color));
            bytes(&l.intensity, sizeof(l.intensity));
            bytes(&l.range, sizeof(l.range));
            bytes(&l.innerAngle, sizeof(l.innerAngle));
            bytes(&l.outerAngle, sizeof(l.outerAngle));
        }
        return h;
    }
};
VoxelGi::VoxelGi() : p(std::make_unique<Impl>()) {}
VoxelGi::~VoxelGi() {
    shutdown();
}
bool VoxelGi::init() {
    if (p->ready)
        return true;
    auto *caps = bgfx::getCaps();
    if (!(caps->supported & BGFX_CAPS_COMPUTE) || caps->limits.maxTextureSamplers < 16 ||
        caps->limits.maxTextureSize < 768)
        return false;
    const auto flags = caps->formats[bgfx::TextureFormat::RGBA16F];
    if (!(flags & BGFX_CAPS_FORMAT_TEXTURE_3D) || !(flags & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE))
        return false;
    if (!(caps->formats[bgfx::TextureFormat::RGBA32F] & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F,
                              BGFX_TEXTURE_COMPUTE_WRITE)) {
        SDL_Log("GI requires RGBA32F compute-image history storage");
        return false;
    }
    p->voxelProgram = compute("cs_gi_voxelize");
    p->injectProgram = compute("cs_gi_inject");
    p->mipProgram = compute("cs_gi_mip");
    p->traceProgram = program("fs_gi_trace");
    p->temporalProgram = compute("cs_gi_temporal");
    p->upsampleProgram = compute("cs_gi_upsample");
    p->volumeDebugProgram = compute("cs_gi_debug");
    for (auto program : {p->voxelProgram, p->injectProgram, p->mipProgram, p->traceProgram,
                         p->temporalProgram, p->upsampleProgram, p->volumeDebugProgram})
        if (!valid(program))
            return false;
    p->boxLayout.begin()
        .add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Bitangent, 4, bgfx::AttribType::Float)
        .end();
    p->screenLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    bool bottom = caps->originBottomLeft;
    FullscreenVertex vertices[] = {{-1, -1, 0, 0, bottom ? 0.f : 1.f},
                                   {3, -1, 0, 2, bottom ? 0.f : 1.f},
                                   {-1, 3, 0, 0, bottom ? 2.f : -1.f}};
    p->triangle = bgfx::createVertexBuffer(bgfx::copy(vertices, sizeof(vertices)), p->screenLayout);
    p->ready = valid(p->triangle);
    return p->ready;
}
void VoxelGi::resize(uint16_t width, uint16_t height, bool low) {
    if (!p->ready)
        return;
    if (p->sourceWidth == width && p->sourceHeight == height && p->low == low && valid(p->output))
        return;
    p->sourceWidth = width;
    p->sourceHeight = height;
    p->width = std::max(1, int(width) / (low ? 4 : 2));
    p->height = std::max(1, int(height) / (low ? 4 : 2));
    p->low = low;
    p->invalid = true;
    p->clearScreen();
    try {
        uint64_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        p->raw = bgfx::createTexture2D(p->width, p->height, false, 1, bgfx::TextureFormat::RGBA16F,
                                       BGFX_TEXTURE_RT | clamp);
        if (!valid(p->raw))
            throw std::runtime_error("GI tracing target allocation failed");
        p->rawFb = bgfx::createFrameBuffer(1, &p->raw, false);
        p->output = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F,
                                          BGFX_TEXTURE_COMPUTE_WRITE | clamp);
        p->volumeDebug =
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F,
                                  BGFX_TEXTURE_COMPUTE_WRITE | clamp);
        for (auto &t : p->history)
            t = bgfx::createTexture2D(p->width, p->height, false, 1, bgfx::TextureFormat::RGBA16F,
                                      BGFX_TEXTURE_COMPUTE_WRITE | clamp);
        for (auto &t : p->historyGeometry)
            t = bgfx::createTexture2D(p->width, p->height, false, 1, bgfx::TextureFormat::RGBA32F,
                                      BGFX_TEXTURE_COMPUTE_WRITE | clamp | BGFX_SAMPLER_MIN_POINT |
                                          BGFX_SAMPLER_MAG_POINT);
        p->allocate(p->volumes[0], low ? 64 : 128, .1f);
        p->allocate(p->volumes[1], low ? 64 : 128, .4f);
        if (!valid(p->rawFb) || !valid(p->output) || !valid(p->volumeDebug))
            throw std::runtime_error("GI resolve allocation failed");
        for (auto t : p->history)
            if (!valid(t))
                throw std::runtime_error("GI lighting history allocation failed");
        for (auto t : p->historyGeometry)
            if (!valid(t))
                throw std::runtime_error("GI geometry history allocation failed");
    } catch (...) {
        p->clearScreen();
        throw;
    }
}
void VoxelGi::invalidate() {
    p->invalid = true;
}
void VoxelGi::render(const World &world, Vec3 eye, const float *inverseVP, const float *previousVP,
                     bgfx::TextureHandle depth, bgfx::TextureHandle normal,
                     bgfx::TextureHandle motion, bgfx::TextureHandle palette,
                     const GiDirectLighting &direct, bool resetHistory, bool debugVolume) {
    if (!p->ready || !valid(p->output))
        return;
    if (p->invalid) {
        p->objects.clear();
        for (auto &v : p->volumes)
            v.reset = true;
        resetHistory = true;
        p->invalid = false;
    }
    for (int i = 0; i < 2; ++i) {
        auto &v = p->volumes[i];
        float cell = world.voxelSize * (i ? 4 : 1);
        if (v.cell != cell) {
            p->allocate(v, p->low ? 64 : 128, cell);
            p->objects.clear();
            resetHistory = true;
        }
        float snap = cell * Brick, half = cell * v.resolution * .5f;
        Vec3 origin{std::floor(eye.x / snap) * snap - half, std::floor(eye.y / snap) * snap - half,
                    std::floor(eye.z / snap) * snap - half};
        if (origin != v.origin) {
            v.origin = origin;
            v.reset = true;
            resetHistory = true;
        }
    }
    bool changed = p->collect(world);
    resetHistory |= changed;
    auto signature = p->signature(world);
    if (signature != p->lightSignature) {
        resetHistory = true;
        p->lightSignature = signature;
    }
    p->pending = 0;
    for (int i = 0; i < 2; ++i)
        p->occupancy(p->volumes[i], i);
    for (int i = 0; i < 2; ++i)
        p->inject(p->volumes[i], i, world, palette, direct);
    float screen[] = {float(p->sourceWidth), float(p->sourceHeight),
                      bgfx::getCaps()->originBottomLeft ? 1.f : -1.f,
                      bgfx::getCaps()->homogeneousDepth ? 2.f : 1.f};
    float resolution[] = {float(p->width), float(p->height), resetHistory ? 1.f : 0.f,
                          float(p->volumes[0].resolution)};
    bgfx::setViewName(120, "GI cone trace");
    bgfx::setViewFrameBuffer(120, p->rawFb);
    bgfx::setViewRect(120, 0, 0, p->width, p->height);
    bgfx::setViewClear(120, BGFX_CLEAR_COLOR, uint32_t(0x000000ff));
    p->tex(0, "s_giDepth", depth);
    p->tex(1, "s_giNormal", normal);
    for (int i = 0; i < 2; ++i)
        for (int level = 0; level < Levels; ++level) {
            std::string name = std::string(i ? "s_giFar" : "s_giNear") + std::to_string(level);
            p->tex(uint8_t(2 + i * Levels + level), name.c_str(),
                   p->volumes[i].radiance[std::min(level, p->volumes[i].levelCount - 1)]);
        }
    for (int i = 0; i < 2; ++i) {
        auto &v = p->volumes[i];
        float origin[] = {v.origin.x, v.origin.y, v.origin.z, v.cell};
        p->vec(i ? "u_giFarOrigin" : "u_giOrigin", origin);
    }
    p->matrix("u_giInverseVP", inverseVP);
    p->vec("u_giScreen", screen);
    p->vec("u_giResolve", resolution);
    bgfx::setVertexBuffer(0, p->triangle);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(120, p->traceProgram);
    int write = p->writeHistory, read = 1 - write;
    p->tex(0, "s_giCurrent", p->raw);
    p->tex(1, "s_giHistory", p->history[read]);
    p->tex(2, "s_giPreviousGeometry", p->historyGeometry[read]);
    p->tex(3, "s_giDepth", depth);
    p->tex(4, "s_giNormal", normal);
    p->tex(5, "s_giMotion", motion);
    bgfx::setImage(6, p->history[write], 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    bgfx::setImage(7, p->historyGeometry[write], 0, bgfx::Access::Write,
                   bgfx::TextureFormat::RGBA32F);
    p->matrix("u_giInverseVP", inverseVP);
    p->matrix("u_giPreviousVP", previousVP);
    p->matrix("u_giView", direct.view);
    p->vec("u_giScreen", screen);
    p->vec("u_giResolve", resolution);
    bgfx::setViewName(122, "GI temporal resolve");
    bgfx::dispatch(122, p->temporalProgram, (p->width + 7) / 8, (p->height + 7) / 8);
    p->tex(0, "s_giFiltered", p->history[write]);
    p->tex(1, "s_giGeometry", p->historyGeometry[write]);
    p->tex(2, "s_giDepth", depth);
    p->tex(3, "s_giNormal", normal);
    bgfx::setImage(4, p->output, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
    p->matrix("u_giInverseVP", inverseVP);
    p->matrix("u_giView", direct.view);
    p->vec("u_giScreen", screen);
    p->vec("u_giResolve", resolution);
    bgfx::setViewName(124, "GI bilateral upsample");
    bgfx::dispatch(124, p->upsampleProgram, (p->sourceWidth + 7) / 8, (p->sourceHeight + 7) / 8);
    // Debug is an actual near-volume occupancy slice through the camera's Y plane.
    if (debugVolume) {
        bgfx::setBuffer(0, p->volumes[0].owners, bgfx::Access::Read);
        bgfx::setImage(1, p->volumeDebug, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA16F);
        float debug[] = {float(p->sourceWidth), float(p->sourceHeight),
                         float(p->volumes[0].resolution), float(p->volumes[0].resolution / 2)};
        p->vec("u_giDebug", debug);
        bgfx::setViewName(126, "GI voxel debug");
        bgfx::dispatch(126, p->volumeDebugProgram, (p->sourceWidth + 7) / 8,
                       (p->sourceHeight + 7) / 8);
    }
    p->writeHistory = uint8_t(read);
    p->memory = uint64_t(p->sourceWidth) * p->sourceHeight * 16 +
                uint64_t(p->width) * p->height * 56 + uint64_t(p->boxCapacity) * sizeof(GpuBox);
    for (auto &v : p->volumes)
        p->memory += uint64_t(v.resolution) * v.resolution * v.resolution * 4 +
                     radianceBytes(v.resolution) +
                     (uint64_t(v.dirtyFlags.size()) * 3 + v.indexCapacity) * 4;
}
bgfx::TextureHandle VoxelGi::texture() const {
    return p->output;
}
bgfx::TextureHandle VoxelGi::visibilityTexture() const {
    return p->volumeDebug;
}
uint32_t VoxelGi::pendingBricks() const {
    return p->pending;
}
uint64_t VoxelGi::memoryBytes() const {
    return p->memory;
}
void VoxelGi::shutdown() {
    if (!p)
        return;
    for (auto &v : p->volumes)
        v.destroy();
    release(p->boxBuffer);
    release(p->triangle);
    p->clearScreen();
    for (auto *program : {&p->voxelProgram, &p->injectProgram, &p->mipProgram, &p->traceProgram,
                          &p->temporalProgram, &p->upsampleProgram, &p->volumeDebugProgram})
        release(*program);
    for (auto &[_, u] : p->uniforms)
        release(u);
    p = std::make_unique<Impl>();
}
} // namespace aspera
