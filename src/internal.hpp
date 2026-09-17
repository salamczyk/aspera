#pragma once
#include "aspera/engine.hpp"
#include <unordered_map>

namespace aspera {
struct Impact {
    EntityId a{}, b{};
    Vec3 localA{}, localB{}, point{}, normal{};
    float closing{}, impulse{}, energy{};
};
class Physics {
  public:
    Physics();
    ~Physics();
    void clear();
    // Seeds worker-prepared collision boxes into the main-thread cache. This
    // only updates cache data; Jolt bodies are still published by sync().
    void seedCollisionCache(EntityId, const VoxelGrid &, float,
                            const std::map<IVec3, std::vector<CollisionBox>> &);
    void sync(World &);
    void step(World &, float);
    void impulse(EntityId, Vec3, Vec3);
    std::map<EntityId, std::map<IVec3, Vec3>> forces;
    std::vector<Impact> impacts;
    uint32_t droppedImpacts{};
    // Collision preparation diagnostics. These are cumulative for the current
    // physics lifetime except for the current cache/body counts and timing.
    uint64_t collisionChunkRebuilds{};
    uint64_t collisionBodyRebuilds{};
    uint32_t collisionBodyCount{};
    size_t collisionCacheBytes{};
    double collisionPreparationMs{};
    uint32_t queuedCollisionChunks{};
    double publicationMs{};
    std::map<IVec3, std::vector<CollisionBox>>
    prepareFragmentCollisions(EntityId source, const VoxelGrid &, float) const;
    struct CollisionSeed {
        EntityId id;
        const VoxelGrid *grid;
        const std::map<IVec3, std::vector<CollisionBox>> *boxes;
    };
    // All copying/validation precedes a no-allocation cache swap.
    void seedFragmentCollisionCaches(const std::vector<CollisionSeed> &, float);

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
struct StructuralNode {
    std::vector<IVec3> cells;
    Vec3 center;
    float mass{};
    MaterialId material{};
    bool anchored{};
    Vec3 displacement{}, rotation{};
    Vec3 minimum{}, maximum{};
};
struct StructuralBond {
    size_t a{}, b{};
    Vec3 center, normal;
    float area{}, damage{};
    std::vector<Seam> faces;
};
struct StructuralRegion {
    std::array<MaterialId, 512> cells{};
    std::array<uint16_t, 512> labels{};
    std::vector<StructuralNode> nodes;
};
struct StructuralFace {
    Seam face;
    uint16_t a{}, b{};
    uint8_t axis{};
    bool boundary{};
};
struct StructureGraph {
    uint64_t revision{};
    uint64_t geometryKey{};
    std::set<IVec3> anchors;
    std::set<Seam> seams;
    std::vector<StructuralNode> nodes;
    std::vector<StructuralBond> bonds;
    std::map<IVec3, std::shared_ptr<const StructuralRegion>> regions;
    std::map<IVec3, std::shared_ptr<const std::vector<StructuralFace>>> contacts;
    std::map<IVec3, std::vector<size_t>> regionNodes;
    std::map<IVec3, uint64_t> chunkKeys;
    std::vector<std::vector<std::pair<size_t, size_t>>> adjacency;
    std::vector<int> distance;
    std::vector<size_t> order;
    std::vector<Vec3> load, moment;
    float voxelSize{};
    bool free{}, connectivityChecked{};
    std::optional<size_t> nodeAt(IVec3) const;
};
class Structure {
  public:
    std::map<EntityId, StructureGraph> graphs;
    void update(Engine &, float, const std::map<EntityId, std::map<IVec3, Vec3>> &forces);
    // Used by generation/import preflight before an object is published. The
    // implementation lives with the structural solver.
    void validate(const Object &, const World &) const;
    StructureGraph prepare(const Object &, const World &, const StructureGraph *previous = nullptr,
                           Stats *stats = nullptr, bool componentPreflight = false) const;
    void seed(EntityId id, StructureGraph graph) {
        graphs[id] = std::move(graph);
    }
    void materialsChanged(const World &);
    // Called only during successful fragment staging; Engine supplies its collision cache.
    Physics *physics{};
    void clear() {
        graphs.clear();
    }

  private:
    void split(Engine &, EntityId);
};
class Scripts {
  public:
    Scripts();
    ~Scripts();
    void load(Engine &, const std::filesystem::path &, bool initialize);
    void tick(Engine &, float);
    void input(const std::string &, float);
    std::string name;
    std::filesystem::path project;

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
inline Seam seam(IVec3 a, IVec3 b) {
    return a < b ? Seam{a, b} : Seam{b, a};
}
inline Vec3 cellCenter(IVec3 p, float s) {
    return {(p.x + .5f) * s, (p.y + .5f) * s, (p.z + .5f) * s};
}
} // namespace aspera
