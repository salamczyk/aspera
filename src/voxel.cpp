#include "aspera/voxel.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace aspera {
namespace {
uint64_t mixKey(uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
uint64_t cellKey(IVec3 p, MaterialId material) {
    if (!material)
        return 0;
    uint64_t xy = uint64_t(uint32_t(p.x)) | (uint64_t(uint32_t(p.y)) << 32);
    return mixKey(mixKey(xy) ^ std::rotl(mixKey(uint32_t(p.z)), 21) ^
                  (uint64_t(material) * 0x9e3779b97f4a7c15ull));
}
int floorDiv(int a, int b) {
    int q = a / b, r = a % b;
    return q - (r < 0 ? 1 : 0);
}
int mod(int a, int b) {
    int r = a % b;
    return r < 0 ? r + b : r;
}
size_t cellIndex(IVec3 local) {
    return static_cast<size_t>(local.x + ChunkSize * (local.y + ChunkSize * local.z));
}
uint32_t colorFor(MaterialId m, const std::vector<Material> &materials) {
    return m < materials.size() ? materials[m].rgba : 0xffffffffu;
}
bool solid(MaterialId m) {
    return m != 0;
}
void addQuad(Mesh &mesh, Vec3 p, Vec3 du, Vec3 dv, Vec3 n, uint32_t rgba, MaterialId material) {
    uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const std::array<Vec3, 4> points{{p, p + du, p + du + dv, p + dv}};
    for (auto a : points)
        mesh.vertices.push_back({a.x, a.y, a.z, n.x, n.y, n.z, rgba, float(material)});
    // The order below is chosen from the actual basis, rather than assuming a
    // particular sweep axis.  It keeps all generated triangles outward-facing.
    if (dot(cross(du, dv), n) > 0.f)
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base, base + 2, base + 3});
    else
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 2, base + 1, base, base + 3, base + 2});
}
float component(Vec3 v, int axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}
float &axis(Vec3 &v, int a) {
    return a == 0 ? v.x : a == 1 ? v.y : v.z;
}
int &axis(IVec3 &v, int a) {
    return a == 0 ? v.x : a == 1 ? v.y : v.z;
}
int floorCell(float a, float size) {
    return static_cast<int>(std::floor(a / size));
}
} // namespace

IVec3 chunkOf(IVec3 p) {
    return {floorDiv(p.x, ChunkSize), floorDiv(p.y, ChunkSize), floorDiv(p.z, ChunkSize)};
}
IVec3 localOf(IVec3 p) {
    return {mod(p.x, ChunkSize), mod(p.y, ChunkSize), mod(p.z, ChunkSize)};
}

Vec3 rotate(Quat q, Vec3 v) {
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!(n > 0.f) || !std::isfinite(n))
        return v;
    // q * (v,0) * conjugate(q), divided by |q|^2: accepts non-unit quaternions.
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) * (2.f / n);
    return v + t * q.w + cross(u, t);
}

MaterialId VoxelGrid::get(IVec3 p) const {
    auto ci = chunkOf(p);
    auto it = chunks_.find(ci);
    if (it == chunks_.end())
        return 0;
    return it->second.cells[cellIndex(localOf(p))];
}

