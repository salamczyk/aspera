#include "internal.hpp"
#include "structure_reference.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local std::ptrdiff_t allocationFailureCountdown = -1;
void *testAllocate(std::size_t bytes) {
    if (allocationFailureCountdown == 0)
        throw std::bad_alloc();
    if (allocationFailureCountdown > 0)
        --allocationFailureCountdown;
    if (void *memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
} // namespace
void *operator new(std::size_t bytes) {
    return testAllocate(bytes);
}
void *operator new[](std::size_t bytes) {
    return testAllocate(bytes);
}
void operator delete(void *memory) noexcept {
    std::free(memory);
}
void operator delete[](void *memory) noexcept {
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

using namespace aspera;
namespace {
constexpr float Step = 1.f / 60.f;

class ScopedAllocationFailure {
  public:
    explicit ScopedAllocationFailure(std::ptrdiff_t countdown)
        : previous_(allocationFailureCountdown) {
        allocationFailureCountdown = countdown;
    }
    ~ScopedAllocationFailure() {
        allocationFailureCountdown = previous_;
    }

  private:
    std::ptrdiff_t previous_;
};

void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
bool near(float a, float b, float relative = 1e-5f) {
    return std::abs(a - b) <= relative * std::max({1.f, std::abs(a), std::abs(b)});
}
bool near(Vec3 a, Vec3 b, float relative = 1e-5f) {
    return near(a.x, b.x, relative) && near(a.y, b.y, relative) && near(a.z, b.z, relative);
}
Material material(const char *name) {
    Material m;
    m.name = name;
    m.rgba = 0xaacceeFF;
    m.density = 1000.f;
    // The fatigue fixture stays in the shear regime; bending is deliberately
    // far above it so the reference threshold is deterministic.
    m.tension = m.compression = 5000.f;
    m.shear = 5000.f;
    m.bending = 1.e9f;
    return m;
}
World world() {
    World w;
    w.voxelSize = .1f;
    w.materials = {Material{}, material("one"), material("two")};
    return w;
}
VoxelGrid line(int first, int last, bool alternating = false) {
    VoxelGrid grid;
    for (int x = first; x <= last; ++x)
        grid.set({x, 0, 0}, alternating ? MaterialId((x - first) & 1 ? 2 : 1) : 1);
    grid.clearDirty();
    return grid;
}
void configureCantilever(Engine &e) {
    e.world = world();
    auto id = e.world.spawn("cantilever", line(-9, 9, true), {}, false);
    auto &o = e.world.objects.at(id);
    o.anchors.insert({-9, 0, 0});
    o.velocity = {1.25f, -2.f, .75f};
    o.angularVelocity = {.2f, .3f, -.1f};
    e.world.joints.push_back({1, id, id + 99, cellCenter({9, 0, 0}, e.world.voxelSize), false});
}

template <class ReferenceGraph, class Graph>
void compareGraph(const ReferenceGraph &reference, const Graph &actual) {
    require(reference.nodes.size() == actual.nodes.size(), "node count/order changed");
    require(reference.bonds.size() == actual.bonds.size(), "bond count/order changed");
    for (size_t i = 0; i < reference.nodes.size(); ++i) {
        const auto &a = reference.nodes[i];
        const auto &b = actual.nodes[i];
        require(a.cells == b.cells && a.material == b.material && a.anchored == b.anchored,
                "node membership/order changed");
        require(near(a.center, b.center) && near(a.mass, b.mass), "node geometry changed");
    }
    for (size_t i = 0; i < reference.bonds.size(); ++i) {
        const auto &a = reference.bonds[i];
        const auto &b = actual.bonds[i];
        require(a.a == b.a && a.b == b.b && a.faces == b.faces, "bond ownership/order changed");
        require(near(a.center, b.center) && near(a.normal, b.normal) && near(a.area, b.area),
                "bond geometry changed");
        require(near(a.damage, b.damage), "bond fatigue changed");
    }
}
void compareObjects(const Object &reference, const Object &actual) {
    require(reference.grid.contentKey() == actual.grid.contentKey() &&
                reference.anchors == actual.anchors && reference.seams == actual.seams &&
                reference.revision == actual.revision && reference.dynamic == actual.dynamic,
            "fracture ownership differs");
    require(near(reference.velocity, actual.velocity, 1e-3f) &&
                near(reference.angularVelocity, actual.angularVelocity, 1e-3f),
            "fragment velocity differs");
    require(near(reference.stress, actual.stress), "stress differs");
    require(reference.stressRegions.size() == actual.stressRegions.size(), "stress regions differ");
    for (size_t i = 0; i < reference.stressRegions.size(); ++i)
        require(near(reference.stressRegions[i].utilization, actual.stressRegions[i].utilization),
                "stress utilization differs");
}
void compareState(const Engine &reference, const Engine &actual,
                  const test::ReferenceStructure &referenceSolver, const Structure &solver) {
    require(reference.world.objects.size() == actual.world.objects.size(),
            "fragment count differs");
    require(reference.world.joints.size() == actual.world.joints.size(),
            "joint reassignment differs");
    for (const auto &[id, object] : reference.world.objects) {
        auto it = actual.world.objects.find(id);
        require(it != actual.world.objects.end(), "fragment id differs");
        compareObjects(object, it->second);
        auto refGraph = referenceSolver.graphs.find(id);
        auto graph = solver.graphs.find(id);
        // The production solver may seed a newly published fragment graph in
        // the fracture tick; the legacy solver rebuilds it next tick.  A
        // reference graph can similarly be stale while its fracture decision
        // is being published.  Compare graph representation only once the
        // oracle's revision is current; object ownership is always compared.
        bool referenceCurrent = refGraph != referenceSolver.graphs.end() &&
                                refGraph->second.revision == object.revision;
        if (referenceCurrent)
            require(graph != solver.graphs.end(), "production fragment graph is missing");
        if (referenceCurrent) {
            require(graph->second.revision == it->second.revision,
                    "production graph revision is stale");
            compareGraph(refGraph->second, graph->second);
        }
    }
    for (size_t i = 0; i < reference.world.joints.size(); ++i) {
        const auto &a = reference.world.joints[i];
        const auto &b = actual.world.joints[i];
        require(a.a == b.a && a.b == b.b && near(a.anchor, b.anchor), "joint target differs");
    }
    require(reference.events.size() == actual.events.size(), "fracture event count differs");
    for (size_t i = 0; i < reference.events.size(); ++i)
        require(reference.events[i].type == actual.events[i].type &&
                    reference.events[i].entity == actual.events[i].entity &&
                    reference.events[i].message == actual.events[i].message,
                "fracture decision tick differs");
}

void differentialFatigueAndFracture() {
    Engine reference, actual;
    configureCantilever(reference);
    configureCantilever(actual);
    test::ReferenceStructure oracle;
    Structure solver;
    EntityId id = 1;
    std::map<EntityId, std::map<IVec3, Vec3>> forces;
    forces[id][{9, 0, 0}] = {0, -100.f, 0};
    bool fractured = false;
    for (int tick = 0; tick < 30; ++tick) {
        oracle.update(reference, Step, forces);
        solver.update(actual, Step, forces);
        compareState(reference, actual, oracle, solver);
        fractured |= reference.world.objects.size() > 1;
        if (fractured) {
            oracle.update(reference, Step, {});
            solver.update(actual, Step, {});
            compareState(reference, actual, oracle, solver);
            break;
        }
    }
    require(fractured, "fatigue fixture did not fracture");
}

void prepareSeedAndLocalInvalidation() {
    Engine e;
    e.world = world();
    for (MaterialId id : {MaterialId(1), MaterialId(2)})
        e.world.materials[id].tension = e.world.materials[id].compression =
            e.world.materials[id].shear = e.world.materials[id].bending = 1.e9f;
    const EntityId id = e.world.spawn("regions", line(-17, 17, true), {}, false);
    auto &object = e.world.objects.at(id);
    object.anchors.insert({-17, 0, 0});
    Structure solver;
    Stats initial{};
    auto graph = solver.prepare(object, e.world, nullptr, &initial);
    require(initial.structuralRegionRebuilds > 0 && initial.structuralCellsScanned > 0,
            "initial structural graph did not report a region build");
    const auto initialRebuilds = initial.structuralRegionRebuilds;
    solver.seed(id, std::move(graph));
    solver.update(e, Step, {});
    require(e.stats.structuralRegionRebuilds == 0 && e.stats.structuralCacheBytes > 0,
            "seeded graph rebuilt on its first tick");
    solver.update(e, Step, {});
    require(e.stats.structuralRegionRebuilds == 0, "unchanged graph rebuilt");

    Object changed = object;
    changed.grid.set({0, 0, 0}, 1);
    ++changed.revision;
    Stats local{};
    auto localGraph = solver.prepare(changed, e.world, &solver.graphs.at(id), &local);
    require(local.structuralRegionRebuilds > 0 && local.structuralRegionRebuilds < initialRebuilds,
            "single-cell edit did not invalidate only local structural regions");
    require(local.structuralCellsScanned < initial.structuralCellsScanned,
            "single-cell edit scanned every structural region");

    Object anchorsOnly = changed;
    anchorsOnly.anchors.insert({0, 0, 0});
    ++anchorsOnly.revision;
    Stats anchorStats{};
    auto anchorGraph = solver.prepare(anchorsOnly, e.world, &localGraph, &anchorStats);
    require(anchorStats.structuralRegionRebuilds == 0 && anchorStats.structuralCellsScanned == 0 &&
                anchorStats.structuralCacheHits > 0,
            "anchor-only edit rebuilt structural regions");
    // Exercise the production cell-to-node lookup on negative positions and
    // each 8-cell region boundary against the frozen map oracle.
    test::ReferenceStructure oracle;
    Engine reference;
    reference.world = e.world;
    reference.world.objects.at(id) = anchorsOnly;
    oracle.update(reference, Step, {});
    compareGraph(oracle.graphs.at(id), anchorGraph);
    for (int x : {-17, -16, -9, -8, -1, 0, 7, 8, 15, 16, 17})
        require(anchorGraph.nodeAt({x, 0, 0}) == oracle.graphs.at(id).lookup.at({x, 0, 0}),
                "region label lookup changed at a signed 8-cell boundary");
}

void makeStrong(World &w) {
    for (MaterialId id : {MaterialId(1), MaterialId(2)})
        w.materials[id].tension = w.materials[id].compression = w.materials[id].shear =
            w.materials[id].bending = 1.e9f;
}
VoxelGrid signedThreeDimensionalFixture() {
    VoxelGrid grid;
    auto paint = [&](IVec3 p) {
        // A deterministic pseudo-random material field makes a varied node
        // graph while preserving a dense, reproducible 3D fixture.
        uint32_t hash = uint32_t(int64_t(p.x) * 73856093) ^ uint32_t(int64_t(p.y) * 19349663) ^
                        uint32_t(int64_t(p.z) * 83492791);
        hash ^= hash >> 16;
        hash *= 0x7feb352dU;
        grid.set(p, (hash & 1) ? 1 : 2);
    };
    // The beam crosses -16/-8/0/8/16 regions and the 32-cell chunk origin.
    for (int x = -17; x <= 17; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                paint({x, y, z});
    // Perpendicular arms make the same signed region/chunk boundaries live
    // on every axis rather than testing an effectively one-dimensional case.
    for (int y = -17; y <= 17; ++y)
        paint({0, y, 0});
    for (int z = -17; z <= 17; ++z)
        paint({0, 0, z});
    grid.clearDirty();
    return grid;
}
void configureThreeDimensional(Engine &e) {
    e.world = world();
    makeStrong(e.world);
    auto id = e.world.spawn("signed 3d regions", signedThreeDimensionalFixture(), {}, false);
    e.world.objects.at(id).anchors.insert({-17, 0, 0});
}
void stepAndCompare(Engine &reference, Engine &actual, test::ReferenceStructure &oracle,
                    Structure &solver,
                    const std::map<EntityId, std::map<IVec3, Vec3>> &forces = {}) {
    oracle.update(reference, Step, forces);
    solver.update(actual, Step, forces);
    compareState(reference, actual, oracle, solver);
}
void setDamage(test::ReferenceStructure &oracle, Structure &solver, EntityId id, float damage) {
    for (auto &bond : oracle.graphs.at(id).bonds)
        bond.damage = damage;
    for (auto &bond : solver.graphs.at(id).bonds)
        bond.damage = damage;
}
void requireNoDamage(const test::ReferenceStructure &oracle, const Structure &solver, EntityId id,
                     const char *message) {
    for (const auto &bond : oracle.graphs.at(id).bonds)
        require(bond.damage == 0, message);
    for (const auto &bond : solver.graphs.at(id).bonds)
        require(bond.damage == 0, message);
}
void threeDimensionalDifferentialEdits() {
    Engine reference, actual;
    configureThreeDimensional(reference);
    configureThreeDimensional(actual);
    test::ReferenceStructure oracle;
    Structure solver;
    constexpr EntityId id = 1;

    // Baseline exactness also checks signed 8-cell and 32-cell boundaries.
    stepAndCompare(reference, actual, oracle, solver);
    const auto &referenceGraph = oracle.graphs.at(id);
    const auto &actualGraph = solver.graphs.at(id);
    compareGraph(referenceGraph, actualGraph);
    const auto baselineRegionRebuilds = actual.stats.structuralRegionRebuilds;
    const auto baselineCellsScanned = actual.stats.structuralCellsScanned;
    require(baselineRegionRebuilds > 0 && baselineCellsScanned > 0,
            "3D baseline did not build structural regions");
    for (auto [cell, ignored] : reference.world.objects.at(id).grid.occupied()) {
        (void)ignored;
        require(actualGraph.nodeAt(cell) == referenceGraph.lookup.at(cell),
                "3D region node label differs from reference");
    }

    // Recovery happens in the solve loop; a following paint must discard the
    // accumulated fatigue when both solvers rebuild the changed graph.
    setDamage(oracle, solver, id, .1f);
    stepAndCompare(reference, actual, oracle, solver);
    require(near(oracle.graphs.at(id).bonds.front().damage, .1f - Step * .1f),
            "reference fatigue recovery fixture is invalid");

    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.grid.set({-8, 0, 0}, 1); // paint across a signed region boundary
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);
    require(actual.stats.structuralRegionRebuilds > 0 &&
                actual.stats.structuralRegionRebuilds < baselineRegionRebuilds &&
                actual.stats.structuralCellsScanned < baselineCellsScanned,
            "3D paint invalidated more than local structural regions");
    requireNoDamage(oracle, solver, id, "paint did not reset fatigue");

    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.grid.set({-1, 1, 1}, 0); // remove at the signed 32-cell boundary
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);

    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.anchors.insert({8, 0, 0});
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);

    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.seams.insert(seam({0, 0, 0}, {1, 0, 0})); // internal 8-cell region seam
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);
    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.seams.insert(seam({7, 0, 0}, {8, 0, 0})); // external region seam
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);

    // Refreshing material data preserves membership but recalculates masses
    // and resets fatigue.  Clearing the oracle forces the legacy rebuild.
    setDamage(oracle, solver, id, .1f);
    size_t materialOneNode = 0;
    while (solver.graphs.at(id).nodes[materialOneNode].material != 1)
        ++materialOneNode;
    const float previousMass = solver.graphs.at(id).nodes[materialOneNode].mass;
    for (Engine *e : {&reference, &actual}) {
        e->world.materials[1].density = 2300.f;
        e->world.materials[1].tension = e->world.materials[1].compression =
            e->world.materials[1].shear = e->world.materials[1].bending = 8.e8f;
    }
    solver.materialsChanged(actual.world);
    oracle.clear();
    stepAndCompare(reference, actual, oracle, solver);
    requireNoDamage(oracle, solver, id, "material refresh did not reset fatigue");
    require(near(oracle.graphs.at(id).nodes.front().mass, solver.graphs.at(id).nodes.front().mass),
            "material density refresh changed node mass");
    require(near(solver.graphs.at(id).nodes[materialOneNode].mass, previousMass * 2.3f),
            "material density refresh did not recompute mass");

    // Empty-object cleanup should erase both the object and its cached graph;
    // clear() then verifies reset remains safe after deletion.
    for (Engine *e : {&reference, &actual}) {
        auto &o = e->world.objects.at(id);
        o.grid = VoxelGrid{};
        ++o.revision;
    }
    stepAndCompare(reference, actual, oracle, solver);
    require(reference.world.objects.empty() && actual.world.objects.empty() &&
                oracle.graphs.empty() && solver.graphs.empty(),
            "empty structural object was not cleaned up");
    oracle.clear();
    solver.clear();
    require(oracle.graphs.empty() && solver.graphs.empty(), "structural cache reset failed");
}

