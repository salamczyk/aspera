#include "aspera/engine.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace aspera;
namespace {
constexpr float Step = 1.f / 60.f;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void requireThrows(F &&action, const char *message) {
    bool threw = false;
    try {
        action();
    } catch (const std::exception &) {
        threw = true;
    }
    require(threw, message);
}
Material testMaterial() {
    Material m;
    m.name = "test";
    m.rgba = 0x55aaeeff;
    m.density = 1000.f;
    m.resistance = 1.f;
    m.tension = 1.0e9f;
    m.compression = 1.0e9f;
    m.shear = 1.0e9f;
    m.bending = 1.0e9f;
    return m;
}
VoxelGrid cells(std::initializer_list<IVec3> positions) {
    VoxelGrid g;
    for (auto p : positions)
        g.set(p, 1);
    g.clearDirty();
    return g;
}
World testWorld() {
    World w;
    w.voxelSize = .1f;
    w.materials = {Material{}, testMaterial(), testMaterial()};
    w.materials[2].name = "paint";
    return w;
}
void configure(Engine &e) {
    e.world = testWorld();
}
bool eventFor(const Engine &e, uint64_t command) {
    for (const auto &event : e.events)
        if (event.command == command)
            return true;
    return false;
}
void settle(Engine &e, uint64_t command) {
    for (int i = 0; i < 2000 && !eventFor(e, command); ++i) {
        e.step(Step);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(eventFor(e, command), "queued command completes within bounded fixed ticks");
}

void persistenceIsExactAndDefensive() {
    World source = testWorld();
    source.materials[1].impactToughness = 432100.f;
    source.materials[1].roughness = .35f;
    source.materials[1].metallic = .65f;
    source.materials[1].emissive = {.2f, .1f, .05f};
    source.materials[1].emissionIntensity = 900.f;
    source.environment.exposure = 12.f;
    source.lights[1] = {
        1, LightType::Spot, {1, 2, 3}, {0, -1, 0}, {1, .8f, .6f}, 1200, 15, 15, 30, true, 2};
    source.nextLight = 2;
    source.properties["wind"] = 7.5;
    source.properties["difficulty"] = 3;
    Transform t;
    t.position = {1.5f, -2.f, .25f};
    t.rotation = {0, 0, std::sqrt(.5f), std::sqrt(.5f)};
    EntityId id = source.spawn("persisted", cells({{-4, 0, 2}, {-3, 0, 2}}), t, true);
    auto &object = source.objects.at(id);
    object.anchors.insert({-4, 0, 2});
    object.seams.insert(Seam{{-4, 0, 2}, {-3, 0, 2}});
    object.damage[{-3, 0, 2}] = .25f;
    auto bytes = encodeWorld(source);
    World decoded = decodeWorld(bytes);
    require(decoded.checksum() == source.checksum(), "persistence roundtrip checksum");
    require(decoded.properties == source.properties && decoded.objects.at(id).seams == object.seams,
            "persistence retains properties and seams");
    require(std::abs(decoded.materials[1].impactToughness - 432100.f) < 1e-3f,
            "persistence retains impact toughness");
    require(std::abs(decoded.materials[1].metallic - .65f) < 1e-5f && decoded.lights.size() == 1 &&
                decoded.nextLight == 2 && std::abs(decoded.environment.exposure - 12.f) < 1e-5f,
            "persistence retains PBR and lighting");
    auto file = std::filesystem::temp_directory_path() / "aspera-engine-world.bin";
    saveWorld(source, file);
    World loaded = loadWorld(file);
    std::filesystem::remove(file);
    require(loaded.checksum() == source.checksum(), "save/load checksum");
    require(std::abs(loaded.materials[1].impactToughness - 432100.f) < 1e-3f,
            "save/load retains impact toughness");
    auto truncated = bytes;
    truncated.pop_back();
    requireThrows([&] { (void)decodeWorld(truncated); }, "truncated snapshot rejected");
    auto trailing = bytes;
    trailing.push_back(0);
    requireThrows([&] { (void)decodeWorld(trailing); }, "trailing snapshot rejected");
    auto hostile = bytes;
    hostile[0] ^= 0xff;
    requireThrows([&] { (void)decodeWorld(hostile); }, "foreign snapshot rejected");
    auto invalidLight = bytes;
    invalidLight[invalidLight.size() - 5] = 2; // shadows flag immediately precedes int32 priority.
    requireThrows([&] { (void)decodeWorld(invalidLight); }, "invalid lighting packet rejected");
    invalidLight = bytes;
    invalidLight[invalidLight.size() - 4] = 0xff;
    invalidLight[invalidLight.size() - 3] = 0xff;
    invalidLight[invalidLight.size() - 2] = 0xff;
    invalidLight[invalidLight.size() - 1] = 0x7f;
    requireThrows([&] { (void)decodeWorld(invalidLight); }, "extreme shadow priority rejected");
    source.materials[1].roughness = 2;
    requireThrows([&] { (void)encodeWorld(source); }, "invalid PBR settings cannot be encoded");
}

void rotatedNegativeRaycast() {
    World w = testWorld();
    Transform t;
    t.position = {1.f, 1.f, 1.f};
    t.rotation = {0, 0, std::sqrt(.5f), std::sqrt(.5f)};
    EntityId id = w.spawn("rotated", cells({{-1, 0, 0}}), t, false);
    auto hit = raycastWorld(w, {.95f, .8f, 1.05f}, {0, 1, 0}, 1.f);
    require(hit && hit->entity == id && hit->hit.cell == IVec3{-1, 0, 0},
            "negative rotated raycast");
    require(std::abs(hit->worldPoint.y - .9f) < 1e-4f, "ray world point");
}

void commandsAndStructure() {
    Engine e;
    configure(e);
    EntityId id = e.world.spawn("beam", cells({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}), {}, false);
    e.world.objects.at(id).anchors.insert({0, 0, 0});
    e.step(Step); // Build the initial structural graph while supported.
    auto before = e.world.objects.at(id).revision;
    Command add;
    add.type = CommandType::SetVoxel;
    add.entity = id;
    add.cell = {3, 0, 0};
    add.material = 1;
    add.expectedRevision = before;
    uint64_t addId = e.submit(add);
    settle(e, addId);
    require(e.world.objects.at(id).grid.get({3, 0, 0}) == 1, "set voxel command adds material");
    before = e.world.objects.at(id).revision;
    Command paint;
    paint.type = CommandType::Paint;
    paint.entity = id;
    paint.cell = {2, 0, 0};
    paint.material = 2;
    paint.expectedRevision = before;
    uint64_t paintId = e.submit(paint);
    require(paintId != 0, "commands receive nonzero ids");
    settle(e, paintId);
    require(e.world.objects.at(id).revision > before &&
                e.world.objects.at(id).grid.get({2, 0, 0}) == 2,
            "accepted paint revises object");
    Command stale;
    stale.type = CommandType::SetVoxel;
    stale.entity = id;
    stale.cell = {9, 0, 0};
    stale.material = 1;
    stale.expectedRevision = before;
    uint64_t staleId = e.submit(stale);
    settle(e, staleId);
    require(!e.world.objects.at(id).grid.get({9, 0, 0}),
            "stale expected revision rejects mutation");
    require(eventFor(e, staleId), "stale command produces a queued result event");
    auto current = e.world.objects.at(id).revision;
    Command cut;
    cut.type = CommandType::SetVoxel;
    cut.entity = id;
    cut.cell = {1, 0, 0};
    cut.material = 0;
    cut.expectedRevision = current;
    uint64_t cutId = e.submit(cut);
    settle(e, cutId);
    e.step(Step);
    e.step(Step);
    require(eventFor(e, cutId), "successful voxel edit emits an event");
    size_t count = 0;
    bool falling = false;
    for (const auto &[_, o] : e.world.objects) {
        count += o.grid.count();
        falling |= o.dynamic;
    }
    require(count == 3, "cut does not duplicate voxel mass");
    require(e.world.objects.size() == 2 && falling, "removed support creates a falling fragment");

    Engine freeBody;
    configure(freeBody);
    EntityId unsupported = freeBody.world.spawn("free", cells({{0, 0, 0}}), {}, false);
    freeBody.step(Step);
    require(freeBody.world.objects.at(unsupported).dynamic, "unsupported object becomes dynamic");
}

void damageQueueSemantics() {
    Engine e;
    configure(e);
    EntityId id = e.world.spawn("damage", cells({{0, 0, 0}}), {}, false);
    e.world.objects.at(id).anchors.insert({0, 0, 0});
    auto rev = e.world.objects.at(id).revision;
    Command damage;
    damage.type = CommandType::SphereDamage;
    damage.entity = id;
    damage.point = {.05f, .05f, .05f};
    damage.radius = .2f;
    damage.strength = 1;
    damage.expectedRevision = rev;
    uint64_t command = e.submit(damage);
    settle(e, command);
    require(!e.world.objects.contains(id), "sufficient radial damage removes the occupied object");
}

void stressAndBracing() {
    auto setup = [](Engine &e, bool brace) {
        configure(e);
        e.world.materials[1].bending = 500000;
        VoxelGrid grid;
        for (int x = 0; x < 24; ++x)
            grid.set({x, 0, 0}, 1);
        auto id = e.world.spawn("cantilever", std::move(grid));
        e.world.objects.at(id).anchors.insert({0, 0, 0});
        if (brace)
            e.world.objects.at(id).anchors.insert({23, 0, 0});
        return id;
    };
    Engine unsupported, braced;
    auto a = setup(unsupported, false), b = setup(braced, true);
    unsupported.step();
    braced.step();
    require(unsupported.world.objects.at(a).stress > braced.world.objects.at(b).stress,
            "brace reduces bending stress");
    const auto &regions = unsupported.world.objects.at(a).stressRegions;
    require(regions.size() == 3, "cantilever exposes each structural region");
    float minimum = regions.front().utilization, maximum = minimum;
    for (auto &r : regions) {
        minimum = std::min(minimum, r.utilization);
        maximum = std::max(maximum, r.utilization);
        require(r.maximum.x > r.minimum.x, "region outlines have valid bounds");
    }
    require(minimum < maximum && maximum == unsupported.world.objects.at(a).stress,
            "local stress differs along beam and preserves peak reading");
    require(stressColor(0) != stressColor(.5f) && stressColor(.5f) != stressColor(1),
            "zero medium and overloaded stress have distinct colors");
    require(stressColor(2) == stressColor(1), "overload saturates only above capacity");
    for (int i = 0; i < 90; ++i) {
        unsupported.step();
        braced.step();
    }
    require(unsupported.world.objects.size() > 1, "sustained overloaded cantilever fractures");
    require(braced.world.objects.size() == 1 && !braced.world.objects.at(b).dynamic,
            "braced beam remains supported");
    Engine falling;
    configure(falling);
    VoxelGrid grid;
    for (int x = 0; x < 24; ++x)
        grid.set({x, 0, 0}, 1);
    falling.world.spawn("free beam", std::move(grid), {{0, 5, 0}, {}}, true);
    for (int i = 0; i < 30; ++i)
        falling.step();
    require(falling.world.objects.size() == 1 && falling.world.objects.begin()->second.stress == 0,
            "freefall gravity creates no internal stress");
    for (auto &r : falling.world.objects.begin()->second.stressRegions)
        require(r.utilization == 0, "freefall resets regional stress");
}

void pausedAuthoringAndConstruction() {
    Engine e;
    configure(e);
    e.simulating = false;
    auto id = e.world.spawn("foundation", cells({{0, 0, 0}}));
    e.world.objects.at(id).anchors.insert({0, 0, 0});
    Command c;
    c.type = CommandType::SpawnPrefab;
    c.entity = id;
    c.cell = {0, 1, 0};
    c.vector = {0, 1, 0};
    c.radius = .3f;
    auto command = e.submit(c);
    settle(e, command);
    require(e.world.objects.size() == 1 && e.world.objects.at(id).grid.count() == 28,
            "cube construction joins supported grid");
    require(e.world.objects.at(id).transform.position.y == 0,
            "paused edits do not advance physics");
    auto snapshot = encodeWorld(e.world);
    e.world = decodeWorld(snapshot);
    e.resetPhysics();
    e.simulating = true;
    e.step();
    require(!e.world.objects.at(id).dynamic, "built structure remains attached after restore");
}

const Event *findEvent(const Engine &e, const char *type) {
    for (auto it = e.events.rbegin(); it != e.events.rend(); ++it)
        if (it->type == type)
            return &*it;
    return nullptr;
}

void releaseBodyValidation() {
    Engine paused;
    configure(paused);
    auto pausedId = paused.world.spawn("paused release", cells({{0, 0, 0}}));
    paused.world.objects.at(pausedId).anchors.insert({0, 0, 0});
    paused.simulating = false;
    Command release;
    release.type = CommandType::ReleaseBody;
    release.entity = pausedId;
    release.vector = {0, -10, 0};
    release.expectedRevision = paused.world.objects.at(pausedId).revision;
    auto pausedCommand = paused.submit(release);
    paused.step();
    require(eventFor(paused, pausedCommand), "paused release produces a result event");
    require(findEvent(paused, "rejected") && !paused.world.objects.at(pausedId).dynamic &&
                paused.world.objects.at(pausedId).anchors.contains({0, 0, 0}),
            "paused release is rejected without changing the body");

    Engine jointed;
    configure(jointed);
    auto a = jointed.world.spawn("jointed release", cells({{0, 0, 0}}));
    auto b = jointed.world.spawn("joint partner", cells({{0, 0, 0}}), {{2, 0, 0}, {}});
    jointed.world.objects.at(a).anchors.insert({0, 0, 0});
    jointed.world.objects.at(b).anchors.insert({0, 0, 0});
    jointed.world.joints.push_back({1, a, b, {1, 0, 0}, false, 10000});
    jointed.step();
    release.entity = a;
    release.expectedRevision = jointed.world.objects.at(a).revision;
    auto jointCommand = jointed.submit(release);
    jointed.step();
    require(eventFor(jointed, jointCommand), "jointed release produces a result event");
    require(findEvent(jointed, "rejected") && !jointed.world.objects.at(a).dynamic &&
                !jointed.world.objects.at(a).anchors.empty(),
            "release with an attached joint is rejected");
}

void materialCollisionDamage() {
    Engine e;
    configure(e);
    // Structural strengths are deliberately high so this test isolates impact erosion.
    e.world.materials[1].impactToughness = 1000.f;
    auto target = e.world.spawn("static impact target", cells({{0, 0, 0}}), {}, false);
    auto impactor =
        e.world.spawn("falling impactor", cells({{0, 0, 0}}), {{0, 1.5f, 0}, {}}, false);
    e.world.objects.at(target).anchors.insert({0, 0, 0});
    e.world.objects.at(impactor).anchors.insert({0, 0, 0});
    e.step();

    Command launch;
    launch.type = CommandType::ReleaseBody;
    launch.entity = impactor;
    launch.vector = {0, -50, 0};
    launch.expectedRevision = e.world.objects.at(impactor).revision;
    auto command = e.submit(launch);
    require(command != 0, "impact launch receives a command id");

    bool sawPending = false, sawDamage = false, sawCollision = false;
    for (int i = 0; i < 180; ++i) {
        e.step();
        sawPending |= e.stats.pendingImpacts > 0;
        require(e.stats.pendingImpacts <= 256 && e.stats.droppedImpacts == 0,
                "impact queue stays within its bounded budget");
        sawDamage |= !e.world.objects.contains(target) ||
                     e.world.objects.at(target).grid.count() < 1 ||
                     !e.world.objects.at(target).damage.empty();
        for (const auto &event : e.events) {
            if (event.type != "collision" || !event.collision)
                continue;
            sawCollision = true;
            require(event.collision->other == target || event.collision->other == impactor,
                    "collision event identifies the other body");
            require(event.collision->closingSpeed > 1.f && event.collision->energy > 0,
                    "collision event reports a genuine impact");
            require(std::abs(length(event.collision->normal) - 1.f) < .01f,
                    "collision event reports a unit normal");
        }
        if (sawDamage && e.stats.processedImpacts > 0 && sawCollision)
            break;
    }
    require(sawPending, "hard impact enters the bounded pending-impact queue");
    require(sawCollision, "hard drop produces a structured collision event");
    require(sawDamage, "hard drop erodes the static target");
    require(e.stats.processedImpacts > 0, "hard impact is processed after the physics step");

    // Two released bodies exercise the moving-moving path and ensure both local patches are used.
    Engine moving;
    configure(moving);
    moving.world.materials[1].impactToughness = 1000.f;
    auto movingTarget =
        moving.world.spawn("moving target", cells({{0, 0, 0}}), {{0, 1, 0}, {}}, false);
    auto movingImpactor =
        moving.world.spawn("moving impactor", cells({{0, 0, 0}}), {{0, 2, 0}, {}}, false);
    moving.world.objects.at(movingTarget).anchors.insert({0, 0, 0});
    moving.world.objects.at(movingImpactor).anchors.insert({0, 0, 0});
    moving.step();
    Command targetRelease;
    targetRelease.type = CommandType::ReleaseBody;
    targetRelease.entity = movingTarget;
    targetRelease.expectedRevision = moving.world.objects.at(movingTarget).revision;
    Command impactorRelease;
    impactorRelease.type = CommandType::ReleaseBody;
    impactorRelease.entity = movingImpactor;
    impactorRelease.vector = {0, -40, 0};
    impactorRelease.expectedRevision = moving.world.objects.at(movingImpactor).revision;
    moving.submit(targetRelease);
    moving.submit(impactorRelease);
    bool movingDamaged = false, movingCollision = false;
    for (int i = 0; i < 180; ++i) {
        moving.step();
        movingDamaged |= !moving.world.objects.contains(movingTarget) ||
                         moving.world.objects.at(movingTarget).grid.count() < 1 ||
                         !moving.world.objects.at(movingTarget).damage.empty();
        for (const auto &event : moving.events)
            movingCollision |=
                event.type == "collision" && event.collision &&
                ((event.entity == movingTarget && event.collision->other == movingImpactor) ||
                 (event.entity == movingImpactor && event.collision->other == movingTarget));
        if (movingDamaged && movingCollision)
            break;
    }
    require(movingDamaged && movingCollision, "moving body impact damages the other moving body");
}

void restingContactIsStable() {
    Engine e;
    configure(e);
    e.world.materials[1].impactToughness = 1000.f;
    auto floor = e.world.spawn("resting floor", cells({{0, 0, 0}}));
    auto block = e.world.spawn("resting block", cells({{0, 0, 0}}), {{0, .1f, 0}, {}}, false);
    e.world.objects.at(floor).anchors.insert({0, 0, 0});
    e.world.objects.at(block).anchors.insert({0, 0, 0});
    e.step();
    Command release;
    release.type = CommandType::ReleaseBody;
    release.entity = block;
    release.expectedRevision = e.world.objects.at(block).revision;
    auto command = e.submit(release);
    require(command != 0, "resting body release receives a command id");
    for (int i = 0; i < 180; ++i) {
        e.step();
        require(e.stats.pendingImpacts <= 256 && e.stats.droppedImpacts == 0,
                "resting contact keeps impact queue bounded");
    }
    require(e.world.objects.contains(floor) && e.world.objects.at(floor).grid.count() == 1 &&
                e.world.objects.at(floor).damage.empty(),
            "resting contact does not progressively erode the floor");
}
void impactEnergyAndBudgets() {
    auto damageAt = [](float speed, float toughness, float density = 1000.f) {
        Engine e;
        configure(e);
        e.world.materials[1].impactToughness = toughness;
        e.world.materials[1].density = density;
        auto target = e.world.spawn("target", cells({{0, 0, 0}}));
        auto striker = e.world.spawn("striker", cells({{0, 0, 0}}), {{0, 1.5f, 0}, {}}, true);
        e.world.objects.at(target).anchors.insert({0, 0, 0});
        e.world.objects.at(striker).velocity = {0, -speed, 0};
        for (int i = 0; i < 120; ++i) {
            e.step();
            if (e.stats.processedImpacts)
                break;
        }
        require(e.world.objects.contains(target), "tough probe survives impact");
        auto &d = e.world.objects.at(target).damage;
        require(!d.empty(), "impact probe accumulates partial damage");
        return d.begin()->second;
    };
    auto slow = damageAt(5, 1e8f), fast = damageAt(15, 1e8f), tough = damageAt(15, 5e8f);
    require(damageAt(15, 1e8f, 2000) > fast, "greater impactor mass raises damage");
    require(fast > slow && tough < fast, "speed raises damage and toughness reduces damage");
    Engine burst;
    configure(burst);
    burst.world.materials[1].impactToughness = 1e9f;
    for (int i = 0; i < 300; ++i) {
        Vec3 p{float(i % 20) * .4f, 0, float(i / 20) * .4f};
        auto target = burst.world.spawn("burst target", cells({{0, 0, 0}}), {p, {}});
        burst.world.objects.at(target).anchors.insert({0, 0, 0});
        auto striker = burst.world.spawn("burst striker", cells({{0, 0, 0}}),
                                         {p + Vec3{0, .13f, 0}, {}}, true);
        burst.world.objects.at(striker).velocity = {0, -5, 0};
    }
    bool overflow = false;
    for (int i = 0; i < 8; ++i) {
        burst.step();
        require(burst.stats.pendingImpacts <= 256 && burst.stats.processedImpacts <= 64,
                "collision bursts obey pending and processing budgets");
        overflow |= burst.stats.droppedImpacts > 0;
    }
    require(overflow, "collision burst exposes overflow counter");
}
void rotatedImpactAndConcurrentEdit() {
    Engine e;
    configure(e);
    e.world.materials[1].impactToughness = 1e8f;
    e.world.materials[2].impactToughness = 1e8f;
    Quat rotation{0, 0, std::sqrt(.5f), std::sqrt(.5f)};
    auto target = e.world.spawn("rotated target", cells({{0, 0, 0}}), {{0, 0, 0}, rotation});
    e.world.objects.at(target).anchors.insert({0, 0, 0});
    auto striker = e.world.spawn("striker", cells({{0, 0, 0}}), {{-.1f, 1.5f, 0}, {}}, true);
    e.world.objects.at(striker).velocity = {0, -15, 0};
    for (int i = 0; i < 120 && !e.stats.pendingImpacts; ++i)
        e.step();
    require(e.stats.pendingImpacts > 0, "rotated target receives contact");
    Command paint;
    paint.type = CommandType::Paint;
    paint.entity = target;
    paint.cell = {0, 0, 0};
    paint.material = 2;
    auto command = e.submit(paint);
    settle(e, command);
    for (int i = 0; i < 120 && e.stats.pendingImpacts; ++i)
        e.step();
    require(e.world.objects.at(target).grid.get({0, 0, 0}) == 2,
            "impact publication preserves concurrent committed paint");
    require(!e.world.objects.at(target).damage.empty(),
            "contact coordinates damage the rotated object's occupied cell");
}
void legacyWorldMaterials() {
    auto w = testWorld();
    w.materials[1].name = "steel";
    auto modern = encodeWorld(w);
    auto bytes = modern;
    auto read32 = [&](size_t at) {
        return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
               uint32_t(bytes[at + 3]) << 24;
    };
    size_t at = 40;
    for (size_t i = 0; i < w.materials.size(); ++i) {
        at += 4 + read32(at) + 4 + 7 * 4;
        bytes.erase(bytes.begin() + at, bytes.begin() + at + 7 * 4);
    }
    // v3 appends 13 environment floats, nextLight, and a zero light count.
    bytes.erase(bytes.end() - 64, bytes.end());
    bytes[4] = 1;
    auto legacy = decodeWorld(bytes);
    require(legacy.materials[1].impactToughness == 5000000,
            "v1 worlds receive material-name impact defaults");

    bytes = modern;
    at = 40;
    for (size_t i = 0; i < w.materials.size(); ++i) {
        at += 4 + read32(at) + 4 + 8 * 4;
        bytes.erase(bytes.begin() + at, bytes.begin() + at + 6 * 4);
    }
    bytes.erase(bytes.end() - 64, bytes.end());
    bytes[4] = 2;
    auto v2 = decodeWorld(bytes);
    require(v2.materials[1].impactToughness == w.materials[1].impactToughness &&
                v2.materials[1].roughness == .85f && v2.materials[1].metallic == 0 &&
                v2.materials[1].emissionIntensity == 0,
            "v2 worlds receive PBR defaults");
}
void lightingValidation() {
    Engine e;
    auto revision = e.world.revision;
    auto environment = e.world.environment;
    environment.exposure = 10;
    e.setEnvironment(environment);
    require(e.world.revision == revision + 1 && e.world.environment.exposure == 10,
            "environment revisions world");
    auto material = e.world.materials[1];
    material.metallic = .5f;
    material.emissionIntensity = 200;
    e.setMaterial(1, material);
    require(e.world.materials[1].metallic == .5f, "PBR material mutation accepted");
    auto object = e.world.spawn("material probe", cells({{0, 0, 0}}));
    auto objectRevision = e.world.objects.at(object).revision;
    material.roughness = .4f;
    e.setMaterial(1, material);
    require(e.world.objects.at(object).revision == objectRevision,
            "appearance-only material edit does not invalidate physics geometry");
    material.density += 1;
    e.setMaterial(1, material);
    require(e.world.objects.at(object).revision > objectRevision,
            "physical material edit invalidates affected physics geometry");

    Command inFlight;
    inFlight.type = CommandType::SetVoxel;
    inFlight.entity = object;
    inFlight.cell = {1, 0, 0};
    inFlight.material = 1;
    auto inFlightId = e.submit(inFlight);
    Command queued = inFlight;
    queued.cell = {0, 1, 0};
    auto queuedId = e.submit(queued);
    e.step(Step); // Launch the first job and retain the second behind it.
    material.density += 1;
    e.setMaterial(1, material);
    settle(e, inFlightId);
    bool staleRejected = false;
    for (const auto &event : e.events)
        staleRejected |= event.command == inFlightId && event.type == "rejected";
    require(staleRejected, "material edit explicitly rejects an in-flight stale command");
    settle(e, queuedId);
    require(e.world.objects.at(object).grid.get({0, 1, 0}) == 1,
            "material edit retains queued commands for the updated material state");

    auto id = e.addLight({});
    require(id == 1 && e.world.lights.contains(id), "light receives allocator id");
    auto light = e.world.lights.at(id);
    light.range = 20;
    e.updateLight(id, light);
    require(e.world.lights.at(id).range == 20, "light update accepted");
    e.removeLight(id);
    require(e.world.lights.empty(), "light removal accepted");
    material.roughness = 2;
    requireThrows([&] { e.setMaterial(1, material); }, "invalid PBR rejected atomically");
    require(e.world.materials[1].roughness != 2, "invalid material leaves state untouched");
    Light bad;
    bad.range = 0;
    requireThrows([&] { e.addLight(bad); }, "invalid light rejected");
    bad = {};
    bad.shadowPriority = std::numeric_limits<int32_t>::max();
    requireThrows([&] { e.addLight(bad); }, "extreme shadow priority rejected");
    e.world.nextLight = std::numeric_limits<LightId>::max() - 1;
    requireThrows([&] { e.addLight({}); }, "exhausted light allocator rejected");
}
void luaLightingMutations() {
    auto project = std::filesystem::temp_directory_path() / "aspera-lua-lighting";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);
    {
        std::ofstream(project / "project.lua") << "return { name = 'Lua lighting' }\n";
        std::ofstream(project / "scene.lua")
            << "engine.environment({ exposure = 11, sky_color = { x = .2, y = .3, z = .4 } })\n"
               "engine.material(1, { roughness = .25, metallic = .5, emissive = { x = .1, y = .2, "
               "z = .3 }, emission_intensity = 50 })\n"
               "local id = engine.add_light({ type = 'spot', position = { x = 1, y = 2, z = 3 } "
               "})\n"
               "engine.update_light(id, { range = 14 })\nengine.property('light_id', id)\n";
        std::ofstream(project / "server.lua") << "\n";
    }
    Engine e;
    e.loadProject(project);
    require(e.world.environment.exposure == 11 && e.world.environment.skyColor.z == .4f &&
                e.world.materials[1].roughness == .25f && e.world.materials[1].metallic == .5f &&
                e.world.lights.size() == 1 && e.world.lights.begin()->second.range == 14,
            "Lua lighting APIs partially mutate world state");
    auto retained = e.world.materials[1];
    retained.roughness = .72f;
    e.setMaterial(1, retained);
    e.setEnvironment({});
    e.addLight({});
    std::ofstream(project / "scene.lua") << "\n";
    e.loadProject(project);
    require(e.world.materials[1].roughness == .72f && e.world.lights.empty() &&
                e.world.environment.exposure == EnvironmentSettings{}.exposure,
            "scene reload resets lighting while retaining the material palette");
    Engine saved;
    auto savedEnvironment = saved.world.environment;
    savedEnvironment.exposure = 7;
    saved.setEnvironment(savedEnvironment);
    auto savedLight = saved.addLight({});
    std::ofstream(project / "scene.lua") << "engine.environment({ exposure = 1 })\n";
    saveWorld(saved.world, project / "scene.aspera");
    e.loadProject(project);
    std::filesystem::remove_all(project);
    require(e.world.environment.exposure == 7 && e.world.lights.contains(savedLight),
            "saved scene lighting takes precedence over Lua scene initialization");
}
} // namespace

int main() {
    persistenceIsExactAndDefensive();
    rotatedNegativeRaycast();
    commandsAndStructure();
    damageQueueSemantics();
    stressAndBracing();
    pausedAuthoringAndConstruction();
    releaseBodyValidation();
    materialCollisionDamage();
    restingContactIsStable();
    impactEnergyAndBudgets();
    legacyWorldMaterials();
    rotatedImpactAndConcurrentEdit();
    lightingValidation();
    luaLightingMutations();
}
