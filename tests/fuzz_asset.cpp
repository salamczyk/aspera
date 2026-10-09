// The cooked .avox format is read back by the engine and by packaged games, so
// a tampered or truncated asset must be rejected rather than trusted.
#include "aspera/voxel.hpp"
#include "fuzz.hpp"

#include <exception>
#include <fstream>
#include <iterator>

using namespace aspera;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);
    try {
        (void)readAsset(bytes);
    } catch (const std::exception &) {
    }
    return 0;
}

std::vector<std::vector<uint8_t>> asperaFuzzSeeds() {
    // writeAsset only takes a path, so round-trip through a temporary file to
    // produce a seed that is valid by construction rather than hand-rolled.
    ImportedScene scene;
    Material air;
    air.name = "air";
    scene.materials.push_back(air);
    Material stone;
    stone.name = "stone";
    stone.emissive = {0.1f, 0.2f, 0.3f};
    stone.emissionIntensity = 4;
    scene.materials.push_back(stone);
    Material metal;
    metal.name = "metal";
    metal.metallic = 1;
    metal.roughness = 0.2f;
    scene.materials.push_back(metal);

    VoxelAsset asset;
    asset.name = "block";
    asset.transform = {{1.5f, -2.f, 0.25f}, {0, 0, 0, 1}};
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            asset.grid.set({x, y, 0}, MaterialId(1 + (x + y) % 2));
    scene.objects.push_back(std::move(asset));

    auto path = std::filesystem::temp_directory_path() / "aspera-fuzz-seed.avox";
    std::vector<std::vector<uint8_t>> seeds;
    try {
        writeAsset(scene, path);
        std::ifstream in(path, std::ios::binary);
        seeds.push_back({std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
        std::filesystem::remove(path);
    } catch (const std::exception &) {
    }
    return seeds;
}

size_t asperaFuzzPreservedPrefix() {
    return 8; // "ASPA" + version
}

void asperaFuzzRepair(std::vector<uint8_t> &) {}
