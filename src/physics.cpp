#include "internal.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace aspera {
namespace {
JPH::Vec3 jv(Vec3 v) {
    return {v.x, v.y, v.z};
}
Vec3 av(JPH::Vec3 v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
Vec3 bodyLocalPoint(const JPH::Body &body, JPH::RVec3 point) {
    // Jolt's GetPosition() is the body origin (GetCenterOfMassPosition() is
    // the separate center-of-mass accessor). Keep contact coordinates in the
    // object-local frame used by voxel damage and support aggregation.
    auto relative = JPH::Vec3(point - body.GetPosition());
    auto local = body.GetRotation().Conjugated() * relative;
    return av(local);
}
struct JoltLifetime {
    JoltLifetime() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory;
        JPH::RegisterTypes();
    }
    ~JoltLifetime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};
struct BroadPhase : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override {
        return 2;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return JPH::BroadPhaseLayer(l == 0 ? 0 : 1);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l.GetValue() == 0 ? "Static" : "Dynamic";
    }
#endif
};
struct LayerFilter : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return a != 0 || b.GetValue() != 0;
    }
};
struct PairFilter : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == 2 && b == 2) && (a != 0 || b != 0);
    }
};
struct CharacterLayerFilter : JPH::ObjectLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer != 2;
    }
};
} // namespace
struct Physics::Impl : JPH::ContactListener {
    CharacterLayerFilter characterLayers;
    bool prediction{};
    uint64_t publishedRevision{};
    std::map<PlayerId, JPH::Ref<JPH::CharacterVirtual>> characters;
    JPH::Ref<JPH::CharacterVirtual> makeCharacter(Vec3 position) {
        JPH::CharacterVirtualSettings settings;
        settings.mShape = new JPH::RotatedTranslatedShape(
            JPH::Vec3(0, .9f, 0), JPH::Quat::sIdentity(), new JPH::CapsuleShape(.6f, .3f));
        if (!prediction) {
            settings.mInnerBodyShape = new JPH::RotatedTranslatedShape(
                JPH::Vec3(0, .9f, 0), JPH::Quat::sIdentity(), new JPH::CapsuleShape(.6f, .28f));
            settings.mInnerBodyLayer = 2;
        }
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(45);
        settings.mMaxStrength = prediction ? 0.f : 100.f;
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -.3f);
        return new JPH::CharacterVirtual(&settings, JPH::RVec3(position.x, position.y, position.z),
                                         JPH::Quat::sIdentity(), 0, &system);
    }
    BroadPhase broad;
    LayerFilter layer;
    PairFilter pair;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl temp{32 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2};
    struct BodyRecord {
        JPH::BodyID id;
        IVec3 chunk{};
        uint64_t cacheKey{};
    };
    struct ObjectRecord {
        uint64_t geometryKey{};
        float voxelSize{};
        bool dynamic{};
        std::vector<BodyRecord> parts;
    };
    struct CachedChunk {
        uint64_t key{};
        float voxelSize{};
        std::vector<CollisionBox> boxes;
    };
    std::map<EntityId, ObjectRecord> bodies;
    std::map<EntityId, std::map<IVec3, CachedChunk>> collisionCache;
    std::map<uint64_t, JPH::Ref<JPH::TwoBodyConstraint>> joints;
    std::mutex mutex;
    std::map<EntityId, std::map<IVec3, Vec3>> forces;
    float voxelSize{.1f}, dt{1.f / 60.f};
    uint64_t tick{};
    std::map<std::pair<EntityId, EntityId>, uint64_t> lastImpact;
    std::map<std::pair<EntityId, EntityId>, Impact> impacts;
    uint32_t dropped{};
    uint64_t collisionChunkRebuilds{};
    uint64_t collisionBodyRebuilds{};
    uint32_t collisionBodyCount{};
    size_t collisionCacheBytes{};
    double collisionPreparationMs{};
    uint32_t queuedCollisionChunks{};
    Impl() {
        system.Init(8192, 0, 32768, 32768, broad, layer, pair);
        system.SetContactListener(this);
    }
    void contact(const JPH::Body &a, const JPH::Body &b, const JPH::ContactManifold &m,
                 const JPH::ContactSettings &settings) {
        // Character proxy bodies participate in physical collision but never in voxel damage.
        if (!a.GetUserData() || !b.GetUserData())
            return;
        if (m.mRelativeContactPointsOn1.empty())
            return;
        auto normal = m.mWorldSpaceNormal;
        size_t strongest = 0;
        float closing = 0;
        for (size_t i = 0; i < m.mRelativeContactPointsOn1.size(); ++i) {
            auto point = m.GetWorldSpaceContactPointOn1(uint(i));
            float speed = (a.GetPointVelocity(point) - b.GetPointVelocity(point)).Dot(normal);
            if (speed > closing) {
                closing = speed;
                strongest = i;
            }
        }
        auto pointA = m.GetWorldSpaceContactPointOn1(uint(strongest));
        auto pointB = m.GetWorldSpaceContactPointOn2(uint(strongest));
        float massA = a.IsDynamic() ? 1.f / a.GetMotionProperties()->GetInverseMass() : 0;
        float massB = b.IsDynamic() ? 1.f / b.GetMotionProperties()->GetInverseMass() : 0;
        float effective = massA && massB ? massA * massB / (massA + massB) : std::max(massA, massB);
        float impulse = 0;
        if (closing > 1.f) {
            JPH::CollisionEstimationResult estimate;
            JPH::EstimateCollisionResponse(a, b, m, estimate, settings.mCombinedFriction,
                                           settings.mCombinedRestitution, 1.f, 4);
            for (auto &v : estimate.mImpulses)
                impulse += v.mContactImpulse;
        }
        auto localA = bodyLocalPoint(a, pointA);
        auto localB = bodyLocalPoint(b, pointB);
        auto key = std::pair{std::min(a.GetUserData(), b.GetUserData()),
                             std::max(a.GetUserData(), b.GetUserData())};
        std::lock_guard lock(mutex);
        // Sustained support is distinct from the one-shot impact impulse.
        auto add = [&](const JPH::Body &body, Vec3 local, float sign, float force) {
            IVec3 c{int(std::floor(local.x / voxelSize)), int(std::floor(local.y / voxelSize)),
                    int(std::floor(local.z / voxelSize))};
            auto f = body.GetRotation().Conjugated() * (normal * (force * sign));
            auto &old = forces[body.GetUserData()][c];
            if (length(av(f)) > length(old))
                old = av(f);
        };
        float force = effective * 9.81f * std::abs(normal.GetY());
        bool accepted = impulse > 0 && std::isfinite(impulse) &&
                        (!lastImpact.contains(key) || tick - lastImpact[key] >= 3);
        if (accepted) {
            float energy = std::min(1e7f, .5f * impulse * closing *
                                              std::max(0.f, 1.f - settings.mCombinedRestitution));
            Impact hit{a.GetUserData(), b.GetUserData(), localA,  localB, av(JPH::Vec3(pointA)),
                       av(normal),      closing,         impulse, energy};
            if (impacts.contains(key)) {
                if (energy > impacts[key].energy)
                    impacts[key] = hit;
            } else if (impacts.size() < 256)
                impacts.emplace(key, hit);
            else
                ++dropped;
            force += std::min(1e8f, impulse / dt);
        }
        add(a, localA, -1, force);
        add(b, localB, 1, force);
    }
    void OnContactAdded(const JPH::Body &a, const JPH::Body &b, const JPH::ContactManifold &m,
                        JPH::ContactSettings &s) override {
        contact(a, b, m, s);
    }
    void OnContactPersisted(const JPH::Body &a, const JPH::Body &b, const JPH::ContactManifold &m,
                            JPH::ContactSettings &s) override {
        contact(a, b, m, s);
    }
    static void destroyParts(JPH::BodyInterface &bi, ObjectRecord &record) {
        for (auto &part : record.parts) {
            if (part.id.IsInvalid())
                continue;
            bi.RemoveBody(part.id);
            bi.DestroyBody(part.id);
        }
        record.parts.clear();
    }
    void updateCacheBytes() {
        size_t bytes = 0;
        for (auto &[_, chunks] : collisionCache)
            for (auto &[__, cached] : chunks)
                bytes += cached.boxes.size() * sizeof(CollisionBox);
        collisionCacheBytes = bytes;
    }
    void refreshCollisionCache(EntityId id, const VoxelGrid &grid, float size) {
        auto &cache = collisionCache[id];
        size_t pending = 0;
        for (const auto &[coordinate, _] : grid.chunks()) {
            auto it = cache.find(coordinate);
            if (it == cache.end() || it->second.key != grid.chunkContentKey(coordinate) ||
                it->second.voxelSize != size)
                ++pending;
        }
        queuedCollisionChunks += uint32_t(std::min<size_t>(pending, UINT32_MAX));
        for (const auto &[coordinate, _] : grid.chunks()) {
            const uint64_t key = grid.chunkContentKey(coordinate);
            auto it = cache.find(coordinate);
            if (it == cache.end() || it->second.key != key || it->second.voxelSize != size) {
                CachedChunk value;
                value.key = key;
                value.voxelSize = size;
                value.boxes = collisionBoxesChunk(grid, coordinate, size);
                if (value.boxes.size() > 32768)
                    throw std::runtime_error("Collision complexity budget exceeded");
                cache[coordinate] = std::move(value);
                ++collisionChunkRebuilds;
                if (queuedCollisionChunks)
                    --queuedCollisionChunks;
            }
        }
        for (auto it = cache.begin(); it != cache.end();) {
            if (!grid.chunks().contains(it->first))
                it = cache.erase(it);
            else
                ++it;
        }
        updateCacheBytes();
    }
    static void addBoxes(JPH::StaticCompoundShapeSettings &compound,
                         const std::vector<CollisionBox> &boxes, const VoxelGrid &grid, float size,
                         const std::vector<Material> &materials) {
        for (const auto &box : boxes) {
            auto *shape = new JPH::BoxShape(jv(box.halfExtent), std::min(.01f, size * .05f));
            // Collision boxes are emitted from whole cells, so rounding the
            // lower corner recovers a stable material for merged boxes,
            // including boxes at negative coordinates.
            IVec3 cell{int(std::llround((box.center.x - box.halfExtent.x) / size)),
                       int(std::llround((box.center.y - box.halfExtent.y) / size)),
                       int(std::llround((box.center.z - box.halfExtent.z) / size))};
            MaterialId material = grid.get(cell);
            shape->SetDensity(
                material < materials.size() ? std::max(1.f, materials[material].density) : 1000.f);
            compound.AddShape(jv(box.center), JPH::Quat::sIdentity(), shape);
        }
    }
    static size_t bodyPartsNeeded(const Object &object) {
        if (!object.grid.count())
            return 0;
        return object.dynamic ? 1 : object.grid.chunks().size();
    }
};
Physics::Physics() {
    static JoltLifetime lifetime;
    p = std::make_unique<Impl>();
}
Physics::~Physics() {
    clear();
}
void Physics::clear() {
    p->characters.clear();
    p->publishedRevision = 0;
    for (auto &[id, j] : p->joints)
        p->system.RemoveConstraint(j);
    p->joints.clear();
    auto &bi = p->system.GetBodyInterface();
    for (auto &[id, record] : p->bodies)
        Impl::destroyParts(bi, record);
    p->bodies.clear();
    p->collisionCache.clear();
    p->collisionCacheBytes = 0;
    p->collisionBodyCount = 0;
    p->queuedCollisionChunks = 0;
    p->collisionPreparationMs = 0;
    p->collisionChunkRebuilds = 0;
    p->collisionBodyRebuilds = 0;
    forces.clear();
    impacts.clear();
    p->lastImpact.clear();
    p->impacts.clear();
    collisionChunkRebuilds = 0;
    collisionBodyRebuilds = 0;
    collisionBodyCount = 0;
    collisionCacheBytes = 0;
    collisionPreparationMs = 0;
    publicationMs = 0;
    queuedCollisionChunks = 0;
}
void Physics::seedCollisionCache(EntityId id, const VoxelGrid &grid, float size,
                                 const std::map<IVec3, std::vector<CollisionBox>> &collisions) {
    auto &cache = p->collisionCache[id];
    for (const auto &[coordinate, boxes] : collisions) {
        if (!grid.chunks().contains(coordinate))
            continue;
        const uint64_t key = grid.chunkContentKey(coordinate);
        auto it = cache.find(coordinate);
        if (it != cache.end() && it->second.key == key && it->second.voxelSize == size)
            continue;
        cache[coordinate] = Impl::CachedChunk{key, size, boxes};
        ++p->collisionChunkRebuilds;
    }
    // A seed is a complete snapshot for this entity. Drop chunks removed from
    // the grid so a later sync cannot retain stale collision data.
    for (auto it = cache.begin(); it != cache.end();) {
        if (!grid.chunks().contains(it->first))
            it = cache.erase(it);
        else
            ++it;
    }
    p->updateCacheBytes();
    collisionChunkRebuilds = p->collisionChunkRebuilds;
    collisionCacheBytes = p->collisionCacheBytes;
}
std::map<IVec3, std::vector<CollisionBox>>
Physics::prepareFragmentCollisions(EntityId source, const VoxelGrid &grid, float size) const {
    std::map<IVec3, std::vector<CollisionBox>> result;
    auto old = p->collisionCache.find(source);
    size_t count = 0;
    for (auto &[co, chunk] : grid.chunks()) {
        bool reused = false;
        if (old != p->collisionCache.end()) {
            auto cached = old->second.find(co);
            if (cached != old->second.end() && cached->second.key == chunk.contentKey &&
                cached->second.voxelSize == size) {
                result[co] = cached->second.boxes;
                reused = true;
            }
        }
        if (!reused)
            result[co] = collisionBoxesChunk(grid, co, size);
        count += result[co].size();
        if (count > 32768)
            throw std::runtime_error("Fragment collision budget exceeded; split deferred");
    }
    return result;
}
void Physics::seedFragmentCollisionCaches(const std::vector<CollisionSeed> &seeds, float size) {
    decltype(p->collisionCache) staged;
    uint64_t rebuilt = 0;
    for (const auto &seed : seeds) {
        if (!seed.grid || !seed.boxes || staged.contains(seed.id))
            throw std::runtime_error("Invalid fragment collision seed");
        auto &cache = staged[seed.id];
        size_t count = 0;
        for (const auto &[co, boxes] : *seed.boxes) {
            if (!seed.grid->chunks().contains(co))
                throw std::runtime_error("Fragment collision seed does not match grid");
            count += boxes.size();
            if (count > 32768)
                throw std::runtime_error("Fragment collision budget exceeded; split deferred");
            const auto key = seed.grid->chunkContentKey(co);
            cache.emplace(co, Impl::CachedChunk{key, size, boxes});
            auto old = p->collisionCache.find(seed.id);
            bool same = false;
            if (old != p->collisionCache.end()) {
                auto chunk = old->second.find(co);
                same = chunk != old->second.end() && chunk->second.key == key &&
                       chunk->second.voxelSize == size;
            }
            rebuilt += !same;
        }
        if (cache.size() != seed.grid->chunks().size())
            throw std::runtime_error("Incomplete fragment collision seed");
    }
    // No allocations or shape creation beyond this boundary. An allocation or
    // validation failure above leaves every live entity's cache untouched.
    for (const auto &seed : seeds)
        p->collisionCache.erase(seed.id);
    p->collisionCache.merge(staged);
    p->collisionChunkRebuilds += rebuilt;
    p->updateCacheBytes();
    collisionChunkRebuilds = p->collisionChunkRebuilds;
    collisionCacheBytes = p->collisionCacheBytes;
}
void Physics::sync(World &w) {
    const auto preparationStart = std::chrono::steady_clock::now();
    auto &bi = p->system.GetBodyInterface();
    p->voxelSize = w.voxelSize;
    p->queuedCollisionChunks = 0;
    std::erase_if(p->collisionCache, [&](auto &entry) { return !w.objects.contains(entry.first); });

    // Work out the complete replacement set before touching Jolt. This lets
    // us refresh the independent per-chunk cache first and reject complexity
    // failures while the previous body set is still intact.
    std::set<EntityId> rebuild;
    for (const auto &[id, record] : p->bodies) {
        auto found = w.objects.find(id);
        if (found == w.objects.end() || record.geometryKey != found->second.grid.contentKey() ||
            record.dynamic != found->second.dynamic || record.voxelSize != w.voxelSize)
            rebuild.insert(id);
    }
    for (const auto &[id, object] : w.objects)
        if ((!p->bodies.contains(id) && object.grid.count()) || rebuild.contains(id))
            rebuild.insert(id);

    for (auto id : rebuild) {
        auto found = w.objects.find(id);
        if (found == w.objects.end() || !found->second.grid.count()) {
            p->collisionCache.erase(id);
            continue;
        }
        p->refreshCollisionCache(id, found->second.grid, w.voxelSize);
    }

    size_t oldParts = 0, newParts = 0;
    for (auto id : rebuild) {
        if (auto old = p->bodies.find(id); old != p->bodies.end())
            oldParts += old->second.parts.size();
        if (auto found = w.objects.find(id); found != w.objects.end())
            newParts += Impl::bodyPartsNeeded(found->second);
    }
    const size_t currentBodies = p->system.GetNumBodies();
    if (currentBodies < oldParts || currentBodies - oldParts + newParts > p->system.GetMaxBodies())
        throw std::runtime_error("Rigid body budget exceeded");

    // Cache/box generation is preparation; Jolt shape/body staging and publication
    // are measured separately, including constraints and body bookkeeping.
    const auto publicationStart = std::chrono::steady_clock::now();
    p->collisionPreparationMs =
        std::chrono::duration<double, std::milli>(publicationStart - preparationStart).count();
    if (!rebuild.empty()) {
        // Allocate and validate every replacement shape/body before retiring live
        // bodies. Unassigned Jolt bodies consume no body IDs, so replacements work
        // even when the final body count is at the configured limit.
        struct UnassignedDeleter {
            JPH::BodyInterface *interface;
            void operator()(JPH::Body *body) const {
                if (body)
                    interface->DestroyBodyWithoutID(body);
            }
        };
        using OwnedBody = std::unique_ptr<JPH::Body, UnassignedDeleter>;
        struct PendingPart {
            EntityId entity;
            size_t index;
            OwnedBody body;
            bool dynamic;
        };
        std::vector<PendingPart> pending;
        pending.reserve(newParts);
        auto nextBodies = p->bodies;
        for (auto id : rebuild) {
            auto found = w.objects.find(id);
            if (found == w.objects.end() || !found->second.grid.count()) {
                nextBodies.erase(id);
                continue;
            }
            const auto &object = found->second;
            const auto &cache = p->collisionCache.at(id);
            size_t boxCount = 0;
            for (const auto &[coordinate, chunk] : cache) {
                boxCount += chunk.boxes.size();
                if (boxCount > 32768)
                    throw std::runtime_error("Collision complexity budget exceeded");
            }
            float mass = 0;
            if (object.dynamic)
                for (auto &[coordinate, chunk] : object.grid.chunks())
                    for (auto material : chunk.cells)
                        if (material)
                            mass += w.materials.at(material).density * w.voxelSize * w.voxelSize *
                                    w.voxelSize;
            Impl::ObjectRecord replacement;
            replacement.geometryKey = object.grid.contentKey();
            replacement.voxelSize = w.voxelSize;
            replacement.dynamic = object.dynamic;
            replacement.parts.reserve(object.dynamic ? 1 : cache.size());
            auto stage = [&](IVec3 coordinate, uint64_t key,
                             const std::vector<CollisionBox> &boxes) {
                if (boxes.empty())
                    return;
                JPH::StaticCompoundShapeSettings compound;
                Impl::addBoxes(compound, boxes, object.grid, w.voxelSize, w.materials);
                auto shape = compound.Create();
                if (!shape.IsValid())
                    throw std::runtime_error(shape.HasError() ? shape.GetError().c_str()
                                                              : "Unable to create collision shape");
                auto q = object.transform.rotation;
                JPH::BodyCreationSettings settings(
                    shape.Get(),
                    JPH::RVec3(object.transform.position.x, object.transform.position.y,
                               object.transform.position.z),
                    JPH::Quat(q.x, q.y, q.z, q.w),
                    object.dynamic
                        ? (p->prediction ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic)
                        : JPH::EMotionType::Static,
                    object.dynamic ? 1 : 0);
                settings.mUserData = id;
                settings.mFriction = .7f;
                settings.mRestitution = .05f;
                if (object.dynamic) {
                    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
                    settings.mLinearVelocity = jv(object.velocity);
                    settings.mAngularVelocity = jv(object.angularVelocity);
                    settings.mOverrideMassProperties =
                        JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass = std::max(.01f, mass);
                }
                OwnedBody body(bi.CreateBodyWithoutID(settings), UnassignedDeleter{&bi});
                if (!body)
                    throw std::runtime_error("Unable to allocate collision body");
                const size_t index = replacement.parts.size();
                replacement.parts.push_back({JPH::BodyID{}, coordinate, key});
                pending.push_back({id, index, std::move(body), object.dynamic});
            };
            if (object.dynamic) {
                std::vector<CollisionBox> boxes;
                boxes.reserve(boxCount);
                for (const auto &[coordinate, chunk] : cache)
                    boxes.insert(boxes.end(), chunk.boxes.begin(), chunk.boxes.end());
                stage({}, replacement.geometryKey, boxes);
            } else {
                auto old = p->bodies.find(id);
                const bool reusable = old != p->bodies.end() && !old->second.dynamic &&
                                      old->second.voxelSize == w.voxelSize;
                for (const auto &[coordinate, chunk] : cache) {
                    if (chunk.boxes.empty())
                        continue;
                    bool kept = false;
                    if (reusable)
                        for (const auto &part : old->second.parts)
                            if (part.chunk == coordinate && part.cacheKey == chunk.key) {
                                replacement.parts.push_back(part);
                                kept = true;
                                break;
                            }
                    if (!kept)
                        stage(coordinate, chunk.key, chunk.boxes);
                }
            }
            nextBodies[id] = std::move(replacement);
        }
        std::set<JPH::BodyID> kept;
        for (auto &[id, record] : nextBodies)
            for (auto &part : record.parts)
                if (!part.id.IsInvalid())
                    kept.insert(part.id);
        struct RetiredPart {
            JPH::BodyID id;
            JPH::Body *body;
            bool active;
        };
        std::vector<RetiredPart> retired;
        retired.reserve(oldParts);
        if (!rebuild.empty()) {
            for (auto &[id, joint] : p->joints)
                p->system.RemoveConstraint(joint);
        }
        for (auto &[id, record] : p->bodies)
            for (auto &part : record.parts)
                if (!kept.contains(part.id)) {
                    bool active = bi.IsActive(part.id);
                    bi.RemoveBody(part.id);
                    retired.push_back({part.id, bi.UnassignBodyID(part.id), active});
                }
        try {
            for (auto &part : pending) {
                if (!bi.AssignBodyID(part.body.get()))
                    throw std::runtime_error("Rigid body budget exceeded");
                nextBodies.at(part.entity).parts[part.index].id = part.body->GetID();
            }
        } catch (...) {
            // Restore the exact previous IDs so existing constraints remain valid.
            for (auto &part : pending)
                if (!part.body->GetID().IsInvalid())
                    bi.UnassignBodyID(part.body->GetID());
            for (auto &part : retired) {
                if (!bi.AssignBodyID(part.body, part.id))
                    std::terminate();
                bi.AddBody(part.id, part.active ? JPH::EActivation::Activate
                                                : JPH::EActivation::DontActivate);
            }
            if (!rebuild.empty())
                for (auto &[id, joint] : p->joints)
                    p->system.AddConstraint(joint);
            throw;
        }
        for (auto &part : pending) {
            bi.AddBody(part.body->GetID(),
                       part.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            part.body.release();
        }
        if (!rebuild.empty())
            p->joints.clear();
        for (auto &part : retired)
            bi.DestroyBodyWithoutID(part.body);
        p->bodies.swap(nextBodies);
        p->collisionBodyRebuilds += pending.size();
    }

    p->collisionBodyCount = 0;
    for (const auto &[_, record] : p->bodies)
        p->collisionBodyCount += uint32_t(record.parts.size());
    p->updateCacheBytes();

    for (const auto &[id, record] : p->bodies) {
        const auto &object = w.objects.at(id);
        for (const auto &part : record.parts) {
            auto q = object.transform.rotation;
            if (!object.dynamic || p->prediction)
                bi.SetPositionAndRotation(
                    part.id,
                    JPH::RVec3(object.transform.position.x, object.transform.position.y,
                               object.transform.position.z),
                    JPH::Quat(q.x, q.y, q.z, q.w), JPH::EActivation::DontActivate);
            if (object.dynamic && p->prediction) {
                bi.SetLinearVelocity(part.id, jv(object.velocity));
                bi.SetAngularVelocity(part.id, jv(object.angularVelocity));
            }
        }
    }
    p->publishedRevision = playerCollisionRevision(w);
    std::set<uint64_t> keep;
    auto bodyFor = [&](EntityId id, Vec3 anchor) -> std::optional<JPH::BodyID> {
        auto record = p->bodies.find(id);
        auto object = w.objects.find(id);
        if (record == p->bodies.end() || object == w.objects.end() || record->second.parts.empty())
            return std::nullopt;
        if (record->second.dynamic)
            return record->second.parts.front().id;
        auto q = object->second.transform.rotation;
        Vec3 local = rotate({-q.x, -q.y, -q.z, q.w}, anchor - object->second.transform.position);
        IVec3 cell{int(std::floor(local.x / w.voxelSize)), int(std::floor(local.y / w.voxelSize)),
                   int(std::floor(local.z / w.voxelSize))};
        auto chunk = chunkOf(cell);
        for (const auto &part : record->second.parts)
            if (part.chunk == chunk)
                return part.id;
        // A joint may be placed just outside the occupied cells. Preserve the
        // old permissive behavior by using the nearest chunk in that case.
        float best = std::numeric_limits<float>::max();
        JPH::BodyID selected;
        for (const auto &part : record->second.parts) {
            Vec3 center{(part.chunk.x + .5f) * ChunkSize * w.voxelSize,
                        (part.chunk.y + .5f) * ChunkSize * w.voxelSize,
                        (part.chunk.z + .5f) * ChunkSize * w.voxelSize};
            float distance = dot(center - local, center - local);
            if (distance < best) {
                best = distance;
                selected = part.id;
            }
        }
        return selected.IsInvalid() ? std::nullopt : std::optional<JPH::BodyID>(selected);
    };
    for (auto &j : w.joints) {
        if (p->prediction)
            continue;
        keep.insert(j.id);
        if (p->joints.contains(j.id) || !p->bodies.contains(j.a) || !p->bodies.contains(j.b))
            continue;
        auto &a = w.objects.at(j.a);
        auto &b = w.objects.at(j.b);
        if (!a.dynamic && !b.dynamic)
            continue;
        auto bodyA = bodyFor(j.a, j.anchor), bodyB = bodyFor(j.b, j.anchor);
        if (!bodyA || !bodyB || *bodyA == *bodyB)
            continue;
        JPH::TwoBodyConstraint *constraint = nullptr;
        auto position = JPH::RVec3(j.anchor.x, j.anchor.y, j.anchor.z);
        if (j.hinge) {
            JPH::HingeConstraintSettings s;
            s.mPoint1 = s.mPoint2 = position;
            s.mHingeAxis1 = s.mHingeAxis2 = JPH::Vec3::sAxisY();
            s.mNormalAxis1 = s.mNormalAxis2 = JPH::Vec3::sAxisX();
            constraint = bi.CreateConstraint(&s, *bodyA, *bodyB);
        } else {
            JPH::FixedConstraintSettings s;
            s.mAutoDetectPoint = false;
            s.mPoint1 = s.mPoint2 = position;
            constraint = bi.CreateConstraint(&s, *bodyA, *bodyB);
        }
        if (constraint) {
            p->joints[j.id] = constraint;
            p->system.AddConstraint(constraint);
        }
    }
    for (auto it = p->joints.begin(); it != p->joints.end();)
        if (!keep.contains(it->first)) {
            p->system.RemoveConstraint(it->second);
            it = p->joints.erase(it);
        } else
            ++it;
    publicationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              publicationStart)
                        .count();
    collisionChunkRebuilds = p->collisionChunkRebuilds;
    collisionBodyRebuilds = p->collisionBodyRebuilds;
    collisionBodyCount = p->collisionBodyCount;
    collisionCacheBytes = p->collisionCacheBytes;
    collisionPreparationMs = p->collisionPreparationMs;
    queuedCollisionChunks = p->queuedCollisionChunks;
}
void Physics::setPrediction(bool enabled) {
    p->prediction = enabled;
}
uint64_t Physics::collisionRevision() const {
    return p->publishedRevision;
}
void Physics::forgetPlayer(PlayerId id) {
    p->characters.erase(id);
}
bool Physics::playerClear(Vec3 position) {
    auto character = p->makeCharacter(position);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    character->CheckCollision(
        JPH::RVec3(position.x, position.y, position.z), JPH::Quat::sIdentity(), JPH::Vec3::sZero(),
        0, character->GetShape(), JPH::RVec3(position.x, position.y, position.z), collector,
        p->system.GetDefaultBroadPhaseLayerFilter(1), p->characterLayers, {}, {});
    for (const auto &hit : collector.mHits)
        if (hit.mPenetrationDepth > .005f)
            return false;
    return true;
}
void Physics::restorePlayer(const PlayerState &state) {
    p->characters.erase(state.id);
    auto character = p->makeCharacter(state.position);
    character->SetLinearVelocity(jv(state.velocity));
    character->RefreshContacts(p->system.GetDefaultBroadPhaseLayerFilter(1), p->characterLayers, {},
                               {}, p->temp);
    p->characters[state.id] = std::move(character);
}
void Physics::movePlayer(PlayerState &state, const PlayerInput &input, float dt) {
    if (!p->characters.contains(state.id))
        restorePlayer(state);
    auto character = p->characters.at(state.id);
    state.yaw = input.yaw;
    state.pitch = input.pitch;
    character->RefreshContacts(p->system.GetDefaultBroadPhaseLayerFilter(1), p->characterLayers, {},
                               {}, p->temp);
    character->UpdateGroundVelocity();
    Vec3 forward{std::sin(state.yaw), 0, std::cos(state.yaw)},
        right{std::cos(state.yaw), 0, -std::sin(state.yaw)};
    Vec3 horizontal = (right * input.moveX + forward * input.moveZ) * 4.f;
    auto current = character->GetLinearVelocity();
    auto ground = character->GetGroundVelocity();
    bool grounded = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    JPH::Vec3 velocity(horizontal.x, current.GetY(), horizontal.z);
    if (grounded && current.GetY() - ground.GetY() < .1f) {
        velocity = ground + jv(horizontal);
        if (input.jump)
            velocity += JPH::Vec3(0, 5, 0);
    }
    velocity += JPH::Vec3(0, -9.81f * dt, 0);
    character->SetLinearVelocity(velocity);
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mWalkStairsStepUp = JPH::Vec3(0, .3f, 0);
    settings.mStickToFloorStepDown = JPH::Vec3(0, -.3f, 0);
    if (input.jump)
        settings.mStickToFloorStepDown = JPH::Vec3::sZero();
    character->ExtendedUpdate(dt, JPH::Vec3(0, -9.81f, 0), settings,
                              p->system.GetDefaultBroadPhaseLayerFilter(1), p->characterLayers, {},
                              {}, p->temp);
    state.position = av(JPH::Vec3(character->GetPosition()));
    state.velocity = av(character->GetLinearVelocity());
    state.grounded = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    auto body = character->GetGroundBodyID();
    state.support = body.IsInvalid() ? 0 : p->system.GetBodyInterface().GetUserData(body);
    state.collisionRevision = p->publishedRevision;
}
void Physics::step(World &w, float dt) {
    p->dt = dt;
    ++p->tick;
    p->impacts.clear();
    p->dropped = 0;
    p->forces.clear();
    if (p->system.Update(dt, 1, &p->temp, &p->jobs) != JPH::EPhysicsUpdateError::None)
        ++droppedSimulationSteps;
    forces = std::move(p->forces);
    impacts.clear();
    for (auto &[key, hit] : p->impacts) {
        impacts.push_back(hit);
        p->lastImpact[key] = p->tick;
    }
    std::erase_if(p->lastImpact, [&](auto &entry) { return p->tick - entry.second > 3; });
    droppedImpacts = p->dropped;
    auto &bi = p->system.GetBodyInterface();
    for (auto &[id, record] : p->bodies) {
        auto &o = w.objects.at(id);
        if (!o.dynamic)
            continue;
        if (record.parts.empty())
            continue;
        const auto bodyId = record.parts.front().id;
        auto position = bi.GetPosition(bodyId);
        auto q = bi.GetRotation(bodyId);
        // Jolt's GetPosition is already the body origin, which is the frame
        // Object uses for voxel queries and rendering.
        o.transform.position = av(JPH::Vec3(position));
        o.transform.rotation = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
        o.velocity = av(bi.GetLinearVelocity(bodyId));
        o.angularVelocity = av(bi.GetAngularVelocity(bodyId));
    }
    std::erase_if(w.joints, [&](const Joint &j) {
        auto it = p->joints.find(j.id);
        if (it == p->joints.end())
            return false;
        float load = j.hinge ? static_cast<JPH::HingeConstraint *>(it->second.GetPtr())
                                   ->GetTotalLambdaPosition()
                                   .Length()
                             : static_cast<JPH::FixedConstraint *>(it->second.GetPtr())
                                   ->GetTotalLambdaPosition()
                                   .Length();
        if (load / dt <= j.strength)
            return false;
        p->system.RemoveConstraint(it->second);
        p->joints.erase(it);
        ++w.revision;
        return true;
    });
}
bool Physics::impulse(EntityId id, Vec3 impulse, Vec3 point) {
    auto it = p->bodies.find(id);
    if (it == p->bodies.end() || !it->second.dynamic || it->second.parts.empty())
        return false;
    p->system.GetBodyInterface().AddImpulse(it->second.parts.front().id, jv(impulse),
                                            JPH::RVec3(point.x, point.y, point.z));
    return true;
}
} // namespace aspera
