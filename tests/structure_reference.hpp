#pragma once

// Frozen copy of the pre-region-cache structural solver's graph model.  Keep
// this independent of src/internal.hpp so it remains a differential oracle
// while the production representation evolves.
#include "aspera/engine.hpp"

#include <map>
#include <set>
#include <vector>

namespace aspera::test {
struct ReferenceStructuralNode {
    std::vector<IVec3> cells;
    Vec3 center;
    float mass{};
    MaterialId material{};
    bool anchored{};
    Vec3 displacement{}, rotation{};
};
struct ReferenceStructuralBond {
    size_t a{}, b{};
    Vec3 center, normal;
    float area{}, damage{};
    std::vector<Seam> faces;
};
struct ReferenceStructureGraph {
    uint64_t revision{};
    uint64_t geometryKey{};
    std::set<IVec3> anchors;
    std::set<Seam> seams;
    std::vector<ReferenceStructuralNode> nodes;
    std::vector<ReferenceStructuralBond> bonds;
    std::map<IVec3, size_t> lookup;
};

class ReferenceStructure {
  public:
    std::map<EntityId, ReferenceStructureGraph> graphs;
    void update(Engine &, float, const std::map<EntityId, std::map<IVec3, Vec3>> &forces);
    void validate(const Object &, const World &) const;
    void clear() {
        graphs.clear();
    }

  private:
    void split(Engine &, EntityId);
};
} // namespace aspera::test
