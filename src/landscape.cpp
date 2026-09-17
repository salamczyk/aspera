#include "aspera/landscape.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aspera {
namespace {
uint32_t hash(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}
float lattice(int x, int y, int z, uint32_t seed) {
    auto h = hash(uint32_t(x) ^ hash(uint32_t(y) ^ hash(uint32_t(z) ^ seed)));
    return float(h >> 8) / 16777215.f;
}
float noise(float x, float y, float z, uint32_t seed) {
    int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    auto smooth = [](float t) { return t * t * (3 - 2 * t); };
    float fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float sides[2];
    for (int k = 0; k < 2; ++k)
        sides[k] = lerp(
            lerp(lattice(ix, iy, iz + k, seed), lattice(ix + 1, iy, iz + k, seed), fx),
            lerp(lattice(ix, iy + 1, iz + k, seed), lattice(ix + 1, iy + 1, iz + k, seed), fx), fy);
    return lerp(sides[0], sides[1], fz);
}
float surface(const LandscapeSettings &s, int x, int z) {
    float scale = s.featureScale;
    return s.surfaceHeight +
           s.hillAmplitude *
               ((noise(x / scale, 0, z / scale, s.seed) * 2 - 1) * .75f +
                (noise(x * 2 / scale, 0, z * 2 / scale, s.seed ^ 0x893u) * 2 - 1) * .25f);
}
bool filled(const LandscapeSettings &s, int x, int y, int z, float height) {
    if (y < s.minimum.y + s.bottomDepth)
        return true;
    float scale = s.featureScale * .45f;
    float displacement =
        (noise(x / scale, y / scale, z / scale, s.seed ^ 0xa73u) * 2 - 1) * s.overhangStrength;
    if (y + .5f > height + displacement)
        return false;
    // Taper caves to zero at the foundation, without making chunk boundaries special.
    float taper = std::clamp(float(y - (s.minimum.y + s.bottomDepth)) / 4.f, 0.f, 1.f);
    float caves = noise(x / scale, y / scale, z / scale, s.seed ^ 0xdecafbadU);
    return caves * taper <= s.caveThreshold;
}
VoxelGrid rawChunk(const LandscapeSettings &s, IVec3 coordinate) {
    VoxelGrid grid;
    int x0 = std::max(s.minimum.x, coordinate.x * 32),
        x1 = std::min(s.maximum.x, coordinate.x * 32 + 32);
    int z0 = std::max(s.minimum.z, coordinate.z * 32),
        z1 = std::min(s.maximum.z, coordinate.z * 32 + 32);
    int y0 = std::max(s.minimum.y, coordinate.y * 32),
        y1 = std::min(s.maximum.y, coordinate.y * 32 + 32);
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x) {
            float height = surface(s, x, z);
            int top = s.maximum.y;
            // Scan the column independent of the requested chunk so layers match across Y chunks.
            while (top > s.minimum.y && !filled(s, x, top - 1, z, height))
                --top;
            int layer = 0;
            for (int y = top - 1; y >= y0; --y) {
                bool solid = filled(s, x, y, z, height);
                if (!solid) {
                    layer = s.grassDepth + s.soilDepth;
                    continue;
                }
                MaterialId material = s.rockMaterial;
                bool caveWall = false;
                for (auto d : Neighbors) {
                    int nx = x + d.x, ny = y + d.y, nz = z + d.z;
                    if (ny < height - s.grassDepth - s.soilDepth &&
                        !filled(s, nx, ny, nz, surface(s, nx, nz)))
                        caveWall = true;
                }
                if (!caveWall && y >= s.minimum.y + s.bottomDepth) {
                    if (layer < s.grassDepth)
                        material = s.grassMaterial;
                    else if (layer < s.grassDepth + s.soilDepth)
                        material = s.soilMaterial;
                }
                if (y < y1)
                    grid.set({x, y, z}, material);
                ++layer;
            }
        }
    return grid;
}
} // namespace
void validateLandscapeSettings(const LandscapeSettings &s) {
    if (s.minimum.x < -1280 || s.minimum.x > 1280 || s.maximum.x < -1280 || s.maximum.x > 1281 ||
        s.minimum.y < -640 || s.minimum.y > 640 || s.maximum.y < -640 || s.maximum.y > 641 ||
        s.minimum.z < -1280 || s.minimum.z > 1280 || s.maximum.z < -1280 || s.maximum.z > 1281)
        throw std::invalid_argument("Landscape coordinates exceed editable bounds");
    auto d = s.maximum - s.minimum;
    if (d.x < 1 || d.y < 8 || d.z < 1 || d.x > 512 || d.y > 512 || d.z > 512 ||
        int64_t(d.x) * d.y * d.z > 2000000 || s.minimum.x < -1280 || s.maximum.x > 1281 ||
        s.minimum.y < -640 || s.maximum.y > 641 || s.minimum.z < -1280 || s.maximum.z > 1281)
        throw std::invalid_argument("Landscape bounds exceed the finite landscape budget");
    for (float f :
         {s.surfaceHeight, s.hillAmplitude, s.featureScale, s.caveThreshold, s.overhangStrength})
        if (!std::isfinite(f))
            throw std::invalid_argument("Landscape parameters must be finite");
    if (s.surfaceHeight < s.minimum.y || s.surfaceHeight >= s.maximum.y || s.hillAmplitude < 0 ||
        s.hillAmplitude > 512 || s.featureScale < 4 || s.featureScale > 2048 ||
        s.caveThreshold < 0 || s.caveThreshold > 1 || s.overhangStrength < 0 ||
        s.overhangStrength > 128 || s.grassDepth < 0 || s.soilDepth < 0 || s.grassDepth > 32 ||
        s.soilDepth > 64 || s.bottomDepth < 1 || s.bottomDepth >= d.y || !s.grassMaterial ||
        !s.soilMaterial || !s.rockMaterial)
        throw std::invalid_argument("Invalid landscape layers, materials, or noise parameters");
}
VoxelGrid generateLandscapeChunk(const LandscapeSettings &s, IVec3 coordinate) {
    validateLandscapeSettings(s);
    auto lo = chunkOf(s.minimum), hi = chunkOf(s.maximum - IVec3{1, 1, 1});
    if (coordinate.x < lo.x || coordinate.y < lo.y || coordinate.z < lo.z || coordinate.x > hi.x ||
        coordinate.y > hi.y || coordinate.z > hi.z)
        return {};
    return rawChunk(s, coordinate);
}
LandscapeData generateLandscape(const LandscapeSettings &s) {
    validateLandscapeSettings(s);
    LandscapeData result;
    auto lo = chunkOf(s.minimum), hi = chunkOf(s.maximum - IVec3{1, 1, 1});
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x)
                for (auto [p, m] : rawChunk(s, {x, y, z}).occupied())
                    result.grid.set(p, m);
    auto dims = s.maximum - s.minimum;
    auto index = [&](IVec3 p) {
        auto q = p - s.minimum;
        return size_t(q.x) + size_t(dims.x) * (q.y + size_t(dims.y) * q.z);
    };
    std::vector<uint8_t> visited(size_t(dims.x) * dims.y * dims.z);
    std::vector<IVec3> queue;
    queue.reserve(result.grid.count());
    for (int z = s.minimum.z; z < s.maximum.z; ++z)
        for (int x = s.minimum.x; x < s.maximum.x; ++x) {
            IVec3 p{x, s.minimum.y, z};
            result.anchors.insert(p);
            visited[index(p)] = 1;
            queue.push_back(p);
        }
    for (size_t i = 0; i < queue.size(); ++i)
        for (auto d : Neighbors) {
            auto p = queue[i] + d;
            if (p.x < s.minimum.x || p.x >= s.maximum.x || p.y < s.minimum.y ||
                p.y >= s.maximum.y || p.z < s.minimum.z || p.z >= s.maximum.z)
                continue;
            auto at = index(p);
            if (!visited[at] && result.grid.get(p)) {
                visited[at] = 1;
                queue.push_back(p);
            }
        }
    for (auto [p, m] : result.grid.occupied())
        if (!visited[index(p)])
            result.grid.set(p, 0);
    return result;
}
} // namespace aspera
