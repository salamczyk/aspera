#include "internal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace aspera;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
EntityId box(World &w, Vec3 position, IVec3 size, bool dynamic = false) {
    VoxelGrid grid;
    for (int x = 0; x < size.x; ++x)
        for (int y = 0; y < size.y; ++y)
            for (int z = 0; z < size.z; ++z)
                grid.set({x, y, z}, 1);
    auto id = w.spawn("fixture", std::move(grid), {position, {}}, dynamic);
    if (!dynamic)
        for (int x = 0; x < size.x; ++x)
            for (int z = 0; z < size.z; ++z)
                w.objects.at(id).anchors.insert({x, 0, z});
    return id;
}
void fixedSteps(Physics &physics, World &world, PlayerState &state, PlayerInput input, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        physics.sync(world);
        physics.movePlayer(state, input, 1.f / 60.f);
        physics.step(world, 1.f / 60.f);
    }
}
void locomotionAndEdits() {
    Engine engine;
    auto &world = engine.world;
    box(world, {-4, -.2f, -4}, {80, 2, 80});
    Physics physics;
    physics.sync(world);
    require(physics.playerClear({0, .03f, 0}), "standing spawn is clear");
    PlayerState state;
    state.id = 1;
    state.position = {0, .03f, 0};
    PlayerInput input;
    input.generation = 1;
    input.sequence = 1;
    fixedSteps(physics, world, state, input, 30);
    require(state.grounded && std::abs(state.position.y) < .06f, "settles on ground");
    auto baseline = state;
    input.moveZ = 1;
    fixedSteps(physics, world, state, input, 30);
    require(std::abs(state.position.z - baseline.position.z - 2) < .08f,
            "walks at four metres per second");
    physics.restorePlayer(baseline);
    state = baseline;
    input.moveZ = input.moveX = .70710678f;
    fixedSteps(physics, world, state, input, 30);
    require(std::abs(length(state.position - baseline.position) - 2) < .08f,
            "normalized diagonal speed");
    state = baseline;
    physics.restorePlayer(state);
    input.moveX = input.moveZ = 0;
    input.jump = true;
    fixedSteps(physics, world, state, input, 1);
    input.jump = false;
    fixedSteps(physics, world, state, input, 20);
    require(state.position.y > .8f, "jump leaves ground");
    fixedSteps(physics, world, state, input, 70);
    require(state.grounded, "jump lands");
    box(world, {-.6f, 0, 1}, {12, 30, 2});
    physics.sync(world);
    require(!physics.playerClear({0, .03f, 1.1f}), "spawn wall overlap rejected");
    state = baseline;
    physics.restorePlayer(state);
    input.moveZ = 1;
    fixedSteps(physics, world, state, input, 45);
    require(state.position.z < .72f, "wall blocks movement");
    auto revision = physics.collisionRevision();
    auto wall = world.objects.rbegin()->first;
    world.objects.erase(wall);
    physics.sync(world);
    require(revision != physics.collisionRevision(), "removed wall changes collision revision");
    fixedSteps(physics, world, state, input, 15);
    require(state.position.z > 1.5f, "removed geometry no longer blocks");
    world.objects.clear();
    physics.sync(world);
    input.moveZ = 0;
    fixedSteps(physics, world, state, input, 30);
    require(!state.grounded && state.position.y < -.5f, "removed ground falls");
}
void stepsCeilingsAndSupports() {
    Engine engine;
    auto &world = engine.world;
    Physics physics;
    box(world, {-4, -.2f, -4}, {80, 2, 80});
    box(world, {-1, 0, 1}, {20, 2, 15});
    physics.sync(world);
    PlayerState state;
    state.id = 1;
    state.position = {0, .03f, 0};
    PlayerInput input;
    input.moveZ = 1;
    fixedSteps(physics, world, state, input, 26);
    require(state.position.z > 1.4f && state.position.y > .15f, "walks up voxel step");
    world.objects.erase(world.objects.rbegin()->first);
    box(world, {-1, 1.95f, -1}, {20, 2, 20});
    state.position = {0, .03f, 0};
    state.velocity = {};
    physics.restorePlayer(state);
    input.moveZ = 0;
    fixedSteps(physics, world, state, input, 20);
    input.jump = true;
    fixedSteps(physics, world, state, input, 1);
    input.jump = false;
    fixedSteps(physics, world, state, input, 20);
    require(state.position.y < .18f, "ceiling limits jump");
    world.objects.clear();
    box(world, {-4, -.2f, -4}, {80, 2, 80});
    auto support = box(world, {-2, 0, -2}, {40, 2, 40}, true);
    world.objects.at(support).velocity = {1, 0, 0};
    physics.sync(world);
    state.position = {0, .23f, 0};
    state.velocity = {};
    physics.restorePlayer(state);
    fixedSteps(physics, world, state, input, 10);
    require(state.support == support, "dynamic debris supports character");
    // Prediction samples replicated dynamic bodies without simulating gravity.
    world.objects.at(support).velocity = {1, 0, 0};
    Physics prediction;
    prediction.setPrediction(true);
    prediction.sync(world);
    state.position = {0, .23f, 0};
    state.velocity = {};
    prediction.restorePlayer(state);
    for (int i = 0; i < 30; ++i)
        prediction.movePlayer(state, input, 1.f / 60.f);
    require(state.position.x > .3f, "inherits moving-support velocity");
}
void slopesAndDebris() {
    Engine engine;
    auto &world = engine.world;
    Physics physics;
    box(world, {-3, -.2f, -3}, {60, 2, 30});
    auto ramp = box(world, {-1, 0, 0}, {20, 2, 50});
    world.objects.at(ramp).transform.rotation = {-std::sin(.2617994f), 0, 0, std::cos(.2617994f)};
    PlayerState state;
    state.id = 1;
    state.position = {0, .03f, -1};
    PlayerInput input;
    input.moveZ = 1;
    fixedSteps(physics, world, state, input, 60);
    require(state.position.z > 1 && state.position.y > .6f, "walks thirty-degree slope");
    world.objects.at(ramp).transform.rotation = {-std::sin(.5235988f), 0, 0, std::cos(.5235988f)};
    physics.sync(world);
    state.position = {0, .03f, -1};
    state.velocity = {};
    physics.restorePlayer(state);
    fixedSteps(physics, world, state, input, 60);
    require(state.position.z < 1.1f, "sixty-degree slope is too steep");
    world.objects.clear();
    box(world, {-3, -.2f, -3}, {60, 2, 60});
    world.materials[1].density = 30; // A light 3.75 kg fixture, below the 100 N push limit.
    auto debris = box(world, {-.25f, 0, 1}, {5, 5, 5}, true);
    state.position = {0, .03f, 0};
    state.velocity = {};
    physics.sync(world);
    physics.restorePlayer(state);
    fixedSteps(physics, world, state, input, 45);
    require(world.objects.at(debris).transform.position.z > 1.05f,
            "bounded character force pushes light debris");
    require(physics.impacts.empty(), "character proxy contacts do not become voxel impacts");
}
struct ProjectFixture {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("aspera-player-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ProjectFixture() {
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "project.lua")
            << "return {name='Character "
               "test',player={mode='first_person',spawns={{x=0,y=.03,z=0}},bounds={min={x=-10,y=-5,"
               "z=-10},max={x=10,y=10,z=10}},build_material=5}}";
        std::ofstream(directory / "scene.lua")
            << "engine.spawn_box('floor',-4,-.2,-4,80,2,80,1,true)";
        std::ofstream(directory / "server.lua") << "function update(dt) end";
    }
    ~ProjectFixture() {
        std::filesystem::remove_all(directory);
    }
};
void lifecycleInputsAndPrediction() {
    ProjectFixture project;
    Engine engine;
    engine.loadProject(project.directory);
    require(engine.playerSettings().enabled && engine.playerSettings().spawns.size() == 1,
            "manifest exposes player settings");
    auto before = encodeWorld(engine.world);
    engine.addPlayer(1);
    engine.addPlayer(2);
    require(before == encodeWorld(engine.world), "players excluded from saved world");
    engine.step();
    require(!engine.players().at(1).spawnBlocked, "spawn succeeds");
    PlayerInput input;
    input.sequence = 1;
    input.generation = engine.players().at(1).generation;
    input.moveZ = 1;
    require(engine.submitPlayerInput(1, input), "owned input accepted");
    require(!engine.submitPlayerInput(1, input), "duplicate input ignored");
    require(!engine.submitPlayerInput(9, input), "unknown player rejected");
    auto bad = input;
    bad.sequence = 2;
    bad.moveX = std::numeric_limits<float>::infinity();
    require(!engine.submitPlayerInput(1, bad), "nonfinite input rejected");
    engine.step();
    auto moved = engine.players().at(1).position;
    for (int i = 0; i < 20; ++i)
        engine.step();
    require(engine.players().at(1).position.z - moved.z < .45f, "missing movement becomes neutral");
    input.sequence = 2;
    input.recover = true;
    engine.submitPlayerInput(1, input);
    engine.step();
    require(engine.players().at(1).generation != input.generation,
            "recovery invalidates input generation");
    require(!engine.submitPlayerInput(1, input), "old generation cannot move recovered player");
    engine.resetPhysics();
    engine.step();
    require(!engine.players().at(1).spawnBlocked, "world reset restores player bodies");
    Engine replica;
    replica.loadProject(project.directory);
    replica.setPlayerStates(engine.players());
    input = {};
    input.generation = engine.players().at(1).generation;
    for (int i = 1; i <= 150; ++i) {
        input.sequence = uint64_t(i);
        replica.predictPlayer(1, input);
    }
    require(replica.predictionHistorySize() <= 120, "prediction history bounded");
    auto predicted = replica.predictedPlayer();
    require(predicted.has_value(), "prediction available");
    require(length(predicted->position - engine.players().at(1).position) < .1f,
            "stationary prediction converges");
    engine.removePlayer(2);
    require(engine.players().size() == 1, "disconnect removes player");
    Command prefab;
    prefab.type = CommandType::SpawnPrefab;
    prefab.entity = engine.world.objects.begin()->first;
    prefab.cell = {40, 2, 40};
    prefab.vector = {0, 1, 0};
    prefab.radius = .4f;
    prefab.material = 5;
    auto command = engine.submit(prefab);
    bool rejected = false;
    for (int n = 0; n < 500 && !rejected; ++n) {
        engine.step();
        for (const auto &event : engine.events)
            if (event.command == command && event.type == "rejected")
                rejected = true;
        std::this_thread::yield();
    }
    require(rejected, "asynchronous construction rechecks player clearance at publication");
    auto outside = engine.players();
    outside.at(1).position = {0, -6, 0};
    engine.setPlayerStates(outside);
    auto oldGeneration = engine.players().at(1).generation;
    engine.step();
    require(engine.players().at(1).generation > oldGeneration &&
                !engine.players().at(1).spawnBlocked,
            "bounds recovery returns to a clear spawn");
    // Block every recovery candidate; retry only after geometry has been removed.
    auto blocker = box(engine.world, {-2, -.1f, -2}, {40, 40, 40});
    engine.resetPlayers();
    engine.step();
    require(engine.players().at(1).spawnBlocked, "blocked spawn remains pending");
    engine.world.objects.erase(blocker);
    engine.resetPhysics();
    engine.step();
    require(!engine.players().at(1).spawnBlocked, "spawn retries after clearance returns");
    engine.loadProject(project.directory);
    require(engine.players().empty(), "project load clears transient players");
}
void authoritativeTools() {
    ProjectFixture project;
    Engine engine;
    engine.loadProject(project.directory);
    auto target = box(engine.world, {-1, 0, 3}, {20, 30, 2});
    engine.addPlayer(1);
    engine.step();
    PlayerInput input;
    input.sequence = 1;
    input.generation = engine.players().at(1).generation;
    input.damage = true;
    require(engine.submitPlayerInput(1, input), "tool intent accepted");
    for (int n = 0; n < 100; ++n) {
        engine.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!engine.world.objects.at(target).damage.empty(),
            "tool ray damages an in-range surface");
    size_t damageEvents = 0;
    for (const auto &event : engine.events)
        if (event.type == "damage")
            ++damageEvents;
    require(damageEvents == 1, "one-shot damage is applied exactly once");
    auto count = engine.world.objects.at(target).grid.count();
    input.sequence = 2;
    input.damage = false;
    input.build = true;
    engine.submitPlayerInput(1, input);
    for (int n = 0; n < 100; ++n) {
        engine.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(engine.world.objects.at(target).grid.count() > count,
            "in-range construction adds supported voxels");
    engine.world.objects.erase(target);
    box(engine.world, {-1, 0, 6}, {20, 30, 2});
    engine.resetPhysics();
    engine.step();
    input = {};
    input.sequence = 1;
    input.generation = engine.players().at(1).generation;
    input.damage = true;
    engine.events.clear();
    engine.submitPlayerInput(1, input);
    engine.step();
    bool rejected = false;
    for (const auto &event : engine.events)
        if (event.type == "tool_rejected")
            rejected = true;
    require(rejected, "authority rejects surfaces beyond four-metre reach");
}
void quarryScenario() {
    Engine engine;
    engine.loadProject("games/quarry-demolition");
    require(engine.playerSettings().spawns.size() == 4, "quarry authors four spawns");
    for (PlayerId id = 1; id <= 4; ++id) {
        engine.addPlayer(id);
        require(engine.players().at(id).yaw == engine.playerSettings().spawns.at(id - 1).yaw,
                "pending quarry spawn preserves the authored camera direction");
    }
    for (int n = 0; n < 120; ++n)
        engine.step();
    for (const auto &[id, state] : engine.players()) {
        (void)id;
        require(!state.spawnBlocked && state.grounded, "all quarry spawns stand on clear ground");
    }
    require(engine.world.objects.size() == 4,
            "quarry structure stays connected until player action");
    require(engine.world.properties.at("game.state") == 0,
            "quarry starts with an active objective");
    auto initial = engine.world.properties.at("demolition.works_initial");
    auto works = EntityId(engine.world.properties.at("quarry.works_id"));
    // Stage all support removals while stopped, then exercise the resulting live collapse.
    // This avoids nondeterministic revision rejection while other supports are fracturing.
    engine.simulating = false;
    Command cut;
    cut.type = CommandType::SphereDamage;
    cut.entity = works;
    cut.radius = .8f;
    cut.strength = 100000;
    for (Vec3 point : {Vec3{-2.85f, 1.5f, -1.65f}, Vec3{2.75f, 1.5f, -1.65f},
                       Vec3{-2.85f, 1.5f, 1.55f}, Vec3{2.75f, 1.5f, 1.55f}}) {
        auto hit = raycastWorld(engine.world, point + Vec3{0, 0, -2}, {0, 0, 1}, 3);
        if (!hit)
            continue; // A previous cut may already have dropped this support.
        cut.entity = hit->entity;
        cut.point = point;
        auto command = engine.submit(cut);
        bool committed = false;
        for (int n = 0; n < 500 && !committed; ++n) {
            engine.step();
            for (const auto &event : engine.events)
                if (event.command == command && event.type == "committed")
                    committed = true;
                else if (event.command == command && event.type == "rejected")
                    throw std::runtime_error("quarry cut rejected: " + event.message);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!committed)
            for (const auto &event : engine.events)
                if (event.command == command)
                    std::cerr << event.type << ": " << event.message << '\n';
        require(committed, "quarry support cut commits");
    }
    engine.simulating = true;
    for (int n = 0; n < 120; ++n)
        engine.step();
    bool rubble = false;
    for (const auto &[id, object] : engine.world.objects) {
        (void)id;
        if (object.dynamic && object.grid.count())
            rubble = true;
    }
    require(rubble, "removing supports produces physical rubble");
    require(engine.world.properties.at("demolition.core_integrity") >= .8,
            "core survives intended support removal");
    VoxelGrid build;
    build.set({0, 0, 0}, 5);
    engine.world.spawn("construction", std::move(build), {{-7, 0, -7}, {}}, true);
    engine.step();
    require(engine.world.properties.at("demolition.works_initial") == initial,
            "construction does not change works denominator");
    for (auto &[id, object] : engine.world.objects) {
        (void)id;
        for (auto [cell, material] : object.grid.occupied())
            if (material == 2 || material == 3)
                object.grid.set(cell, 0);
        ++object.revision;
    }
    engine.resetPhysics();
    engine.step();
    require(engine.world.properties.at("game.state") == 1,
            "shared objective completes after sufficient works removal");
}

} // namespace
int main() {
    try {
        locomotionAndEdits();
        stepsCeilingsAndSupports();
        slopesAndDebris();
        lifecycleInputsAndPrediction();
        authoritativeTools();
        quarryScenario();
        std::cout << "Player controller tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