void VoxelGrid::set(IVec3 p, MaterialId material) {
    const IVec3 ci = chunkOf(p), local = localOf(p);
    auto it = chunks_.find(ci);
    if (it == chunks_.end()) {
        if (!material)
            return;
        if (chunks_.size() >= 4096)
            throw std::length_error("Voxel chunk budget exceeded");
        it = chunks_.emplace(ci, Chunk{}).first;
    }
    Chunk &chunk = it->second;
    MaterialId &old = chunk.cells[cellIndex(local)];
    if (old == material)
        return;
    const auto delta = cellKey(p, old) ^ cellKey(p, material);
    contentKey_ ^= delta;
    chunk.contentKey ^= delta;
    const bool visibilityChanged = bool(old) != bool(material);
    if (visibilityChanged) {
        const auto faceDelta = cellKey(p, 1);
        if (local.x == 31)
            chunk.faceKeys[0] ^= faceDelta;
        if (local.x == 0)
            chunk.faceKeys[1] ^= faceDelta;
        if (local.y == 31)
            chunk.faceKeys[2] ^= faceDelta;
        if (local.y == 0)
            chunk.faceKeys[3] ^= faceDelta;
        if (local.z == 31)
            chunk.faceKeys[4] ^= faceDelta;
        if (local.z == 0)
            chunk.faceKeys[5] ^= faceDelta;
    }
    if (!old && material)
        ++chunk.occupied;
    if (old && !material)
        --chunk.occupied;
    old = material;
    dirty_.insert(ci);
    if (visibilityChanged && local.x == 0)
        dirty_.insert(ci + IVec3{-1, 0, 0});
    if (visibilityChanged && local.x == ChunkSize - 1)
        dirty_.insert(ci + IVec3{1, 0, 0});
    if (visibilityChanged && local.y == 0)
        dirty_.insert(ci + IVec3{0, -1, 0});
    if (visibilityChanged && local.y == ChunkSize - 1)
        dirty_.insert(ci + IVec3{0, 1, 0});
    if (visibilityChanged && local.z == 0)
        dirty_.insert(ci + IVec3{0, 0, -1});
    if (visibilityChanged && local.z == ChunkSize - 1)
        dirty_.insert(ci + IVec3{0, 0, 1});
    if (!chunk.occupied)
        chunks_.erase(it);
}

uint64_t VoxelGrid::chunkContentKey(IVec3 coordinate) const {
    auto it = chunks_.find(coordinate);
    return it == chunks_.end() ? 0 : it->second.contentKey;
}
uint64_t VoxelGrid::chunkMeshKey(IVec3 coordinate) const {
    uint64_t key = chunkContentKey(coordinate);
    for (size_t i = 0; i < Neighbors.size(); ++i) {
        auto it = chunks_.find(coordinate + Neighbors[i]);
        if (it != chunks_.end())
            key ^= std::rotl(it->second.faceKeys[i ^ 1], int(i * 9 + 1));
    }
    return key;
}

size_t VoxelGrid::count() const {
    size_t total = 0;
    for (const auto &[_, c] : chunks_)
        total += c.occupied;
    return total;
}

std::vector<std::pair<IVec3, MaterialId>> VoxelGrid::occupied() const {
    std::vector<std::pair<IVec3, MaterialId>> out;
    out.reserve(count());
    for (const auto &[ci, chunk] : chunks_) {
        for (int z = 0; z < ChunkSize; ++z)
            for (int y = 0; y < ChunkSize; ++y)
                for (int x = 0; x < ChunkSize; ++x) {
                    MaterialId m = chunk.cells[cellIndex({x, y, z})];
                    if (m)
                        out.push_back(
                            {{ci.x * ChunkSize + x, ci.y * ChunkSize + y, ci.z * ChunkSize + z},
                             m});
                }
    }
    return out;
}

uint64_t VoxelGrid::checksum() const {
    // FNV-1a over a canonical, coordinate-sorted cell stream.  No native object
    // representation is hashed, so this is stable across chunk insertion order.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    for (const auto &[p, m] : occupied()) {
        auto integer = [&](uint32_t n) {
            for (int i = 0; i < 4; ++i)
                mix(static_cast<uint8_t>(n >> (i * 8)));
        };
        integer(static_cast<uint32_t>(p.x));
        integer(static_cast<uint32_t>(p.y));
        integer(static_cast<uint32_t>(p.z));
        mix(static_cast<uint8_t>(m));
        mix(static_cast<uint8_t>(m >> 8));
    }
    return h;
}

