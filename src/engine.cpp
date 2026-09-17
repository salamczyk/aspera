#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>

namespace aspera {
namespace {
struct CollisionPreparation {
    std::map<IVec3, uint64_t> keys;
    std::map<IVec3, std::vector<CollisionBox>> boxes;
    float size{};
};
CollisionPreparation prepareCollisions(const VoxelGrid &grid, float size,
                                       CollisionPreparation cache = {}) {
    if (cache.size != size)
        cache = {};
    cache.size = size;
    size_t count = 0;
    for (auto &[coordinate, chunk] : grid.chunks()) {
        const auto key = grid.chunkContentKey(coordinate);
        if (!cache.keys.contains(coordinate) || cache.keys.at(coordinate) != key) {
            cache.boxes[coordinate] = collisionBoxesChunk(grid, coordinate, size);
            cache.keys[coordinate] = key;
        }
        count += cache.boxes.at(coordinate).size();
        if (count > 32768)
            throw std::runtime_error("Collision complexity budget exceeded");
    }
    std::erase_if(cache.keys, [&](auto &entry) { return !grid.chunks().contains(entry.first); });
    std::erase_if(cache.boxes, [&](auto &entry) { return !grid.chunks().contains(entry.first); });
    return cache;
}
struct PreparedLandscape {
    LandscapeData data;
    CollisionPreparation collisions;
    StructureGraph structure;
};
struct Prepared {
    Command command;
    Object object;
    std::string error;
    CollisionPreparation collisions;
};
bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool validMaterial(const Material &m) {
    return !m.name.empty() && m.name.size() <= 4096 && std::isfinite(m.density) &&
           std::isfinite(m.resistance) && std::isfinite(m.friction) && std::isfinite(m.tension) &&
           std::isfinite(m.compression) && std::isfinite(m.shear) && std::isfinite(m.bending) &&
           std::isfinite(m.impactToughness) && std::isfinite(m.roughness) &&
           std::isfinite(m.metallic) && finite(m.emissive) && std::isfinite(m.emissionIntensity) &&
           m.density >= 0 && m.resistance > 0 && m.friction >= 0 && m.tension > 0 &&
           m.compression > 0 && m.shear > 0 && m.bending > 0 && m.impactToughness > 0 &&
           m.roughness >= 0 && m.roughness <= 1 && m.metallic >= 0 && m.metallic <= 1 &&
           m.emissive.x >= 0 && m.emissive.y >= 0 && m.emissive.z >= 0 && m.emissionIntensity >= 0;
}
bool physicsMaterialChanged(const Material &a, const Material &b) {
    return a.density != b.density || a.resistance != b.resistance || a.friction != b.friction ||
           a.tension != b.tension || a.compression != b.compression || a.shear != b.shear ||
           a.bending != b.bending || a.impactToughness != b.impactToughness;
}
bool validEnvironment(const EnvironmentSettings &v) {
    return finite(v.sunDirection) && length(v.sunDirection) > .0001f && finite(v.sunColor) &&
           finite(v.skyColor) && std::isfinite(v.sunIntensity) && std::isfinite(v.skyIntensity) &&
           std::isfinite(v.exposure) && std::isfinite(v.bloomStrength) && v.sunColor.x >= 0 &&
           v.sunColor.y >= 0 && v.sunColor.z >= 0 && v.skyColor.x >= 0 && v.skyColor.y >= 0 &&
           v.skyColor.z >= 0 && v.sunIntensity >= 0 && v.skyIntensity >= 0 && v.exposure >= -20 &&
           v.exposure <= 30 && v.bloomStrength >= 0 && v.bloomStrength <= 10;
}
bool validLight(const Light &v) {
    return (v.type == LightType::Point || v.type == LightType::Spot) && finite(v.position) &&
           finite(v.direction) && length(v.direction) > .0001f && finite(v.color) &&
           std::isfinite(v.intensity) && std::isfinite(v.range) && std::isfinite(v.innerAngle) &&
           std::isfinite(v.outerAngle) && v.color.x >= 0 && v.color.y >= 0 && v.color.z >= 0 &&
           v.intensity >= 0 && v.range > 0 && v.range <= 10000 && v.innerAngle >= 0 &&
           v.outerAngle >= v.innerAngle && v.outerAngle <= 89.9f &&
           std::abs(int64_t(v.shadowPriority)) <= 100000;
}
Prepared prepare(Command c, Object o, float size, const std::vector<Material> &materials,
                 CollisionPreparation collisions) {
    Prepared result{c, std::move(o), {}, std::move(collisions)};
    auto &obj = result.object;
    try {
        if (c.expectedRevision && c.expectedRevision != obj.revision)
            throw std::runtime_error("Stale object revision");
        if (c.type == CommandType::SetVoxel || c.type == CommandType::Paint) {
            if (c.type != CommandType::Paint || obj.grid.get(c.cell)) {
                obj.grid.set(c.cell, c.material);
                obj.damage.erase(c.cell);
                if (!c.material)
                    obj.anchors.erase(c.cell);
            }
        } else if (c.type == CommandType::SpawnPrefab) {
            if (obj.dynamic)
                throw std::runtime_error("Construction requires a static support");
            int n = std::clamp(int(c.radius / size), 1, 16);
            auto start = c.cell;
            if (c.vector.x < 0)
                start.x -= n - 1;
            if (c.vector.y < 0)
                start.y -= n - 1;
            if (c.vector.z < 0)
                start.z -= n - 1;
            bool touching = false;
            for (auto d : Neighbors)
                touching |= obj.grid.get(c.cell + d) != 0;
            if (!touching)
                throw std::runtime_error("Construction must touch its support");
            for (int x = 0; x < n; ++x)
                for (int y = 0; y < n; ++y)
                    for (int z = 0; z < n; ++z) {
                        auto p = start + IVec3{x, y, z};
                        if (!obj.grid.get(p))
                            obj.grid.set(p, std::max<MaterialId>(1, c.material));
                    }
        } else if (c.type == CommandType::Anchor) {
            if (!obj.grid.get(c.cell))
                throw std::runtime_error("Anchor requires occupied cell");
            if (c.strength > 0)
                obj.anchors.insert(c.cell);
            else
                obj.anchors.erase(c.cell);
            obj.dynamic = obj.anchors.empty();
        } else if (c.type == CommandType::SphereDamage) {
            auto q = obj.transform.rotation;
            Vec3 local = rotate({-q.x, -q.y, -q.z, q.w}, c.point - obj.transform.position);
            if (!finite(local))
                throw std::runtime_error("Invalid local damage position");
            const double width = double(size) * ChunkSize;
            const double lo[3]{std::floor((double(local.x) - c.radius) / width),
                               std::floor((double(local.y) - c.radius) / width),
                               std::floor((double(local.z) - c.radius) / width)};
            const double hi[3]{std::floor((double(local.x) + c.radius) / width),
                               std::floor((double(local.y) + c.radius) / width),
                               std::floor((double(local.z) + c.radius) / width)};
            // Capture only intersecting occupied cells; edits may erase a chunk while iterating.
            std::vector<std::pair<IVec3, MaterialId>> affected;
            for (auto &[coordinate, chunk] : obj.grid.chunks()) {
                if (coordinate.x < lo[0] || coordinate.y < lo[1] || coordinate.z < lo[2] ||
                    coordinate.x > hi[0] || coordinate.y > hi[1] || coordinate.z > hi[2])
                    continue;
                for (int z = 0; z < 32; ++z)
                    for (int y = 0; y < 32; ++y)
                        for (int x = 0; x < 32; ++x) {
                            auto m = chunk.cells[x + 32 * (y + 32 * z)];
                            if (m)
                                affected.push_back({{coordinate.x * 32 + x, coordinate.y * 32 + y,
                                                     coordinate.z * 32 + z},
                                                    m});
                        }
            }
            for (auto [p, m] : affected) {
                float d = length(cellCenter(p, size) - local);
                if (d > c.radius)
                    continue;
                obj.damage[p] += c.strength * std::max(.15f, 1.f - d / c.radius);
                if (obj.damage[p] >= materials[m].resistance) {
                    obj.grid.set(p, 0);
                    obj.damage.erase(p);
                    obj.anchors.erase(p);
                }
            }
        }
        std::erase_if(obj.seams,
                      [&](const Seam &s) { return !obj.grid.get(s.a) || !obj.grid.get(s.b); });
        if (obj.grid.count() > 2000000)
            throw std::runtime_error("Object voxel budget exceeded");
        result.collisions = prepareCollisions(obj.grid, size, std::move(result.collisions));
        ++obj.revision;
    } catch (const std::exception &ex) {
        result.error = ex.what();
    }
    return result;
}
bool impactPatch(Object &object, Vec3 local, float energy, const World &world) {
    if (energy <= 0)
        return false;
    const float size = world.voxelSize;
    const float radius =
        std::min(.5f, std::min(size * 5, std::max(size * 1.5f, std::cbrt(energy / 1e6f))));
    IVec3 center{int(std::floor(local.x / size)), int(std::floor(local.y / size)),
                 int(std::floor(local.z / size))};
    int reach = int(std::ceil(radius / size)) + 1;
    std::vector<std::pair<IVec3, float>> cells;
    float total = 0;
    for (int x = -reach; x <= reach; ++x)
        for (int y = -reach; y <= reach; ++y)
            for (int z = -reach; z <= reach; ++z) {
                auto cell = center + IVec3{x, y, z};
                if (!object.grid.get(cell))
                    continue;
                float distance = length(cellCenter(cell, size) - local);
                if (distance >= radius)
                    continue;
                float weight = 1.f - distance / radius;
                cells.emplace_back(cell, weight);
                total += weight;
            }
    if (total <= 0)
        return false;
    for (auto [cell, weight] : cells) {
        auto &material = world.materials.at(object.grid.get(cell));
        float damage = energy * weight / total / (material.impactToughness * size * size * size) *
                       material.resistance;
        object.damage[cell] += damage;
        if (object.damage[cell] >= material.resistance) {
            object.grid.set(cell, 0);
            object.damage.erase(cell);
            object.anchors.erase(cell);
        }
    }
    std::erase_if(object.seams,
                  [&](auto &s) { return !object.grid.get(s.a) || !object.grid.get(s.b); });
    ++object.revision;
    return true;
}

std::pair<CollisionPreparation, StructureGraph> validateLandscapeData(const LandscapeData &data,
                                                                      const World &world) {
    if (!data.grid.count() || data.grid.count() > 2000000 || data.anchors.empty())
        throw std::runtime_error("Landscape voxel/anchor budget invalid");
    for (auto &[coordinate, chunk] : data.grid.chunks())
        for (auto m : chunk.cells)
            if (m >= world.materials.size())
                throw std::runtime_error("Invalid landscape material");
    for (auto p : data.anchors)
        if (!data.grid.get(p))
            throw std::runtime_error("Landscape anchor requires occupied cell");
    auto collisions = prepareCollisions(data.grid, world.voxelSize);
    Object object;
    object.grid = data.grid;
    object.anchors = data.anchors;
    auto graph = Structure{}.prepare(object, world);
    return {std::move(collisions), std::move(graph)};
}
void landscapeWorldBudget(const World &world, const LandscapeData &data) {
    size_t cells = data.grid.count(), chunks = data.grid.chunks().size();
    for (auto &[id, object] : world.objects) {
        cells += object.grid.count();
        chunks += object.grid.chunks().size();
    }
    if (cells > 4000000 || chunks > 4096 || world.objects.size() >= 4096 || !world.nextEntity ||
        world.nextEntity == std::numeric_limits<EntityId>::max())
        throw std::runtime_error("Landscape exceeds world snapshot budget");
    // Version 3 stores each cell in 14 bytes and each anchor in 12. Include
    // existing metadata/damage/seams, which can dominate a heavily edited save.
    const size_t added = 102 + data.grid.count() * 14 + data.anchors.size() * 12;
    if (encodeWorld(world).size() + added > 128u * 1024u * 1024u)
        throw std::runtime_error("Landscape exceeds world snapshot byte budget");
}
EntityId publishLandscapeData(World &world, LandscapeData data) {
    landscapeWorldBudget(world, data);
    Object object;
    object.id = world.nextEntity;
    object.name = "Landscape";
    object.grid = std::move(data.grid);
    object.anchors = std::move(data.anchors);
    auto id = object.id;
    if (!world.objects.emplace(id, std::move(object)).second)
        throw std::runtime_error("Landscape entity identifier already exists");
    ++world.nextEntity;
    ++world.revision;
    return id;
}

} // namespace
struct Engine::Impl {
    Physics physics;
    Structure structure;
    Scripts scripts;
    std::deque<Command> queue;
    std::deque<Impact> impacts;
    struct Job {
        EntityId entity;
        uint64_t revision;
        std::future<Prepared> result;
    };
    std::vector<Job> jobs;
    std::map<EntityId, CollisionPreparation> collisionPreparations;
    struct LandscapeJob {
        uint64_t command{}, revision{};
        std::future<PreparedLandscape> result;
    };
    std::optional<LandscapeJob> landscape;
    // Cancelled requests retain their bounded worker slot until completion, but
    // world replacement never waits for their CPU-only generation work.
    std::vector<std::future<PreparedLandscape>> retiredLandscapes;
    void reapLandscapes() {
        std::erase_if(retiredLandscapes, [](auto &future) {
            if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return false;
            try {
                (void)future.get();
            } catch (...) {
            }
            return true;
        });
    }

