#include "aspera/landscape.hpp"
#include "aspera/undo.hpp"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace aspera;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F fn, const char *message) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, message);
}
LandscapeSettings small() {
    LandscapeSettings s;
    s.minimum = {-32, -16, -32};
    s.maximum = {32, 32, 32};
    s.surfaceHeight = 4;
    s.hillAmplitude = 10;
    s.featureScale = 24;
    s.seed = 7;
    return s;
}
void generation() {
    auto s = small();
    auto a = generateLandscape(s), b = generateLandscape(s);
    require(a.grid.checksum() == b.grid.checksum() && a.anchors == b.anchors,
            "seeded terrain reproduces exactly");
    ++s.seed;
    require(generateLandscape(s).grid.checksum() != a.grid.checksum(),
            "different seeds vary the terrain");
    s = small();
    VoxelGrid forwards, backwards;
    std::vector<IVec3> coordinates;
    for (auto &[coordinate, chunk] : a.grid.chunks())
        coordinates.push_back(coordinate);
    for (auto p : coordinates)
        for (auto [cell, m] : generateLandscapeChunk(s, p).occupied())
            forwards.set(cell, m);
    for (auto it = coordinates.rbegin(); it != coordinates.rend(); ++it)
        for (auto [cell, m] : generateLandscapeChunk(s, *it).occupied())
            backwards.set(cell, m);
    require(forwards.checksum() == backwards.checksum(),
            "chunk traversal order does not alter density or layers");
    require(generateLandscapeChunk(s, {INT32_MAX, 0, 0}).count() == 0,
            "out of bounds chunk is empty");
    require(a.anchors.size() == 64 * 64, "lowest layer anchored across chunk boundaries");
    size_t grass = 0, soil = 0, rock = 0;
    std::set<IVec3> connected = a.anchors;
    std::vector<IVec3> queue(a.anchors.begin(), a.anchors.end());
    for (size_t i = 0; i < queue.size(); ++i)
        for (auto d : Neighbors) {
            auto n = queue[i] + d;
            if (a.grid.get(n) && connected.insert(n).second)
                queue.push_back(n);
        }
    require(connected.size() == a.grid.count(),
            "all published terrain has a path to the foundation");
    for (auto [p, m] : a.grid.occupied()) {
        require(p.x >= s.minimum.x && p.x < s.maximum.x && p.y >= s.minimum.y &&
                    p.y < s.maximum.y && p.z >= s.minimum.z && p.z < s.maximum.z,
                "terrain obeys half-open bounds");
        grass += m == s.grassMaterial;
        soil += m == s.soilMaterial;
        rock += m == s.rockMaterial;
        if (p.y < s.minimum.y + s.bottomDepth)
            require(m == s.rockMaterial, "foundation is solid rock");
    }
    require(grass && soil && rock, "outdoor material layers all appear");
    s.caveThreshold = 1;
    auto solid = generateLandscape(s);
    require(solid.grid.count() > a.grid.count(), "cave density removes interior terrain");
    s.caveThreshold = 0;
    auto foundation = generateLandscape(s);
    require(foundation.grid.count() < a.grid.count(), "cave threshold controls excavation");
    s = small();
    s.overhangStrength = 0;
    require(generateLandscape(s).grid.checksum() != a.grid.checksum(),
            "volumetric displacement changes the surface");
    s = small();
    s.minimum.x = INT32_MIN;
    rejects([&] { generateLandscape(s); }, "extreme bounds rejected without overflow");
    s = small();
    s.featureScale = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { generateLandscape(s); }, "nonfinite noise rejected");
    s = small();
    s.maximum = s.minimum;
    rejects([&] { generateLandscape(s); }, "empty bounds rejected");
    s = small();
    s.rockMaterial = 0;
    rejects([&] { generateLandscape(s); }, "air cannot be a terrain material");
}
float area(const Mesh &mesh) {
    float result = 0;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto a = mesh.vertices[mesh.indices[i]], b = mesh.vertices[mesh.indices[i + 1]],
             c = mesh.vertices[mesh.indices[i + 2]];
        auto n = cross({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z});
        require(dot(n, {a.nx, a.ny, a.nz}) > 0, "chunk triangles point outwards");
        result += length(n) * .5f;
    }
    return result;
}
void chunkDependencies() {
    VoxelGrid grid;
    for (int x = -1; x <= 33; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z)
                grid.set({x, y, z}, 1);
    std::vector<Material> materials(3);
    float sum = 0;
    size_t cells = 0;
    for (auto &[coordinate, chunk] : grid.chunks()) {
        sum += area(meshChunk(grid, coordinate, 1, materials));
        for (auto box : collisionBoxesChunk(grid, coordinate, 1)) {
            cells +=
                size_t(std::lround(box.halfExtent.x * box.halfExtent.y * box.halfExtent.z * 8));
            for (int z = int(box.center.z - box.halfExtent.z);
                 z < int(box.center.z + box.halfExtent.z); ++z)
                for (int y = int(box.center.y - box.halfExtent.y);
                     y < int(box.center.y + box.halfExtent.y); ++y)
                    for (int x = int(box.center.x - box.halfExtent.x);
                         x < int(box.center.x + box.halfExtent.x); ++x)
                        require(chunkOf({x, y, z}) == coordinate && grid.get({x, y, z}),
                                "collision boxes stay in occupied owner chunk");
        }
    }
    require(sum == area(meshGrid(grid, 1, materials)),
            "chunk mesh union equals whole-grid exterior");
    require(cells == grid.count(), "chunk collision coverage is exact");
    auto neighbor = grid.chunkMeshKey({1, 0, 0});
    grid.set({12, 1, 1}, 2);
    require(grid.chunkMeshKey({1, 0, 0}) == neighbor,
            "interior paint does not invalidate neighbor");
    grid.clearDirty();
    grid.set({31, 1, 1}, 2);
    require(grid.chunkMeshKey({1, 0, 0}) == neighbor,
            "boundary paint preserves neighbor visibility");
    require(grid.dirty().size() == 1 && grid.dirty().contains({0, 0, 0}),
            "boundary paint dirties only its owner chunk");
    grid.set({31, 1, 1}, 0);
    require(grid.chunkMeshKey({1, 0, 0}) != neighbor,
            "boundary removal invalidates neighbor visibility");
    VoxelGrid corner;
    std::map<IVec3, uint64_t> before;
    for (auto p : {IVec3{1, 0, 0}, IVec3{0, 1, 0}, IVec3{0, 0, 1}, IVec3{1, 1, 0}, IVec3{-1, 0, 0}})
        before[p] = corner.chunkMeshKey(p);
    corner.set({31, 31, 31}, 1);
    require(corner.chunkMeshKey({1, 0, 0}) != before[{1, 0, 0}] &&
                corner.chunkMeshKey({0, 1, 0}) != before[{0, 1, 0}] &&
                corner.chunkMeshKey({0, 0, 1}) != before[{0, 0, 1}],
            "corner touches three face neighbors");
    require(corner.chunkMeshKey({1, 1, 0}) == before[{1, 1, 0}] &&
                corner.chunkMeshKey({-1, 0, 0}) == before[{-1, 0, 0}],
            "diagonal and opposite chunks stay valid");
    corner.set({31, 31, 31}, 0);
    require(corner.chunks().empty() && meshChunk(corner, {0, 0, 0}, 1, materials).vertices.empty(),
            "deleted last cell removes chunk geometry");
    require(chunkOf({-1, -32, -33}) == IVec3{-1, -1, -2} &&
                localOf({-1, -32, -33}) == IVec3{31, 0, 31},
            "public negative coordinate helpers");
}
void historyBudget() {
    UndoHistory history(12, 3);
    history.record({1, 1, 1, 1});
    history.record({2, 2, 2, 2});
    history.record({3, 3, 3, 3});
    require(history.bytes() == 12 && history.count() == 3, "undo reaches exact byte budget");
    auto last = history.undo({4, 4, 4, 4});
    require(last && last->front() == 3 && history.canRedo() && history.bytes() == 12,
            "undo transfers within combined budget");
    history.record({5, 5, 5, 5, 5, 5, 5, 5});
    require(!history.canRedo() && history.bytes() == 12 && history.count() == 2,
            "new edit drops redo and oldest undo first");
    auto large = history.undo({6});
    require(large && large->size() == 8, "newest snapshot retained under pressure");
    auto older = history.undo({7});
    require(older && older->front() == 2, "oldest snapshot was evicted");
    history.record(std::vector<uint8_t>(13));
    require(history.bytes() <= 12, "oversized snapshot cannot exceed budget");
    UndoHistory entries(100, 2);
    entries.record({1});
    entries.record({2});
    entries.record({3});
    require(entries.count() == 2, "entry limit retained independently of bytes");
}
} // namespace
int main() {
    generation();
    chunkDependencies();
    historyBudget();
    std::cout << "Landscape generation, chunk boundaries, and undo budgets passed\n";
}