static Mesh meshRegion(const VoxelGrid &grid, const IVec3 *only, float voxelSize,
                       const std::vector<Material> &materials) {
    if (!(voxelSize > 0.f) || !std::isfinite(voxelSize))
        throw std::invalid_argument("voxel size must be positive");
    Mesh mesh;
    struct Bounds {
        IVec3 min, max;
    };
    std::map<IVec3, Bounds> bounds;
    auto add = [&](IVec3 coordinate, const Chunk &chunk) {
        IVec3 origin{coordinate.x * 32, coordinate.y * 32, coordinate.z * 32};
        Bounds bound{{origin.x + 32, origin.y + 32, origin.z + 32}, origin};
        for (int z = 0; z < 32; ++z)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    if (!chunk.cells[cellIndex({x, y, z})])
                        continue;
                    auto p = origin + IVec3{x, y, z};
                    bound.min = {std::min(bound.min.x, p.x), std::min(bound.min.y, p.y),
                                 std::min(bound.min.z, p.z)};
                    bound.max = {std::max(bound.max.x, p.x), std::max(bound.max.y, p.y),
                                 std::max(bound.max.z, p.z)};
                }
        if (chunk.occupied)
            bounds.emplace(coordinate, bound);
    };
    if (only) {
        auto it = grid.chunks().find(*only);
        if (it != grid.chunks().end())
            add(it->first, it->second);
    } else {
        for (auto &[coordinate, chunk] : grid.chunks())
            add(coordinate, chunk);
    }
    for (auto &[chunk, bound] : bounds) {
        int minv[3]{bound.min.x, bound.min.y, bound.min.z},
            maxv[3]{bound.max.x, bound.max.y, bound.max.z};
        for (int d = 0; d < 3; ++d) {
            int u = (d + 1) % 3, v = (d + 2) % 3;
            const int nu = maxv[u] - minv[u] + 1, nv = maxv[v] - minv[v] + 1;
            struct Face {
                MaterialId m{};
                int sign{};
                bool operator==(const Face &o) const {
                    return m == o.m && sign == o.sign;
                }
            };
            std::vector<Face> mask(static_cast<size_t>(nu) * nv);
            for (int plane = minv[d]; plane <= maxv[d] + 1; ++plane) {
                std::fill(mask.begin(), mask.end(), Face{});
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu; ++i) {
                        int a[3]{}, b[3]{};
                        a[d] = plane - 1;
                        b[d] = plane;
                        a[u] = b[u] = minv[u] + i;
                        a[v] = b[v] = minv[v] + j;
                        MaterialId left = grid.get({a[0], a[1], a[2]}),
                                   right = grid.get({b[0], b[1], b[2]});
                        if (plane > minv[d] && solid(left) && !solid(right))
                            mask[static_cast<size_t>(j) * nu + i] = {left, 1};
                        else if (plane <= maxv[d] && !solid(left) && solid(right))
                            mask[static_cast<size_t>(j) * nu + i] = {right, -1};
                    }
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu;) {
                        Face f = mask[static_cast<size_t>(j) * nu + i];
                        if (!f.sign) {
                            ++i;
                            continue;
                        }
                        int w = 1;
                        while (i + w < nu && mask[static_cast<size_t>(j) * nu + i + w] == f)
                            ++w;
                        int h = 1;
                        bool more = true;
                        while (j + h < nv && more) {
                            for (int k = 0; k < w; ++k)
                                if (!(mask[static_cast<size_t>(j + h) * nu + i + k] == f)) {
                                    more = false;
                                    break;
                                }
                            if (more)
                                ++h;
                        }
                        int p[3]{};
                        p[d] = plane;
                        p[u] = minv[u] + i;
                        p[v] = minv[v] + j;
                        Vec3 origin{p[0] * voxelSize, p[1] * voxelSize, p[2] * voxelSize};
                        Vec3 du{}, dv{}, normal{};
                        axis(du, u) = w * voxelSize;
                        axis(dv, v) = h * voxelSize;
                        axis(normal, d) = static_cast<float>(f.sign);
                        addQuad(mesh, origin, du, dv, normal, colorFor(f.m, materials), f.m);
                        for (int yy = 0; yy < h; ++yy)
                            for (int xx = 0; xx < w; ++xx)
                                mask[static_cast<size_t>(j + yy) * nu + i + xx] = {};
                        i += w;
                    }
            }
        }
    }
    return mesh;
}

