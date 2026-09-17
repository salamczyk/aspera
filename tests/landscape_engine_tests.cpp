#include "aspera/engine.hpp"
#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aspera;
namespace {
void require(bool value, const char *message) {
    if (!value)
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
    s.minimum = {-16, -8, -16};
    s.maximum = {48, 24, 16};
    s.surfaceHeight = 6;
    s.hillAmplitude = 5;
    s.featureScale = 24;
    return s;
}
Event finish(Engine &e, uint64_t command) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        e.step();
        for (auto &event : e.events)
            if (event.command == command && (event.type == "committed" || event.type == "rejected"))
                return event;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("landscape command timed out");
}
void generationLifecycle() {
    Engine e;
    e.simulating = false;
    auto baseline = e.world.checksum(), allocator = e.world.nextEntity;
    auto invalid = small();
    invalid.rockMaterial = 65535;
    rejects([&] { e.generateLandscape(invalid); }, "unknown material rejected before publication");
    require(e.world.checksum() == baseline && e.world.nextEntity == allocator,
            "failed generation leaves state and allocator unchanged");
    auto id = e.generateLandscape(small());
    require(e.world.objects.at(id).anchors.size() == 64 * 32, "engine publishes anchored terrain");
    auto snapshot = encodeWorld(e.world);
    require(decodeWorld(snapshot).checksum() == e.world.checksum(),
            "baked landscape uses existing exact snapshot format");
    const auto before = e.world.objects.at(id).grid.count();
    Command cut;
    cut.type = CommandType::SphereDamage;
    cut.entity = id;
    cut.point = {3.2f, .05f, 0};
    cut.radius = .3f;
    cut.strength = 100000;
    require(finish(e, e.submit(cut)).type == "committed",
            "terrain accepts radial edits across X chunk boundary");
    require(e.world.objects.at(id).grid.count() < before, "radial terrain edit removes voxels");
    e.world = decodeWorld(snapshot);
    e.resetPhysics();
    require(e.world.objects.at(id).grid.count() == before,
            "world replacement restores edited chunk content");
    auto request = e.requestLandscape(small());
    ++e.world.revision;
    require(finish(e, request).type == "rejected" && e.world.objects.size() == 1,
            "stale generated result cannot overwrite intervening world change");
    request = e.requestLandscape(small());
    e.resetPhysics();
    require(finish(e, request).type == "rejected" && e.world.objects.size() == 1,
            "reset cancels pending terrain publication");
    request = e.requestLandscape(small());
    e.simulating = true;
    require(finish(e, request).type == "rejected", "starting Play cancels editor generation");
    e.simulating = false;
    auto result = finish(e, e.requestLandscape(small()));
    require(result.type == "committed" && e.world.objects.contains(result.entity),
            "background generation publishes once at a tick boundary");
    auto count = e.world.objects.size();
    for (int i = 0; i < 5; ++i)
        e.step();
    require(e.world.objects.size() == count, "completed generation is not published again");
    LandscapeData bad;
    for (int x = 0; x < 32; ++x)
        for (int y = 0; y < 32; ++y)
            for (int z = 0; z < 32; ++z)
                if ((x + y + z) % 2 == 0)
                    bad.grid.set({x, y, z}, 7);
    bad.anchors.insert({0, 0, 0});
    auto revision = e.world.revision;
    rejects([&] { e.publishLandscape(bad); },
            "structural node budget checked before publishing terrain");
    require(e.world.objects.size() == count && e.world.revision == revision,
            "derived-budget failure is atomic");
}
void collisionCaches() {
    Engine e;
    VoxelGrid grid;
    for (int x = 30; x < 35; ++x)
        grid.set({x, 0, 0}, 1);
    auto id = e.world.spawn("two chunks", std::move(grid));
    Physics physics;
    physics.sync(e.world);
    require(physics.collisionBodyCount == 2, "static voxel object uses two chunk bodies");
    auto chunks = physics.collisionChunkRebuilds, bodies = physics.collisionBodyRebuilds;
    physics.sync(e.world);
    require(physics.collisionChunkRebuilds == chunks && physics.collisionBodyRebuilds == bodies,
            "unchanged collision sync performs no rebuilds");
    e.world.objects.at(id).grid.set({30, 0, 0}, 2);
    physics.sync(e.world);
    require(physics.collisionChunkRebuilds == chunks + 1 &&
                physics.collisionBodyRebuilds == bodies + 1,
            "one chunk edit replaces only its cached shape and body");
    e.world.objects.at(id).grid.set({30, 0, 0}, 0);
    e.world.objects.at(id).grid.set({31, 0, 0}, 0);
    physics.sync(e.world);
    require(physics.collisionBodyCount == 1, "empty chunk removes its static body");
    e.world.objects.at(id).grid.set({31, 0, 0}, 1);
    e.world.objects.at(id).dynamic = true;
    physics.sync(e.world);
    require(physics.collisionBodyCount == 1,
            "dynamic fragment has one compound body across chunks");
    physics.step(e.world, 1.f / 60.f);
    require(e.world.objects.at(id).velocity.y < 0, "dynamic chunk compound responds to gravity");
    require(std::abs(e.world.objects.at(id).transform.position.x) < .00001f &&
                std::abs(e.world.objects.at(id).transform.position.z) < .00001f,
            "compound center of mass never shifts the object origin");
    auto baseline = e.world;
    const auto bodyRebuilds = physics.collisionBodyRebuilds;
    e.world.objects.at(id).grid.set({31, 0, 0}, 2);
    VoxelGrid excessive;
    for (int x = 0; x < 64; ++x)
        for (int y = 0; y < 32; ++y)
            for (int z = 0; z < 64; ++z)
                if ((x + y + z) % 2 == 0)
                    excessive.set({x, y, z}, 1);
    e.world.spawn("excessive compound", std::move(excessive));
    rejects([&] { physics.sync(e.world); },
            "aggregate collision complexity is checked before body publication");
    e.world = std::move(baseline);
    physics.sync(e.world);
    require(physics.collisionBodyRebuilds == bodyRebuilds && physics.collisionBodyCount == 1,
            "failed multi-object replacement preserves every original body");
    e.world.objects.clear();
    physics.sync(e.world);
    require(physics.collisionBodyCount == 0 && physics.collisionCacheBytes == 0,
            "deletion clears all collision cache resources");
    VoxelGrid unpublished;
    unpublished.set({0, 0, 0}, 1);
    auto unpublishedId = e.world.spawn("unpublished cache", unpublished);
    std::map<IVec3, std::vector<CollisionBox>> prepared;
    prepared[{0, 0, 0}] = collisionBoxesChunk(unpublished, {0, 0, 0}, e.world.voxelSize);
    physics.seedCollisionCache(unpublishedId, unpublished, e.world.voxelSize, prepared);
    e.world.objects.erase(unpublishedId);
    physics.sync(e.world);
    require(physics.collisionCacheBytes == 0,
            "deletion before first physics sync clears seeded collision caches");
}
void crossChunkCollapse() {
    Engine e;
    auto material = e.world.materials[1];
    material.tension = material.compression = material.shear = material.bending = 1e12f;
    e.setMaterial(1, material);
    VoxelGrid grid;
    grid.set({29, 0, 0}, 1);
    grid.set({29, 1, 0}, 1);
    for (int x = 29; x < 36; ++x)
        grid.set({x, 2, 0}, 1);
    auto id = e.world.spawn("cross chunk roof", std::move(grid));
    e.world.objects.at(id).anchors.insert({29, 0, 0});
    e.step();
    require(e.world.objects.size() == 1 && !e.world.objects.at(id).dynamic,
            "connected cross-chunk roof remains supported");
    Command cut;
    cut.entity = id;
    cut.type = CommandType::SetVoxel;
    cut.cell = {29, 1, 0};
    cut.material = 0;
    require(finish(e, e.submit(cut)).type == "committed", "support removal commits");
    size_t count = 0;
    bool spanning = false;
    for (auto &[other, object] : e.world.objects) {
        count += object.grid.count();
        if (object.dynamic)
            spanning = object.grid.get({31, 2, 0}) && object.grid.get({32, 2, 0}) &&
                       object.anchors.empty();
    }
    require(count == 8 && spanning && e.world.objects.size() == 2,
            "unsupported roof stays one cross-chunk fragment with conserved mass");
    require(e.world.objects.at(id).anchors.contains({29, 0, 0}),
            "original foundation retains its anchor");
    auto bytes = encodeWorld(e.world);
    require(decodeWorld(bytes).checksum() == e.world.checksum(),
            "fragment ownership survives snapshots");
    Engine strong, weak;
    auto setup = [](Engine &engine, bool brace) {
        auto m = engine.world.materials[1];
        m.density = 1000;
        m.bending = 500000;
        m.tension = m.compression = m.shear = 1e9f;
        engine.setMaterial(1, m);
        VoxelGrid beam;
        for (int x = 24; x < 48; ++x)
            beam.set({x, 0, 0}, 1);
        auto id = engine.world.spawn("cross chunk beam", std::move(beam));
        engine.world.objects.at(id).anchors.insert({24, 0, 0});
        if (brace)
            engine.world.objects.at(id).anchors.insert({47, 0, 0});
    };
    setup(strong, true);
    setup(weak, false);
    for (int i = 0; i < 90; ++i) {
        strong.step();
        weak.step();
    }
    require(strong.world.objects.size() == 1 && weak.world.objects.size() > 1,
            "material overload fractures across chunk seams while braced roof remains stable");
}
void terrainJointOwnership() {
    Engine e;
    VoxelGrid roof;
    roof.set({29, 0, 0}, 7);
    roof.set({29, 1, 0}, 7);
    for (int x = 29; x < 36; ++x)
        roof.set({x, 2, 0}, 7);
    auto terrain = e.world.spawn("jointed roof", std::move(roof));
    e.world.objects.at(terrain).anchors.insert({29, 0, 0});
    VoxelGrid base;
    base.set({0, 0, 0}, 7);
    auto fixture = e.world.spawn("joint anchor", std::move(base), {{3.35f, 0, .05f}, {}});
    e.world.objects.at(fixture).anchors.insert({0, 0, 0});
    e.world.joints.push_back({1, terrain, fixture, {3.35f, .25f, .05f}, false, 1e9f});
    e.step();
    Command cut;
    cut.entity = terrain;
    cut.type = CommandType::SetVoxel;
    cut.cell = {29, 1, 0};
    cut.material = 0;
    require(finish(e, e.submit(cut)).type == "committed", "jointed terrain support cut commits");
    require(e.world.joints.size() == 1 && e.world.joints.front().a != terrain &&
                e.world.objects.at(e.world.joints.front().a).grid.get({33, 2, 0}) == 7 &&
                e.world.objects.at(e.world.joints.front().a).dynamic,
            "joint follows attachment cell into a cross-chunk dynamic fragment");
}
void luaLandscape() {
    auto path = std::filesystem::temp_directory_path() / "aspera-landscape-lua-test";
    std::filesystem::create_directories(path);
    std::ofstream(path / "project.lua") << "return {name='Landscape test'}\n";
    std::ofstream(path / "scene.lua")
        << "local "
           "id=engine.generate_landscape({seed=9,minimum={x=-8,y=-8,z=-8},maximum={x=8,y=16,z=8},"
           "surface_height=3})\nengine.property('terrain',id)\n";
    std::ofstream(path / "server.lua") << "function update(dt) engine.generate_landscape({}) end\n";
    Engine e;
    e.loadProject(path);
    require(e.world.objects.contains(EntityId(e.world.properties.at("terrain"))),
            "Lua generator returns a usable terrain entity");
    e.step();
    bool error = false;
    for (auto &event : e.events)
        error |= event.type == "script_error";
    require(error && e.world.objects.size() == 1,
            "Lua runtime cannot invoke scene-only generation");
    std::filesystem::remove_all(path);
}
void benchmark(size_t runs = 5) {
    std::cout << std::unitbuf;
    require(runs > 0, "benchmark needs at least one run");
    struct Tick {
        double frameMs{};
        Stats stats{};
    };
    struct Run {
        double generationMs{};
        Tick initialTick{};
        Tick postPausedDigTick{};
        size_t voxels{}, chunks{}, snapshotBytes{};
        uint32_t collisionBodies{};
        std::vector<double> pausedEdits;
        std::vector<Tick> idleTicks, activeTicks;
        int detachmentTicks{};
        size_t fallingRoofVoxels{};
    };
    auto percentile = [](std::vector<double> values, double fraction) {
        require(!values.empty(), "benchmark percentile has samples");
        std::sort(values.begin(), values.end());
        const size_t index =
            std::min(values.size() - 1, size_t(std::ceil(fraction * values.size())) - 1);
        return values[index];
    };
    auto printSeries = [&](const char *name, const std::vector<double> &values) {
        std::cout << name << "_samples=" << values.size() << " p50_ms=" << percentile(values, .50)
                  << " p95_ms=" << percentile(values, .95) << " max_ms=" << percentile(values, 1.)
                  << '\n';
    };
    auto recordTick = [](const Engine &engine) { return Tick{engine.stats.frameMs, engine.stats}; };

    constexpr int idleTicksPerRun = 8;
    constexpr int pausedEditsPerRun = 12;
    constexpr int maximumCollapseTicks = 90;
    constexpr size_t expectedRoofVoxels = 1152;
    constexpr int expectedDetachmentTicks = 16;
    std::vector<Run> results;
    results.reserve(runs);

    for (size_t run = 0; run < runs; ++run) {
        Run result;
        Engine terrain;
        terrain.simulating = false;
        auto start = std::chrono::steady_clock::now();
        const auto terrainId = terrain.generateLandscape({});
        result.generationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        result.voxels = terrain.world.objects.at(terrainId).grid.count();
        result.chunks = terrain.world.objects.at(terrainId).grid.chunks().size();
        result.snapshotBytes = encodeWorld(terrain.world).size();

        // Initialization has its own sample: it establishes collision/structure caches before
        // measuring steady-state idle ticks.
        terrain.simulating = true;
        terrain.step();
        result.initialTick = recordTick(terrain);
        result.collisionBodies = terrain.stats.collisionBodyCount;
        for (int tick = 0; tick < idleTicksPerRun; ++tick) {
            terrain.step();
            result.idleTicks.push_back(recordTick(terrain));
        }

        terrain.simulating = false;
        for (int edit = 0; edit < pausedEditsPerRun; ++edit) {
            Command cut;
            cut.type = CommandType::SphereDamage;
            cut.entity = terrainId;
            cut.point = {float(edit) * .2f, 0, 0};
            cut.radius = .3f;
            cut.strength = 100000;
            start = std::chrono::steady_clock::now();
            require(finish(terrain, terrain.submit(cut)).type == "committed",
                    "paused benchmark dig commits");
            result.pausedEdits.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        terrain.simulating = true;
        terrain.step();
        result.postPausedDigTick = recordTick(terrain);

        Engine demo;
        demo.loadProject("games/landscape");
        demo.simulating = false;
        Command removeSupport;
        removeSupport.type = CommandType::SphereDamage;
        removeSupport.entity = EntityId(demo.world.properties.at("landscape.terrain"));
        removeSupport.point = {5, -2, -6.8f};
        removeSupport.radius = .6f;
        removeSupport.strength = 100000;
        require(finish(demo, demo.submit(removeSupport)).type == "committed",
                "paused quarry support removal commits");
        demo.simulating = true;
        for (int tick = 0; tick < maximumCollapseTicks && !result.fallingRoofVoxels; ++tick) {
            demo.step();
            result.activeTicks.push_back(recordTick(demo));
            for (const auto &[fragment, object] : demo.world.objects) {
                if (!object.dynamic)
                    continue;
                for (auto [p, material] : object.grid.occupied())
                    result.fallingRoofVoxels += p.x >= 16 && p.x <= 51 && p.y >= -8 && p.y <= -5 &&
                                                p.z >= -72 && p.z <= -65;
            }
            if (result.fallingRoofVoxels)
                result.detachmentTicks = tick + 1;
        }
        require(result.fallingRoofVoxels == expectedRoofVoxels &&
                    result.detachmentTicks == expectedDetachmentTicks,
                "quarry collapse retains all 1152 roof voxels and tick-16 timing");

        results.push_back(std::move(result));
    }

    std::vector<double> generation, initial, postPausedDig, pausedEdits, idle, active, detachment,
        roofVoxels;
    std::vector<Tick> activeSamples;
    generation.reserve(results.size());
    initial.reserve(results.size());
    for (const auto &result : results) {
        generation.push_back(result.generationMs);
        initial.push_back(result.initialTick.frameMs);
        postPausedDig.push_back(result.postPausedDigTick.frameMs);
        pausedEdits.insert(pausedEdits.end(), result.pausedEdits.begin(), result.pausedEdits.end());
        detachment.push_back(result.detachmentTicks);
        roofVoxels.push_back(result.fallingRoofVoxels);
        for (const Tick &tick : result.idleTicks) {
            idle.push_back(tick.frameMs);
        }
        for (const Tick &tick : result.activeTicks) {
            active.push_back(tick.frameMs);
            activeSamples.push_back(tick);
        }
    }
    const auto worst =
        std::max_element(activeSamples.begin(), activeSamples.end(),
                         [](const Tick &a, const Tick &b) { return a.frameMs < b.frameMs; });
    require(worst != activeSamples.end(), "active benchmark produces tick samples");
    const Stats &worstStats = worst->stats;
    const Run &first = results.front();

    std::cout << "benchmark_runs=" << runs << " default_voxels=" << first.voxels
              << " occupied_chunks=" << first.chunks << " snapshot_bytes=" << first.snapshotBytes
              << " initial_collision_bodies=" << first.collisionBodies << '\n';
    printSeries("initialization_generation", generation);
    printSeries("initialization_first_tick", initial);
    printSeries("idle_tick", idle);
    printSeries("paused_edit_completion", pausedEdits);
    printSeries("post_paused_dig_tick", postPausedDig);
    printSeries("active_dig_collapse_tick", active);
    std::cout << "quarry_baseline_roof_voxels=" << expectedRoofVoxels
              << " quarry_baseline_detachment_ticks=" << expectedDetachmentTicks
              << " observed_roof_voxels_p50=" << percentile(roofVoxels, .50)
              << " observed_roof_voxels_max=" << percentile(roofVoxels, 1.)
              << " observed_detachment_ticks_p50=" << percentile(detachment, .50)
              << " observed_detachment_ticks_max=" << percentile(detachment, 1.) << '\n';
    std::cout << "worst_active_tick_ms=" << worst->frameMs
              << " stage_structure_total_ms=" << worstStats.structureMs
              << " stage_structure_preparation_ms=" << worstStats.structurePreparationMs
              << " stage_structure_connectivity_ms=" << worstStats.structureConnectivityMs
              << " stage_structure_solve_ms=" << worstStats.structureSolveMs
              << " stage_structure_fragment_ms=" << worstStats.structureFragmentMs
              << " stage_physics_publication_ms=" << worstStats.physicsPublicationMs
              << " stage_collision_preparation_ms=" << worstStats.collisionPreparationMs
              << " stage_physics_ms=" << worstStats.physicsMs << '\n';
    std::cout << "worst_active_structural_region_rebuilds=" << worstStats.structuralRegionRebuilds
              << " structural_cells_scanned=" << worstStats.structuralCellsScanned
              << " structural_cache_hits=" << worstStats.structuralCacheHits
              << " structural_cache_bytes=" << worstStats.structuralCacheBytes
              << " collision_chunk_rebuilds=" << worstStats.collisionChunkRebuilds
              << " collision_cache_bytes=" << worstStats.collisionCacheBytes << '\n';
}
} // namespace
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        size_t runs = 5;
        if (argc == 4 && std::string(argv[2]) == "--benchmark-runs") {
            try {
                runs = std::stoull(argv[3]);
            } catch (const std::exception &) {
                throw std::runtime_error("--benchmark-runs requires a positive integer");
            }
        } else if (argc != 2) {
            throw std::runtime_error(
                "usage: landscape_engine_tests --benchmark [--benchmark-runs N]");
        }
        benchmark(runs);
        return 0;
    }
    generationLifecycle();
    collisionCaches();
    crossChunkCollapse();
    terrainJointOwnership();
    luaLandscape();
    std::cout << "Landscape engine lifecycle, collision caches, and collapse passed\n";
}
