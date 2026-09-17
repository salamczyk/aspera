#include "aspera/camera.hpp"
#include "aspera/voxel.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace aspera;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void le32(std::vector<uint8_t> &b, uint32_t n) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(n >> (8 * i)));
}
void le16(std::vector<uint8_t> &b, uint16_t n) {
    b.push_back(static_cast<uint8_t>(n));
    b.push_back(static_cast<uint8_t>(n >> 8));
}
void f32(std::vector<uint8_t> &b, float n) {
    le32(b, std::bit_cast<uint32_t>(n));
}
void string32(std::vector<uint8_t> &b, const char *s) {
    auto size = std::strlen(s);
    le32(b, static_cast<uint32_t>(size));
    b.insert(b.end(), s, s + size);
}
void chunk(std::vector<uint8_t> &b, const char *id, const std::vector<uint8_t> &contents) {
    b.insert(b.end(), id, id + 4);
    le32(b, contents.size());
    le32(b, 0);
    b.insert(b.end(), contents.begin(), contents.end());
}
std::filesystem::path temporary(const char *name) {
    return std::filesystem::temp_directory_path() / name;
}

void gridAndMath() {
    VoxelGrid g;
    g.set({-1, -33, 32}, 7);
    require(g.get({-1, -33, 32}) == 7, "negative chunk lookup");
    require(g.chunks().contains({-1, -2, 1}), "negative chunk key");
    require(g.dirty().contains({-1, -2, 0}), "boundary neighbor dirtied");
    uint64_t hash = g.checksum();
    VoxelGrid same;
    same.set({-1, -33, 32}, 7);
    require(hash == same.checksum(), "checksum stable");
    auto key = g.contentKey();
    require(key == same.contentKey() && key != 0, "cache key stable across reconstructed grids");
    same.set({-1, -33, 32}, 7);
    require(key == same.contentKey(), "unchanged assignment retains cache key");
    same.set({-1, -33, 32}, 2);
    require(key != same.contentKey(), "same-size material replacement changes cache key");
    same.set({-1, -33, 32}, 7);
    require(key == same.contentKey(), "undo restores content cache key");
    same.set({9, 2, -40}, 1);
    VoxelGrid reordered;
    reordered.set({9, 2, -40}, 1);
    reordered.set({-1, -33, 32}, 7);
    require(same.contentKey() == reordered.contentKey(),
            "cache key is insertion-order independent");
    auto copied = same;
    copied.set({9, 2, -40}, 0);
    require(copied.contentKey() == key && same.contentKey() != key,
            "copies have independent cache keys");
    copied.set({-1, -33, 32}, 0);
    require(copied.contentKey() == 0, "empty grid restores empty cache key");
    auto r = rotate({0, 0, std::sqrt(.5f), std::sqrt(.5f)}, {1, 0, 0});
    require(std::abs(r.x) < 1e-4f && std::abs(r.y - 1) < 1e-4f, "quaternion rotation");
}
void geometry() {
    VoxelGrid g;
    g.set({0, 0, 0}, 1);
    g.set({1, 0, 0}, 1);
    std::vector<Material> m(2);
    m[1].rgba = 0x11223344;
    auto mesh = meshGrid(g, 1, m);
    require(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "greedy two voxel prism");
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]],
             &c = mesh.vertices[mesh.indices[i + 2]];
        Vec3 n{a.nx, a.ny, a.nz};
        require(
            dot(cross({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z}), n) > 0,
            "outward mesh winding");
        require(a.rgba == 0x11223344, "rgba retained");
        require(a.material == 1.f, "material id retained for PBR lookup");
    }
    auto boxes = collisionBoxes(g, 1);
    require(boxes.size() == 1 && std::abs(boxes[0].halfExtent.x - 1.f) < 1e-5f,
            "merged collision box");
    auto hit = raycast(g, 1, {-1, .5f, .5f}, {1, 0, 0}, 10);
    require(hit && hit->cell == IVec3{0, 0, 0} && hit->normal == IVec3{-1, 0, 0} &&
                std::abs(hit->distance - 1) < 1e-5f,
            "DDA hit");
    require(!raycast(g, 1, {-1, 0, 0}, {0, 0, 0}, 10), "zero ray misses");
    VoxelGrid boundary;
    boundary.set({31, 0, 0}, 1);
    boundary.set({32, 0, 0}, 1);
    auto across = meshGrid(boundary, 1, m);
    float area = 0;
    for (size_t i = 0; i < across.indices.size(); i += 3) {
        auto a = across.vertices[across.indices[i]], b = across.vertices[across.indices[i + 1]],
             c = across.vertices[across.indices[i + 2]];
        area +=
            length(cross({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z})) *
            .5f;
    }
    require(std::abs(area - 10) < 1e-5f, "chunk boundary hides internal faces");
    VoxelGrid sparse;
    sparse.set({-131072, 0, 0}, 1);
    sparse.set({131072, 0, 0}, 1);
    require(meshGrid(sparse, 1, m).indices.size() == 72,
            "sparse meshing does not scan huge empty bounds");
}
void pointerPicking() {
    const Vec3 forward{0, 0, 1};
    const auto center = perspectivePickRay(forward, 400, 300, 800, 600);
    require(length(center - forward) < 1e-5f, "center ray follows camera");
    VoxelGrid targets;
    targets.set({-3, 0, 5}, 1);
    targets.set({2, 0, 5}, 2);
    // Project each target's center to logical window coordinates, then pick it.
    for (int x : {-3, 2}) {
        const float screenX = 400 + (x + .5f) / 5.5f * 300 / .577350269f;
        const float screenY = 300 - .5f / 5.5f * 300 / .577350269f;
        auto ray = perspectivePickRay(forward, screenX, screenY, 800, 600);
        auto hit = raycast(targets, 1, {}, ray, 100);
        require(hit && hit->cell == IVec3{x, 0, 5}, "off-center pointer picks target voxel");
        require(length(ray - perspectivePickRay(forward, screenX * 2, screenY * 2, 1600, 1200)) <
                    1e-5f,
                "picking remains aligned after viewport scaling");
    }
    auto down = perspectivePickRay(normalized({-5, -8, 7}), 400, 100, 800, 600);
    require(down.y > normalized(Vec3{-5, -8, 7}).y, "top-down pointer ray uses camera basis");
}
void assetRoundTrip() {
    ImportedScene a;
    a.materials.resize(2);
    a.materials[1].name = "red";
    a.materials[1].rgba = 0xff0000ff;
    a.materials[1].impactToughness = 123456.f;
    a.materials[1].roughness = .2f;
    a.materials[1].metallic = .8f;
    a.materials[1].emissive = {.1f, .2f, .3f};
    a.materials[1].emissionIntensity = 250.f;
    a.objects.push_back({"thing", {}, {{2, 3, 4}, {0, 0, 0, 1}}});
    a.objects[0].grid.set({-1, 4, 2}, 1);
    auto p = temporary("aspera-voxel.asset");
    writeAsset(a, p);
    std::ifstream encodedFile(p, std::ios::binary);
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(encodedFile)), {});
    auto b = readAsset(p);
    std::filesystem::remove(p);
    require(b.materials.size() == 2 && b.objects.size() == 1 &&
                b.objects[0].grid.get({-1, 4, 2}) == 1 &&
                std::abs(b.materials[1].impactToughness - 123456.f) < 1e-3f,
            "asset roundtrip");
    require(std::abs(b.materials[1].roughness - .2f) < 1e-5f &&
                std::abs(b.materials[1].metallic - .8f) < 1e-5f &&
                std::abs(b.materials[1].emissive.z - .3f) < 1e-5f &&
                std::abs(b.materials[1].emissionIntensity - 250.f) < 1e-5f,
            "asset retains PBR material settings");

    auto read32 = [&](size_t at) {
        return uint32_t(encoded[at]) | uint32_t(encoded[at + 1]) << 8 |
               uint32_t(encoded[at + 2]) << 16 | uint32_t(encoded[at + 3]) << 24;
    };
    size_t at = 12;
    for (size_t i = 0; i < a.materials.size(); ++i) {
        at += 4 + read32(at) + 4 + 8 * 4;
        encoded.erase(encoded.begin() + at, encoded.begin() + at + 6 * 4);
    }
    encoded[4] = 2;
    p = temporary("aspera-voxel-v2.asset");
    std::ofstream v2out(p, std::ios::binary);
    v2out.write(reinterpret_cast<const char *>(encoded.data()), encoded.size());
    v2out.close();
    auto v2 = readAsset(p);
    std::filesystem::remove(p);
    require(v2.materials[1].roughness == .85f && v2.materials[1].metallic == 0 &&
                v2.materials[1].emissionIntensity == 0,
            "v2 assets receive PBR defaults");
}
void legacyAssetCompatibility() {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), {'A', 'S', 'P', 'A'});
    le32(bytes, 1);
    le32(bytes, 2);
    auto material = [&](const char *name, uint32_t rgba) {
        string32(bytes, name);
        le32(bytes, rgba);
        for (float f : {1800.f, 1.f, .7f, 200000.f, 2000000.f, 300000.f, 200000.f})
            f32(bytes, f);
    };
    material("air", 0);
    material("timber", 0xc08040ff);
    le32(bytes, 1);
    string32(bytes, "legacy");
    for (float f : {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f})
        f32(bytes, f);
    le32(bytes, 1);
    le32(bytes, 0);
    le32(bytes, 0);
    le32(bytes, 0);
    le16(bytes, 1);
    auto p = temporary("aspera-voxel-v1.asset");
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    out.close();
    auto scene = readAsset(p);
    std::filesystem::remove(p);
    require(scene.materials.size() == 2 && scene.objects.size() == 1 &&
                scene.objects[0].grid.get({0, 0, 0}) == 1 &&
                std::abs(scene.materials[1].impactToughness - 100000.f) < 1e-3f,
            "v1 asset receives timber impact toughness default");
}
void voxImport() {
    std::vector<uint8_t> kids, size, xyzi, rgba;
    le32(size, 2);
    le32(size, 1);
    le32(size, 1);
    le32(xyzi, 2);
    xyzi.insert(xyzi.end(), {0, 0, 0, 1, 1, 0, 0, 2});
    for (int i = 0; i < 256; ++i)
        rgba.insert(rgba.end(), {static_cast<uint8_t>(i), 0, 0, 255});
    chunk(kids, "SIZE", size);
    chunk(kids, "XYZI", xyzi);
    chunk(kids, "RGBA", rgba);
    std::vector<uint8_t> f;
    f.insert(f.end(), {'V', 'O', 'X', ' '});
    le32(f, 150);
    f.insert(f.end(), {'M', 'A', 'I', 'N'});
    le32(f, 0);
    le32(f, kids.size());
    f.insert(f.end(), kids.begin(), kids.end());
    auto p = temporary("aspera-simple.vox");
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(f.data()), f.size());
    out.close();
    auto scene = importVox(p, .5f);
    std::filesystem::remove(p);
    require(scene.objects.size() == 1 && scene.objects[0].grid.count() == 2 &&
                scene.materials[1].rgba == 0x000000ff,
            "VOX import/palette");
    p = temporary("aspera-bad.vox");
    std::ofstream bad(p, std::ios::binary);
    bad.write("VOX ", 4);
    bad.close();
    bool threw = false;
    try {
        (void)importVox(p);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    std::filesystem::remove(p);
    require(threw, "truncated VOX rejected");
}
} // namespace
int main() {
    gridAndMath();
    geometry();
    pointerPicking();
    assetRoundTrip();
    legacyAssetCompatibility();
    voxImport();
}