Mesh meshGrid(const VoxelGrid &grid, float size, const std::vector<Material> &materials) {
    return meshRegion(grid, nullptr, size, materials);
}
Mesh meshChunk(const VoxelGrid &grid, IVec3 coordinate, float size,
               const std::vector<Material> &materials) {
    return meshRegion(grid, &coordinate, size, materials);
}
static std::vector<CollisionBox> collisionRegion(const VoxelGrid &grid, const IVec3 *only,
                                                 float voxelSize) {
    if (!(voxelSize > 0.f) || !std::isfinite(voxelSize))
        throw std::invalid_argument("voxel size must be positive");
    std::set<IVec3> remaining;
    if (only) {
        auto it = grid.chunks().find(*only);
        if (it != grid.chunks().end())
            for (int z = 0; z < 32; ++z)
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x)
                        if (it->second.cells[cellIndex({x, y, z})])
                            remaining.insert(
                                {only->x * 32 + x, only->y * 32 + y, only->z * 32 + z});
    } else {
        for (auto [p, _] : grid.occupied())
            remaining.insert(p);
    }
    std::vector<CollisionBox> out;
    while (!remaining.empty()) {
        IVec3 start = *remaining.begin();
        MaterialId material = grid.get(start);
        auto available = [&](IVec3 p) { return remaining.contains(p) && grid.get(p) == material; };
        int sx = 1, sy = 1, sz = 1;
        while (available({start.x + sx, start.y, start.z}))
            ++sx;
        bool grow = true;
        while (grow) {
            for (int x = 0; x < sx; ++x)
                if (!available({start.x + x, start.y + sy, start.z})) {
                    grow = false;
                    break;
                }
            if (grow)
                ++sy;
        }
        grow = true;
        while (grow) {
            for (int y = 0; y < sy; ++y)
                for (int x = 0; x < sx; ++x)
                    if (!available({start.x + x, start.y + y, start.z + sz})) {
                        grow = false;
                        break;
                    }
            if (grow)
                ++sz;
        }
        for (int z = 0; z < sz; ++z)
            for (int y = 0; y < sy; ++y)
                for (int x = 0; x < sx; ++x)
                    remaining.erase({start.x + x, start.y + y, start.z + z});
        out.push_back({{(start.x + sx * .5f) * voxelSize, (start.y + sy * .5f) * voxelSize,
                        (start.z + sz * .5f) * voxelSize},
                       {sx * .5f * voxelSize, sy * .5f * voxelSize, sz * .5f * voxelSize}});
    }
    return out;
}

std::vector<CollisionBox> collisionBoxes(const VoxelGrid &grid, float size) {
    return collisionRegion(grid, nullptr, size);
}
std::vector<CollisionBox> collisionBoxesChunk(const VoxelGrid &grid, IVec3 coordinate, float size) {
    return collisionRegion(grid, &coordinate, size);
}

std::optional<RayHit> raycast(const VoxelGrid &grid, float voxelSize, Vec3 origin, Vec3 direction,
                              float maxDistance) {
    if (!(voxelSize > 0.f) || !(maxDistance >= 0.f) || !std::isfinite(voxelSize) ||
        !std::isfinite(maxDistance))
        return std::nullopt;
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        std::abs(origin.x / voxelSize) > 10000000 || std::abs(origin.y / voxelSize) > 10000000 ||
        std::abs(origin.z / voxelSize) > 10000000)
        return std::nullopt;
    float dl = length(direction);
    if (!(dl > 0.f) || !std::isfinite(dl))
        return std::nullopt;
    direction = direction / dl;
    IVec3 cell{floorCell(origin.x, voxelSize), floorCell(origin.y, voxelSize),
               floorCell(origin.z, voxelSize)};
    if (MaterialId m = grid.get(cell); m)
        return RayHit{cell, {0, 0, 0}, 0, m};
    int step[3]{};
    float next[3]{}, delta[3]{};
    for (int a = 0; a < 3; ++a) {
        float o = component(origin, a), d = component(direction, a);
        int c = axis(cell, a);
        if (d > 0) {
            step[a] = 1;
            next[a] = ((c + 1) * voxelSize - o) / d;
            delta[a] = voxelSize / d;
        } else if (d < 0) {
            step[a] = -1;
            next[a] = (c * voxelSize - o) / d;
            delta[a] = -voxelSize / d;
        } else {
            step[a] = 0;
            next[a] = delta[a] = std::numeric_limits<float>::infinity();
        }
        if (next[a] < 0.f && next[a] > -1e-5f)
            next[a] = 0.f;
    }
    float t = 0.f;
    IVec3 normal{};
    for (size_t iterations = 0; iterations < 1000000; ++iterations) {
        int a = (next[1] < next[0]) ? 1 : 0;
        if (next[2] < next[a])
            a = 2;
        t = next[a];
        if (t > maxDistance || !std::isfinite(t))
            break;
        axis(cell, a) += step[a];
        normal = {};
        axis(normal, a) = -step[a];
        next[a] += delta[a];
        if (MaterialId m = grid.get(cell); m)
            return RayHit{cell, normal, t, m};
    }
    return std::nullopt;
}
} // namespace aspera