void structuralBudgetMatchesReference() {
    Engine e;
    e.world = world();
    Object o;
    o.grid = line(0, 10000, true);
    Structure solver;
    test::ReferenceStructure oracle;
    bool referenceThrew = false, actualThrew = false;
    try {
        oracle.validate(o, e.world);
    } catch (const std::exception &) {
        referenceThrew = true;
    }
    try {
        solver.validate(o, e.world);
    } catch (const std::exception &) {
        actualThrew = true;
    }
    require(referenceThrew && actualThrew, "structural node budget changed");
}
void configureFragmentBudget(Engine &e) {
    e.world = world();
    VoxelGrid split;
    split.set({0, 0, 0}, 1);
    split.set({2, 0, 0}, 1);
    split.clearDirty();
    const auto source = e.world.spawn("would split", std::move(split), {}, false);
    // Keep both components attached so gravity cannot distract from the
    // budget rejection, and make every occupant persistent for tick two.
    e.world.objects.at(source).anchors = {{0, 0, 0}, {2, 0, 0}};
    for (EntityId id = 2; id <= 4096; ++id) {
        Object placeholder;
        placeholder.id = id;
        placeholder.grid.set({0, 0, 0}, 1);
        placeholder.grid.clearDirty();
        placeholder.anchors.insert({0, 0, 0});
        e.world.objects.emplace(id, std::move(placeholder));
    }
    e.world.nextEntity = 4097;
}
void fragmentBudgetMatchesReference() {
    Engine reference, actual;
    configureFragmentBudget(reference);
    configureFragmentBudget(actual);
    test::ReferenceStructure oracle;
    Structure solver;
    oracle.update(reference, Step, {});
    solver.update(actual, Step, {});
    compareState(reference, actual, oracle, solver);
    require(reference.events.size() == 1 && reference.events.front().type == "budget",
            "reference fragment budget fixture did not report budget exhaustion");
    oracle.update(reference, Step, {});
    solver.update(actual, Step, {});
    compareState(reference, actual, oracle, solver);
    require(reference.events.size() == 1 && actual.events.size() == 1 &&
                actual.stats.structuralRegionRebuilds == 0,
            "stable over-budget topology repeated connectivity work or budget event");
}
std::map<IVec3, std::vector<CollisionBox>> collisionSeedFor(const VoxelGrid &grid, float size) {
    std::map<IVec3, std::vector<CollisionBox>> result;
    for (const auto &[coordinate, chunk] : grid.chunks()) {
        (void)chunk;
        result[coordinate] = collisionBoxesChunk(grid, coordinate, size);
    }
    return result;
}
void transactionalFragmentCollisionCacheSeed() {
    World w = world();
    VoxelGrid original;
    original.set({0, 0, 0}, 1);
    original.set({32, 0, 0}, 1);
    original.clearDirty();
    const EntityId source = w.spawn("two chunk source", original, {}, false);
    Physics physics;
    auto originalBoxes = collisionSeedFor(w.objects.at(source).grid, w.voxelSize);
    physics.seedCollisionCache(source, w.objects.at(source).grid, w.voxelSize, originalBoxes);
    physics.sync(w);
    const auto cacheBytes = physics.collisionCacheBytes;
    const auto chunkRebuilds = physics.collisionChunkRebuilds;
    const auto bodyRebuilds = physics.collisionBodyRebuilds;
    const auto bodyCount = physics.collisionBodyCount;
    require(bodyCount == 2, "two-chunk source did not publish both static bodies");

    // The first seed is valid for its one-chunk replacement grid.  The second
    // crosses the collision budget, so staging must leave the live two-chunk
    // source cache and its already-published bodies untouched.
    VoxelGrid partial;
    partial.set({0, 0, 0}, 1);
    partial.clearDirty();
    auto partialBoxes = collisionSeedFor(partial, w.voxelSize);
    VoxelGrid invalid;
    invalid.set({0, 0, 0}, 1);
    invalid.clearDirty();
    std::map<IVec3, std::vector<CollisionBox>> excessive;
    excessive[{0, 0, 0}].resize(32769);
    bool rejected = false;
    try {
        physics.seedFragmentCollisionCaches(
            {{source, &partial, &partialBoxes}, {source + 1, &invalid, &excessive}}, w.voxelSize);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "over-budget staged collision batch was accepted");
    require(physics.collisionCacheBytes == cacheBytes &&
                physics.collisionChunkRebuilds == chunkRebuilds &&
                physics.collisionBodyRebuilds == bodyRebuilds &&
                physics.collisionBodyCount == bodyCount,
            "failed collision-cache staging changed public diagnostics");
    physics.sync(w);
    require(physics.collisionCacheBytes == cacheBytes &&
                physics.collisionChunkRebuilds == chunkRebuilds &&
                physics.collisionBodyRebuilds == bodyRebuilds &&
                physics.collisionBodyCount == bodyCount,
            "failed collision-cache staging changed the original synced body");

    physics.seedFragmentCollisionCaches({{source, &partial, &partialBoxes}}, w.voxelSize);
    w.objects.at(source).grid = partial;
    ++w.objects.at(source).revision;
    physics.sync(w);
    require(physics.collisionBodyCount == 1 && physics.collisionCacheBytes < cacheBytes,
            "valid staged collision cache did not publish the changed source body");
}
void allocationFailureLeavesFragmentCollisionCacheUntouched() {
    World w = world();
    VoxelGrid original;
    original.set({0, 0, 0}, 1);
    original.set({32, 0, 0}, 1);
    original.clearDirty();
    const EntityId source = w.spawn("fault source", original, {}, false);
    Physics physics;
    auto originalBoxes = collisionSeedFor(w.objects.at(source).grid, w.voxelSize);
    physics.seedCollisionCache(source, w.objects.at(source).grid, w.voxelSize, originalBoxes);
    physics.sync(w);
    const auto cacheBytes = physics.collisionCacheBytes;
    const auto chunkRebuilds = physics.collisionChunkRebuilds;
    const auto bodyRebuilds = physics.collisionBodyRebuilds;
    const auto bodyCount = physics.collisionBodyCount;

    VoxelGrid first;
    first.set({0, 0, 0}, 1);
    first.clearDirty();
    auto firstBoxes = collisionSeedFor(first, w.voxelSize);
    VoxelGrid second;
    second.set({0, 0, 0}, 1);
    second.clearDirty();
    auto secondBoxes = collisionSeedFor(second, w.voxelSize);
    const std::vector<Physics::CollisionSeed> seeds{{source, &first, &firstBoxes},
                                                    {source + 1, &second, &secondBoxes}};

    bool sawFailure = false, succeeded = false;
    for (std::ptrdiff_t failAfter = 0; failAfter < 96 && !succeeded; ++failAfter) {
        bool failed = false;
        {
            ScopedAllocationFailure fault(failAfter);
            try {
                physics.seedFragmentCollisionCaches(seeds, w.voxelSize);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                failed = true;
                sawFailure = true;
            }
        }
        if (failed)
            require(physics.collisionCacheBytes == cacheBytes &&
                        physics.collisionChunkRebuilds == chunkRebuilds &&
                        physics.collisionBodyRebuilds == bodyRebuilds &&
                        physics.collisionBodyCount == bodyCount,
                    "allocation failure partially published fragment collision caches");
    }
    require(sawFailure && succeeded,
            "fault injection did not cover both failed and successful collision-cache staging");
}
void checkerboardBlock(VoxelGrid &grid, int firstX, int width, int height, int depth) {
    for (int z = 0; z < depth; ++z)
        for (int y = 0; y < height; ++y)
            for (int x = firstX; x < firstX + width; ++x)
                grid.set({x, y, z}, ((x + y + z) & 1) ? 1 : 2);
}
void configureOversizedDisconnected(Engine &e) {
    e.world = world();
    makeStrong(e.world);
    VoxelGrid grid;
    // 20 * 20 * 15 material-checkerboard cells per connected occupancy block.
    // The one-cell gap makes a 12k-node source that splits into valid 6k-node
    // fragments before either fragment needs structural validation.
    checkerboardBlock(grid, 0, 20, 20, 15);
    checkerboardBlock(grid, 21, 20, 20, 15);
    grid.clearDirty();
    const auto id = e.world.spawn("oversized disconnected", std::move(grid), {}, false);
    e.world.objects.at(id).anchors = {{0, 0, 0}, {21, 0, 0}};
}
void oversizedDisconnectedSplitsBeforeBudgetValidation() {
    Engine reference, actual;
    configureOversizedDisconnected(reference);
    configureOversizedDisconnected(actual);
    test::ReferenceStructure oracle;
    Structure solver;
    oracle.update(reference, Step, {});
    solver.update(actual, Step, {});
    compareState(reference, actual, oracle, solver);
    require(reference.world.objects.size() == 2 && actual.world.objects.size() == 2 &&
                reference.events.size() == 1 && actual.events.size() == 1 &&
                reference.events.front().type == "fracture" &&
                actual.events.front().type == "fracture",
            "oversized disconnected source did not split into valid fragments");
    // The oracle defers the second fragment graph until its next tick.
    oracle.update(reference, Step, {});
    solver.update(actual, Step, {});
    compareState(reference, actual, oracle, solver);
}
void connectedOversizedStillRejectsWithoutMutation() {
    auto configure = [](Engine &e) {
        e.world = world();
        makeStrong(e.world);
        VoxelGrid grid;
        checkerboardBlock(grid, 0, 20, 20, 30); // 12,000 nodes, one component.
        grid.clearDirty();
        const auto id = e.world.spawn("oversized connected", std::move(grid), {}, false);
        e.world.objects.at(id).anchors.insert({0, 0, 0});
    };
    Engine reference, actual;
    configure(reference);
    configure(actual);
    const auto referenceChecksum = reference.world.checksum();
    const auto actualChecksum = actual.world.checksum();
    test::ReferenceStructure oracle;
    Structure solver;
    bool referenceRejected = false, actualRejected = false;
    try {
        oracle.update(reference, Step, {});
    } catch (const std::exception &) {
        referenceRejected = true;
    }
    try {
        solver.update(actual, Step, {});
    } catch (const std::exception &) {
        actualRejected = true;
    }
    require(referenceRejected && actualRejected, "connected oversized source was accepted");
    require(reference.world.checksum() == referenceChecksum &&
                actual.world.checksum() == actualChecksum && reference.world.objects.size() == 1 &&
                actual.world.objects.size() == 1 && oracle.graphs.empty() &&
                solver.graphs.empty() && reference.events.empty() && actual.events.empty(),
            "connected oversized rejection changed world ownership or caches");
}
} // namespace

int main() {
    try {
        differentialFatigueAndFracture();
        prepareSeedAndLocalInvalidation();
        threeDimensionalDifferentialEdits();
        structuralBudgetMatchesReference();
        fragmentBudgetMatchesReference();
        transactionalFragmentCollisionCacheSeed();
        allocationFailureLeavesFragmentCollisionCacheUntouched();
        oversizedDisconnectedSplitsBeforeBudgetValidation();
        connectedOversizedStillRejectsWithoutMutation();
        std::cout << "structure performance tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "structure performance tests failed: " << error.what() << '\n';
        return 1;
    }
}
