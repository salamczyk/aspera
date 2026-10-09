#include "render_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <future>
#include <set>
#include <stdexcept>
namespace aspera {
Mesh prepareRenderChunk(const VoxelGrid &g, IVec3 c, float s, const std::vector<Material> &m) {
    return meshChunk(g, c, s, m);
}
namespace {
float linear(float x) {
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
void identity(float *m) {
    std::fill(m, m + 16, 0);
    m[0] = m[5] = m[10] = m[15] = 1;
}
void matrix(float *m, const Transform &t) {
    auto q = t.rotation;
    float x = q.x, y = q.y, z = q.z, w = q.w;
    identity(m);
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + w * z);
    m[2] = 2 * (x * z - w * y);
    m[4] = 2 * (x * y - w * z);
    m[5] = 1 - 2 * (x * x + z * z);
    m[6] = 2 * (y * z + w * x);
    m[8] = 2 * (x * z + w * y);
    m[9] = 2 * (y * z - w * x);
    m[10] = 1 - 2 * (x * x + y * y);
    m[12] = t.position.x;
    m[13] = t.position.y;
    m[14] = t.position.z;
}
void inverseRigid(float *dst, const float *src) {
    identity(dst);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            dst[c * 4 + r] = src[r * 4 + c];
    for (int r = 0; r < 3; r++)
        dst[12 + r] = -(dst[r] * src[12] + dst[4 + r] * src[13] + dst[8 + r] * src[14]);
}
Vec3 point(const float *m, const float *v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12],
            m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
            m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14]};
}
uint64_t hashBytes(const void *v, size_t n, uint64_t h = 1469598103934665603ull) {
    auto b = static_cast<const unsigned char *>(v);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
template <class T> uint64_t hashVec(const std::vector<T> &v, uint64_t h) {
    return hashBytes(v.data(), v.size() * sizeof(T), h);
}
struct Slot {
    EntityId entity;
    IVec3 chunk;
    auto operator<=>(const Slot &) const = default;
};
struct Prepared {
    Slot slot;
    uint64_t key, epoch;
    Mesh mesh;
};
struct Request {
    Slot slot;
    uint64_t key, epoch;
    float size;
    std::shared_ptr<const VoxelGrid> grid;
    std::shared_ptr<const std::vector<Material>> materials;
};
struct Cache {
    uint64_t key{}, id{};
    Mesh mesh;
};
} // namespace
struct RenderSceneBuilder::Impl {
    uint64_t epoch{1}, nextId{1}, geometryRevision{}, lastGeometryHash{}, lastTransformHash{},
        lastLightingHash{};
    float voxelSize{};
    uint64_t emitterGeometry{UINT64_MAX}, emitterTransforms{UINT64_MAX},
        emitterLighting{UINT64_MAX};
    std::map<Slot, Cache> cache;
    std::map<Slot, uint64_t> requested;
    std::deque<Request> queue;
    std::vector<std::future<Prepared>> jobs;
    std::map<uint32_t, std::array<float, 16>> previous;
    std::map<PlayerId, Cache> avatars;
    PtScene scene;
    RenderTimings stats;
    bool dirty{true};
    void invalidate() {
        ++epoch;
        queue.clear();
        requested.clear();
        cache.clear();
        avatars.clear();
        previous.clear();
        dirty = true;
    }
    void publish(const World &w) {
        for (auto i = jobs.begin(); i != jobs.end();) {
            if (i->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++i;
                continue;
            }
            Prepared r = i->get();
            i = jobs.erase(i);
            if (r.epoch != epoch)
                continue;
            auto request = requested.find(r.slot);
            if (request == requested.end() || request->second != r.key)
                continue;
            requested.erase(request);
            auto o = w.objects.find(r.slot.entity);
            if (o == w.objects.end() || !o->second.grid.chunks().contains(r.slot.chunk) ||
                o->second.grid.chunkMeshKey(r.slot.chunk) != r.key)
                continue;
            auto &c = cache[r.slot];
            if (!c.id)
                c.id = nextId++;
            c.key = r.key;
            c.mesh = std::move(r.mesh);
            dirty = true;
            ++stats.rebuiltChunks;
        }
    }
    void launch() {
        while (jobs.size() < 4 && !queue.empty()) {
            auto r = std::move(queue.front());
            queue.pop_front();
            jobs.emplace_back(std::async(std::launch::async, [r = std::move(r)] {
                return Prepared{r.slot, r.key, r.epoch,
                                prepareRenderChunk(*r.grid, r.slot.chunk, r.size, *r.materials)};
            }));
        }
    }
};
RenderSceneBuilder::RenderSceneBuilder() : p(std::make_unique<Impl>()) {}
RenderSceneBuilder::~RenderSceneBuilder() = default;
void RenderSceneBuilder::invalidate() {
    p->invalidate();
}
bool RenderSceneBuilder::ready() const {
    return p->requested.empty() && p->queue.empty() && p->jobs.empty();
}
void RenderSceneBuilder::statistics(RenderTimings &s) const {
    s.pendingMeshJobs = uint32_t(p->jobs.size());
    s.queuedMeshJobs = uint32_t(p->queue.size());
    s.cachedChunks = uint32_t(p->cache.size());
    s.rebuiltChunks = p->stats.rebuiltChunks;
    s.meshPrepareMs = p->stats.meshPrepareMs;
}
const PtScene &RenderSceneBuilder::update(const World &w,
                                          const std::map<PlayerId, PlayerState> &players,
                                          PlayerId local) {
    auto start = std::chrono::steady_clock::now();
    p->stats.rebuiltChunks = 0;
    if (p->voxelSize != w.voxelSize) {
        p->voxelSize = w.voxelSize;
        p->invalidate();
    }
    p->publish(w);
    for (auto i = p->cache.begin(); i != p->cache.end();) {
        auto o = w.objects.find(i->first.entity);
        if (o == w.objects.end() || !o->second.grid.chunks().contains(i->first.chunk)) {
            i = p->cache.erase(i);
            p->dirty = true;
        } else
            ++i;
    }
    auto materials = std::make_shared<const std::vector<Material>>(w.materials);
    for (const auto &[id, o] : w.objects) {
        std::shared_ptr<const VoxelGrid> snapshot;
        for (const auto &[chunk, data] : o.grid.chunks()) {
            Slot slot{id, chunk};
            auto key = o.grid.chunkMeshKey(chunk);
            auto old = p->cache.find(slot);
            if (old != p->cache.end() && old->second.key == key)
                continue;
            auto r = p->requested.find(slot);
            if (r != p->requested.end() && r->second == key)
                continue;
            if (p->queue.size() >= 256)
                break;
            if (!snapshot)
                snapshot = std::make_shared<const VoxelGrid>(o.grid);
            p->requested[slot] = key;
            p->queue.push_back({slot, key, p->epoch, w.voxelSize, snapshot, materials});
        }
    }
    p->launch();
    // Avatars are tiny and generated synchronously, using the raster renderer's shape.
    for (auto i = p->avatars.begin(); i != p->avatars.end();)
        if (!players.contains(i->first) || i->first == local || players.at(i->first).spawnBlocked) {
            i = p->avatars.erase(i);
            p->dirty = true;
        } else
            ++i;
    const MaterialId colors[]{3, 4, 5, 2};
    for (const auto &[id, state] : players) {
        if (id == local || state.spawnBlocked || p->avatars.contains(id))
            continue;
        VoxelGrid grid;
        auto mat = colors[(id - 1) % 4];
        if (mat >= w.materials.size())
            mat = 1;
        for (int x = -2; x < 2; x++)
            for (int y = 0; y < 18; y++)
                for (int z = -2; z < 2; z++)
                    if (y >= 8 || x == -2 || x == 1)
                        grid.set({x, y, z}, mat);
        auto &c = p->avatars[id];
        c.id = p->nextId++;
        c.key = 1;
        c.mesh = meshGrid(grid, .1f, w.materials);
        p->dirty = true;
    }
    auto &s = p->scene;
    if (p->dirty) {
        s.vertices.clear();
        s.indices.clear();
        s.geometry.clear();
        auto append = [&](const Cache &c) {
            if (c.mesh.indices.empty())
                return;
            if (s.vertices.size() + c.mesh.vertices.size() > UINT32_MAX ||
                s.indices.size() + c.mesh.indices.size() > UINT32_MAX)
                throw std::runtime_error("Path-tracing geometry exceeds 32-bit addressing");
            PtGeometry g{c.id,
                         c.key,
                         uint32_t(s.vertices.size()),
                         uint32_t(c.mesh.vertices.size()),
                         uint32_t(s.indices.size()),
                         uint32_t(c.mesh.indices.size())};
            s.geometry.push_back(g);
            for (auto &v : c.mesh.vertices) {
                PtVertex t{};
                t.position[0] = v.x;
                t.position[1] = v.y;
                t.position[2] = v.z;
                t.position[3] = 1;
                t.normal[0] = v.nx;
                t.normal[1] = v.ny;
                t.normal[2] = v.nz;
                t.color[3] = v.material;
                s.vertices.push_back(t);
            }
            s.indices.insert(s.indices.end(), c.mesh.indices.begin(), c.mesh.indices.end());
        };
        for (auto &[slot, c] : p->cache)
            append(c);
        for (auto &[id, c] : p->avatars)
            append(c);
        s.geometryRevision = ++p->geometryRevision;
        p->dirty = false;
    }
    s.instances.clear();
    std::map<uint64_t, uint32_t> lookup;
    for (uint32_t i = 0; i < s.geometry.size(); i++)
        lookup[s.geometry[i].id] = i;
    auto instance = [&](const Cache &c, const Transform &t) {
        if (!lookup.contains(c.id))
            return;
        auto gi = lookup.at(c.id);
        auto &g = s.geometry[gi];
        PtInstance i{};
        matrix(i.model, t);
        inverseRigid(i.inverse, i.model);
        auto prior = p->previous.find(uint32_t(c.id));
        std::copy(prior == p->previous.end() ? i.model : prior->second.data(),
                  prior == p->previous.end() ? i.model + 16 : prior->second.data() + 16,
                  i.previous);
        i.offsets[0] = g.indexOffset;
        i.offsets[1] = g.vertexOffset;
        i.offsets[2] = uint32_t(c.id);
        i.offsets[3] = gi;
        s.instances.push_back(i);
    };
    for (auto &[slot, c] : p->cache)
        instance(c, w.objects.at(slot.entity).transform);
    for (auto &[id, c] : p->avatars) {
        auto &v = players.at(id);
        instance(c, {v.position, {0, std::sin(v.yaw * .5f), 0, std::cos(v.yaw * .5f)}});
    }
    uint64_t th = 1469598103934665603ull;
    for (auto &i : s.instances) {
        th = hashBytes(i.model, sizeof(i.model), th);
        th = hashBytes(i.offsets, sizeof(i.offsets), th);
        std::copy(i.model, i.model + 16, p->previous[i.offsets[2]].begin());
    }
    if (th != p->lastTransformHash) {
        ++s.transformRevision;
        p->lastTransformHash = th;
    }
    s.materials.clear();
    for (auto &m : w.materials) {
        PtMaterial t{};
        t.baseRoughness[0] = linear(float((m.rgba >> 24) & 255) / 255);
        t.baseRoughness[1] = linear(float((m.rgba >> 16) & 255) / 255);
        t.baseRoughness[2] = linear(float((m.rgba >> 8) & 255) / 255);
        t.baseRoughness[3] = m.roughness;
        t.emissionMetallic[0] = m.emissive.x * m.emissionIntensity;
        t.emissionMetallic[1] = m.emissive.y * m.emissionIntensity;
        t.emissionMetallic[2] = m.emissive.z * m.emissionIntensity;
        t.emissionMetallic[3] = m.metallic;
        s.materials.push_back(t);
    }
    s.lights.clear();
    for (auto &[id, l] : w.lights) {
        PtLight t{};
        t.positionRange[0] = l.position.x;
        t.positionRange[1] = l.position.y;
        t.positionRange[2] = l.position.z;
        t.positionRange[3] = l.range;
        auto d = normalized(l.direction);
        t.directionType[0] = d.x;
        t.directionType[1] = d.y;
        t.directionType[2] = d.z;
        t.directionType[3] = l.type == LightType::Spot ? 1 : 0;
        auto outer = std::cos(l.outerAngle * .01745329252f);
        float solid = l.type == LightType::Spot ? 2 * 3.14159265f * (1 - outer) : 4 * 3.14159265f;
        t.colorIntensity[0] = l.color.x;
        t.colorIntensity[1] = l.color.y;
        t.colorIntensity[2] = l.color.z;
        t.colorIntensity[3] = l.intensity / std::max(solid, .0001f);
        t.cone[0] = std::cos(l.innerAngle * .01745329252f);
        t.cone[1] = outer;
        t.cone[2] = l.shadows ? 1 : 0;
        s.lights.push_back(t);
    }
    uint64_t lh = hashVec(s.materials, 1469598103934665603ull);
    lh = hashVec(s.lights, lh);
    lh = hashBytes(&w.environment, sizeof(w.environment), lh);
    if (lh != p->lastLightingHash) {
        ++s.lightingRevision;
        p->lastLightingHash = lh;
    }
    if (p->emitterGeometry != s.geometryRevision || p->emitterTransforms != s.transformRevision ||
        p->emitterLighting != s.lightingRevision) {
        s.emitters.clear();
        double total = 0;
        for (auto &i : s.instances) {
            auto &g = s.geometry[i.offsets[3]];
            for (uint32_t j = 0; j < g.indexCount; j += 3) {
                auto &v = s.vertices[g.vertexOffset + s.indices[g.indexOffset + j]];
                auto mat = uint32_t(v.color[3]);
                if (mat >= s.materials.size())
                    continue;
                auto &m = s.materials[mat];
                float lum = .2126f * m.emissionMetallic[0] + .7152f * m.emissionMetallic[1] +
                            .0722f * m.emissionMetallic[2];
                if (lum <= 0)
                    continue;
                auto a = point(i.model, v.position),
                     b = point(
                         i.model,
                         s.vertices[g.vertexOffset + s.indices[g.indexOffset + j + 1]].position),
                     c = point(
                         i.model,
                         s.vertices[g.vertexOffset + s.indices[g.indexOffset + j + 2]].position);
                auto ab = b - a, ac = c - a;
                float area = length(cross(ab, ac)) * .5f;
                if (area <= 0)
                    continue;
                PtEmitter e{};
                std::copy(&a.x, &a.x + 3, e.aArea);
                e.aArea[3] = area;
                std::copy(&b.x, &b.x + 3, e.bCdf);
                std::copy(&c.x, &c.x + 3, e.cPad);
                std::copy(m.emissionMetallic, m.emissionMetallic + 3, e.radiance);
                e.radiance[3] = area * lum;
                total += e.radiance[3];
                s.emitters.push_back(e);
            }
        }
        double cumulative = 0;
        for (auto &e : s.emitters) {
            e.radiance[3] = float(e.radiance[3] / total);
            cumulative += e.radiance[3];
            e.bCdf[3] = float(cumulative);
        }
        if (!s.emitters.empty())
            s.emitters.back().bCdf[3] = 1;
        p->emitterGeometry = s.geometryRevision;
        p->emitterTransforms = s.transformRevision;
        p->emitterLighting = s.lightingRevision;
    }
    p->stats.meshPrepareMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return s;
}
} // namespace aspera