    uint64_t nextCommand{1}, nextEvent{1};
};
Engine::Engine() : impl_(std::make_unique<Impl>()) {
    impl_->structure.physics = &impl_->physics;
    world.materials = {
        {"air", 0, 0, 1, .7f, 1, 1, 1, 1},
        {"concrete", 0xc5beb0ff, 1800, 3, .8f, 2000000, 20000000, 2000000, 3000000},
        {"timber", 0xa87645ff, 600, 1, .65f, 10000000, 20000000, 5000000, 10000000},
        {"steel", 0x628b95ff, 7800, 8, .5f, 200000000, 200000000, 100000000, 200000000},
        {"target", 0xe5a33dff, 900, 1, .6f, 100000, 500000, 150000, 150000}};
    world.materials.push_back(
        {"grass", 0x66854aff, 1500, 2, .85f, 200000, 2000000, 300000, 300000});
    world.materials.push_back({"soil", 0x856044ff, 1700, 3, .85f, 300000, 3000000, 400000, 400000});
    world.materials.push_back(
        {"rock", 0x858b92ff, 2400, 8, .8f, 5000000, 50000000, 8000000, 10000000});
    for (auto &m : world.materials)
        m.impactToughness = defaultImpactToughness(m.name);
}
Engine::~Engine() = default;
void Engine::emit(std::string type, EntityId entity, std::string message, uint64_t command) {
    events.push_back(
        {command, world.tick, std::move(type), std::move(message), entity, impl_->nextEvent++});
    while (events.size() > 256)
        events.pop_front();
}
uint64_t Engine::submit(Command c) {
    c.id = impl_->nextCommand++;
    if (impl_->queue.size() + impl_->jobs.size() >= 128) {
        emit("rejected", c.entity, "Command queue full", c.id);
        return c.id;
    }
    if (uint8_t(c.type) > uint8_t(CommandType::ReleaseBody) || !finite(c.point) ||
        !finite(c.vector) || !std::isfinite(c.radius) || !std::isfinite(c.strength) ||
        c.radius <= 0 || c.radius > 8 || c.strength < 0 || c.strength > 100000 ||
        length(c.vector) > 100000 || c.material >= world.materials.size() ||
        std::abs(int64_t(c.cell.x)) > 1280 || std::abs(int64_t(c.cell.y)) > 640 ||
        std::abs(int64_t(c.cell.z)) > 1280) {
        emit("rejected", c.entity, "Invalid command parameters", c.id);
        return c.id;
    }
    impl_->queue.push_back(c);
    return c.id;
}
void Engine::step(float dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f)
        throw std::runtime_error("Invalid simulation timestep");
    auto start = std::chrono::steady_clock::now();
    ++world.tick;
    stats.collisionPreparationMs = stats.physicsPublicationMs = 0;
    impl_->reapLandscapes();
    if (impl_->landscape &&
        impl_->landscape->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto command = impl_->landscape->command;
        try {
            auto prepared = impl_->landscape->result.get();
            if (simulating || !impl_->queue.empty() || !impl_->jobs.empty() ||
                world.revision != impl_->landscape->revision)
                throw std::runtime_error(
                    "Landscape generation cancelled: world changed or Play started");
            auto id = publishLandscapeData(world, std::move(prepared.data));
            impl_->structure.seed(id, std::move(prepared.structure));
            impl_->physics.seedCollisionCache(id, world.objects.at(id).grid, world.voxelSize,
                                              prepared.collisions.boxes);
            impl_->collisionPreparations[id] = std::move(prepared.collisions);
            emit("committed", id, "Landscape generated", command);
        } catch (const std::exception &ex) {
            emit("rejected", 0, ex.what(), command);
        }
        impl_->landscape.reset();
    }
    for (auto it = impl_->jobs.begin(); it != impl_->jobs.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto prepared = it->result.get();
        auto found = world.objects.find(it->entity);
        if (!prepared.error.empty())
            emit("rejected", it->entity, prepared.error, prepared.command.id);
        else if (found == world.objects.end() || found->second.revision != it->revision)
            emit("rejected", it->entity, "Stale prepared revision", prepared.command.id);
        else {
            // Preserve motion that advanced while voxel work ran off-thread.
            prepared.object.transform = found->second.transform;
            prepared.object.velocity = found->second.velocity;
            prepared.object.angularVelocity = found->second.angularVelocity;
            found->second = std::move(prepared.object);
            impl_->physics.seedCollisionCache(it->entity, found->second.grid, world.voxelSize,
                                              prepared.collisions.boxes);
            impl_->collisionPreparations[it->entity] = std::move(prepared.collisions);
            ++world.revision;
            emit("committed", it->entity, "Voxel transaction applied", prepared.command.id);
            emit(prepared.command.type == CommandType::SphereDamage ? "damage" : "construction",
                 it->entity);
        }
        it = impl_->jobs.erase(it);
    }
    size_t process = impl_->queue.size();
    while (process-- && !impl_->queue.empty() &&
           impl_->jobs.size() + size_t(impl_->landscape.has_value()) +
                   impl_->retiredLandscapes.size() <
               4) {
        auto c = impl_->queue.front();
        impl_->queue.pop_front();
        if (std::any_of(impl_->jobs.begin(), impl_->jobs.end(),
                        [&](auto &j) { return j.entity == c.entity; })) {
            impl_->queue.push_back(c);
            continue;
        }
        if (c.type == CommandType::SpawnPrefab && !c.entity) {
            VoxelGrid grid;
            int n = std::clamp(int(c.radius / world.voxelSize), 1, 16);
            for (int x = 0; x < n; ++x)
                for (int y = 0; y < n; ++y)
                    for (int z = 0; z < n; ++z)
                        grid.set({x, y, z}, std::max<MaterialId>(1, c.material));
            auto id = world.spawn("Block", std::move(grid), {c.point, {}}, true);
            emit("committed", id, "Prefab spawned", c.id);
            continue;
        }
        auto found = world.objects.find(c.entity);
        if (found == world.objects.end()) {
            emit("rejected", c.entity, "Entity not found", c.id);
            continue;
        }
        if (c.type == CommandType::ReleaseBody) {
            auto &object = found->second;
            if ((c.expectedRevision && c.expectedRevision != object.revision) ||
                length(c.vector) > 50 || !simulating) {
                emit("rejected", c.entity,
                     "Release requires Play, current revision, and speed <= 50 m/s", c.id);
            } else if (std::any_of(world.joints.begin(), world.joints.end(),
                                   [&](auto &j) { return j.a == c.entity || j.b == c.entity; })) {
                emit("rejected", c.entity, "Remove attached joints before releasing this object",
                     c.id);
            } else {
                object.anchors.clear();
                object.dynamic = true;
                object.velocity = c.vector;
                object.angularVelocity = {};
                ++object.revision;
                ++world.revision;
                emit("committed", c.entity, "Object released", c.id);
            }
            continue;
        }
        if (c.type == CommandType::Impulse) {
            impl_->physics.sync(world);
            impl_->physics.impulse(c.entity, c.vector, c.point);
            emit("committed", c.entity, "Impulse applied", c.id);
            continue;
        }
        auto copy = found->second;
        float size = world.voxelSize;
        auto materials = world.materials;
        auto collisions = impl_->collisionPreparations[c.entity];
        impl_->jobs.push_back(
            {c.entity, copy.revision,
             std::async(std::launch::async, [c, copy = std::move(copy), size,
                                             materials = std::move(materials),
                                             collisions = std::move(collisions)]() mutable {
                 return prepare(c, std::move(copy), size, materials, std::move(collisions));
             })});
    }
    if (!simulating) {
        stats.pendingCommands = uint32_t(impl_->queue.size() + impl_->jobs.size() +
                                         size_t(impl_->landscape.has_value()));
        return;
    }
    stats.processedImpacts = 0;
    size_t pending = impl_->impacts.size();
    while (pending-- && !impl_->impacts.empty() && stats.processedImpacts < 64) {
        auto hit = impl_->impacts.front();
        impl_->impacts.pop_front();
        if (std::any_of(impl_->jobs.begin(), impl_->jobs.end(),
                        [&](auto &j) { return j.entity == hit.a || j.entity == hit.b; })) {
            impl_->impacts.push_back(hit);
            continue;
        }
        // Prepare both sides from current revisions, then publish together on this thread.
        // No worker or physics callback can mutate the world across this boundary.
        std::vector<std::pair<Object, CollisionPreparation>> prepared;
        try {
            for (auto [id, local] : {std::pair{hit.a, hit.localA}, std::pair{hit.b, hit.localB}}) {
                auto it = world.objects.find(id);
                if (it == world.objects.end())
                    continue;
                Object copy = it->second;
                if (impactPatch(copy, local, hit.energy * .5f, world)) {
                    auto collisions = prepareCollisions(copy.grid, world.voxelSize,
                                                        impl_->collisionPreparations[id]);
                    prepared.emplace_back(std::move(copy), std::move(collisions));
                }
            }
            for (auto &[object, collisions] : prepared) {
                auto id = object.id;
                world.objects.at(id) = std::move(object);
                impl_->physics.seedCollisionCache(id, world.objects.at(id).grid, world.voxelSize,
                                                  collisions.boxes);
                impl_->collisionPreparations[id] = std::move(collisions);
                ++world.revision;
                emit("damage", id, "Collision damage committed");
            }
        } catch (const std::exception &ex) {
            emit("budget", hit.a, ex.what());
        }
        ++stats.processedImpacts;
    }
    impl_->scripts.tick(*this, dt);
    try {
        impl_->structure.update(*this, dt, impl_->physics.forces);
    } catch (const std::exception &ex) {
        emit("budget", 0, ex.what());
    }
    auto physicsStart = std::chrono::steady_clock::now();
    impl_->physics.sync(world);
    std::erase_if(impl_->collisionPreparations,
                  [&](auto &entry) { return !world.objects.contains(entry.first); });
    impl_->physics.step(world, dt);
    stats.droppedImpacts += impl_->physics.droppedImpacts;
    for (auto &hit : impl_->physics.impacts) {
        emit("collision", hit.a,
             "Impact " + std::to_string(hit.energy) + " J at " + std::to_string(hit.closing) +
                 " m/s");
        events.back().collision =
            CollisionDetails{hit.b, hit.point, hit.normal, hit.closing, hit.energy};
        if (impl_->impacts.size() < 256)
            impl_->impacts.push_back(hit);
        else
            ++stats.droppedImpacts;
    }
    stats.pendingImpacts = uint32_t(impl_->impacts.size());
    stats.physicsMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - physicsStart)
            .count();
    stats.collisionChunkRebuilds = impl_->physics.collisionChunkRebuilds;
    stats.collisionBodyRebuilds = impl_->physics.collisionBodyRebuilds;
    stats.collisionCacheBytes = impl_->physics.collisionCacheBytes;
    stats.collisionBodyCount = impl_->physics.collisionBodyCount;
    stats.queuedCollisionChunks = impl_->physics.queuedCollisionChunks;
    stats.collisionPreparationMs += impl_->physics.collisionPreparationMs;
    stats.physicsPublicationMs = impl_->physics.publicationMs;
    stats.activeBodies = 0;
    for (auto &[id, o] : world.objects)
        stats.activeBodies += o.dynamic;
    stats.pendingCommands =
        uint32_t(impl_->queue.size() + impl_->jobs.size() + size_t(impl_->landscape.has_value()));
    stats.frameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void Engine::validateLandscape(const LandscapeData &data) const {
    landscapeWorldBudget(world, data);
    validateLandscapeData(data, world);
}
EntityId Engine::publishLandscape(LandscapeData data) {
    landscapeWorldBudget(world, data);
    auto [collisions, structure] = validateLandscapeData(data, world);
    auto id = publishLandscapeData(world, std::move(data));
    impl_->structure.seed(id, std::move(structure));
    impl_->physics.seedCollisionCache(id, world.objects.at(id).grid, world.voxelSize,
                                      collisions.boxes);
    impl_->collisionPreparations[id] = std::move(collisions);
    return id;
}
EntityId Engine::generateLandscape(const LandscapeSettings &settings) {
    for (auto m : {settings.grassMaterial, settings.soilMaterial, settings.rockMaterial})
        if (!m || m >= world.materials.size())
            throw std::runtime_error("Invalid landscape material");
    return publishLandscape(aspera::generateLandscape(settings));
}
uint64_t Engine::requestLandscape(const LandscapeSettings &settings) {
    const auto command = impl_->nextCommand++;
    impl_->reapLandscapes();
    try {
        validateLandscapeSettings(settings);
        for (auto m : {settings.grassMaterial, settings.soilMaterial, settings.rockMaterial})
            if (!m || m >= world.materials.size())
                throw std::runtime_error("Invalid landscape material");
        if (simulating || impl_->landscape || !impl_->jobs.empty() || !impl_->queue.empty() ||
            impl_->retiredLandscapes.size() >= 4)
            throw std::runtime_error(
                "Stop simulation and wait for pending edits before generating");
        World context;
        context.materials = world.materials;
        context.voxelSize = world.voxelSize;
        impl_->landscape.emplace(Impl::LandscapeJob{
            command, world.revision,
            std::async(std::launch::async, [settings, context = std::move(context)] {
                auto data = aspera::generateLandscape(settings);
                auto [collisions, structure] = validateLandscapeData(data, context);
                return PreparedLandscape{std::move(data), std::move(collisions),
                                         std::move(structure)};
            })});
    } catch (const std::exception &ex) {
        emit("rejected", 0, ex.what(), command);
    }
    return command;
}
void Engine::resetPhysics() {
    if (impl_->landscape) {
        emit("rejected", 0, "Landscape generation cancelled: world reset",
             impl_->landscape->command);
        impl_->landscape.reset();
    }
    impl_->jobs.clear();
    impl_->collisionPreparations.clear();
    impl_->queue.clear();
    impl_->impacts.clear();
    stats.pendingImpacts = stats.droppedImpacts = stats.processedImpacts = 0;
    impl_->physics.clear();
    impl_->structure.clear();
}
void Engine::loadProject(const std::filesystem::path &path) {
    resetPhysics();
    world.objects.clear();
    world.joints.clear();
    world.properties.clear();
    world.tick = 0;
    world.revision = 0;
    world.nextEntity = 1;
    world.environment = {};
    world.lights.clear();
    world.nextLight = 1;
    impl_->scripts.load(*this, path, true);
    if (std::filesystem::exists(path / "scene.aspera"))
        world = loadWorld(path / "scene.aspera");
    uint32_t hash = 2166136261u;
    std::vector<std::filesystem::path> files;
    for (auto &entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.is_regular_file() &&
            (entry.path().extension() == ".lua" || entry.path().extension() == ".avox" ||
             entry.path().filename() == "scene.aspera"))
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (auto &file : files) {
        for (auto c : std::filesystem::relative(file, path).generic_string()) {
            hash ^= uint8_t(c);
            hash *= 16777619u;
        }
        std::ifstream f(file, std::ios::binary);
        char c;
        while (f.get(c)) {
            hash ^= uint8_t(c);
            hash *= 16777619u;
        }
    }
    world.properties["_content_hash"] = hash;
}
void Engine::reloadScripts() {
    impl_->scripts.load(*this, impl_->scripts.project, false);
}
std::string Engine::projectName() const {
    return impl_->scripts.name;
}
void Engine::setInput(std::string action, float value) {
    impl_->scripts.input(action, value);
}
void Engine::setEnvironment(const EnvironmentSettings &environment) {
    if (!validEnvironment(environment))
        throw std::runtime_error("Invalid environment settings");
    world.environment = environment;
    ++world.revision;
}
void Engine::setMaterial(MaterialId id, const Material &material) {
    if (id >= world.materials.size() || !validMaterial(material))
        throw std::runtime_error("Invalid material");
    bool physicsChanged = physicsMaterialChanged(world.materials[id], material);
    world.materials[id] = material;
    if (physicsChanged) {
        for (auto &[_, object] : world.objects) {
            auto cells = object.grid.occupied();
            if (std::any_of(cells.begin(), cells.end(),
                            [id](auto cell) { return cell.second == id; }))
                ++object.revision;
        }
        // Material data is captured by asynchronous voxel jobs.  Keep those jobs and queued
        // commands so the normal object-revision check can reject stale work and later commands
        // can run against the new material state.  resetPhysics() would silently discard both.
        impl_->impacts.clear();
        stats.pendingImpacts = 0;
        impl_->physics.clear();
        impl_->structure.materialsChanged(world);
    }
    ++world.revision;
}
LightId Engine::addLight(Light light) {
    if (world.lights.size() >= 64 || !validLight(light) || world.nextLight == 0 ||
        world.nextLight >= std::numeric_limits<LightId>::max() - 1)
        throw std::runtime_error("Invalid light");
    light.id = world.nextLight++;
    world.lights.emplace(light.id, light);
    ++world.revision;
    return light.id;
}
void Engine::updateLight(LightId id, const Light &light) {
    if (!world.lights.contains(id) || !validLight(light))
        throw std::runtime_error("Invalid light");
    Light value = light;
    value.id = id;
    world.lights[id] = value;
    ++world.revision;
}
void Engine::removeLight(LightId id) {
    if (!world.lights.erase(id))
        throw std::runtime_error("Unknown light");
    ++world.revision;
}
std::vector<EntityId> Engine::importAsset(const std::filesystem::path &path) {
    auto scene = path.extension() == ".vox" ? importVox(path, world.voxelSize) : readAsset(path);
    if (world.materials.size() + scene.materials.size() > 4096 ||
        world.objects.size() + scene.objects.size() > 4096)
        throw std::runtime_error("Imported scene exceeds project limits");
    World candidate = world;
    MaterialId offset = MaterialId(candidate.materials.size() - 1);
    for (size_t i = 1; i < scene.materials.size(); ++i)
        candidate.materials.push_back(scene.materials[i]);
    std::vector<EntityId> ids;
    for (auto &a : scene.objects) {
        VoxelGrid grid;
        for (auto [p, m] : a.grid.occupied())
            grid.set(p, MaterialId(m + offset));
        auto id = candidate.spawn(a.name, std::move(grid), a.transform, true);
        ids.push_back(id);
    }
    world = std::move(candidate);
    return ids;
}
} // namespace aspera
