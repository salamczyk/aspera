#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aspera {
struct IVec3 {
    int x{}, y{}, z{};
    auto operator<=>(const IVec3 &) const = default;
};
inline IVec3 operator+(IVec3 a, IVec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline IVec3 operator-(IVec3 a, IVec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
struct Vec3 {
    float x{}, y{}, z{};
    auto operator<=>(const Vec3 &) const = default;
};
inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline Vec3 operator/(Vec3 a, float s) {
    return a * (1.f / s);
}
inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) {
    return std::sqrt(dot(a, a));
}
inline Vec3 normalized(Vec3 a) {
    auto l = length(a);
    return l > 0 ? a / l : Vec3{};
}
struct Quat {
    float x{}, y{}, z{}, w{1};
};
Vec3 rotate(Quat q, Vec3 v);
struct Transform {
    Vec3 position{};
    Quat rotation{};
};
inline float defaultImpactToughness(const std::string &name) {
    if (name == "steel")
        return 5000000;
    if (name == "concrete")
        return 800000;
    return 100000;
}
struct Material {
    std::string name{"stone"};
    uint32_t rgba{0xaaaaaaff};
    float density{1800}, resistance{1}, friction{0.7f};
    float tension{200000}, compression{2000000}, shear{300000}, bending{200000};
    float impactToughness{100000};
    float roughness{.85f}, metallic{};
    Vec3 emissive{};
    float emissionIntensity{};
};
using MaterialId = uint16_t;
using EntityId = uint64_t;
inline constexpr int ChunkSize = 32;
inline constexpr std::array<IVec3, 6> Neighbors{
    {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
struct Chunk {
    std::array<MaterialId, 32768> cells{};
    uint32_t occupied{};
    uint64_t contentKey{};
    std::array<uint64_t, 6> faceKeys{};
};
IVec3 chunkOf(IVec3 p);
IVec3 localOf(IVec3 p);
class VoxelGrid {
  public:
    MaterialId get(IVec3 p) const;
    void set(IVec3 p, MaterialId material);
    size_t count() const;
    std::vector<std::pair<IVec3, MaterialId>> occupied() const;
    const std::map<IVec3, Chunk> &chunks() const {
        return chunks_;
    }
    const std::set<IVec3> &dirty() const {
        return dirty_;
    }
    void clearDirty() {
        dirty_.clear();
    }
    uint64_t checksum() const;
    uint64_t chunkContentKey(IVec3 coordinate) const;
    uint64_t chunkMeshKey(IVec3 coordinate) const;
    // Order-independent cache key, updated only for cells whose material actually changes.
    // This is separate from the serialized checksum and does not change any file format.
    uint64_t contentKey() const {
        return contentKey_;
    }

  private:
    // Fragment staging can copy intact, already validated chunks without rehashing cells.
    friend class Structure;
    std::map<IVec3, Chunk> chunks_;
    std::set<IVec3> dirty_;
    uint64_t contentKey_{};
};
struct Vertex {
    float x, y, z, nx, ny, nz;
    uint32_t rgba;
    float material;
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};
Mesh meshChunk(const VoxelGrid &, IVec3 coordinate, float voxelSize, const std::vector<Material> &);
Mesh meshGrid(const VoxelGrid &, float voxelSize, const std::vector<Material> &);
struct CollisionBox {
    Vec3 center, halfExtent;
};
std::vector<CollisionBox> collisionBoxesChunk(const VoxelGrid &, IVec3 coordinate, float voxelSize);
std::vector<CollisionBox> collisionBoxes(const VoxelGrid &, float voxelSize);
struct RayHit {
    IVec3 cell, normal;
    float distance{};
    MaterialId material{};
};
std::optional<RayHit> raycast(const VoxelGrid &, float voxelSize, Vec3 origin, Vec3 direction,
                              float maxDistance);
struct VoxelAsset {
    std::string name;
    VoxelGrid grid;
    Transform transform;
};
struct ImportedScene {
    std::vector<Material> materials;
    std::vector<VoxelAsset> objects;
};
ImportedScene importVox(const std::filesystem::path &, float voxelSize = 0.1f);
void writeAsset(const ImportedScene &, const std::filesystem::path &);
ImportedScene readAsset(const std::filesystem::path &);
} // namespace aspera
