// decodeWorld parses save files and replicated snapshots. Both are untrusted:
// a save can be hand-edited and a snapshot arrives over the network.
#include "aspera/engine.hpp"
#include "fuzz.hpp"

#include <exception>

using namespace aspera;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);
    try {
        auto world = decodeWorld(bytes);
        // Anything that decodes must re-encode and decode again: encodeWorld is
        // the writer that produces snapshots, so a decodable-but-unencodable
        // world is exactly the asymmetry that made saves unloadable.
        auto again = encodeWorld(world);
        auto round = decodeWorld(again);
        if (round.checksum() != world.checksum())
            __builtin_trap();
    } catch (const std::exception &) {
        // Rejecting malformed input is the contract.
    }
    return 0;
}

std::vector<std::vector<uint8_t>> asperaFuzzSeeds() {
    std::vector<std::vector<uint8_t>> seeds;
    // An empty world, exercising the material/environment/light sections.
    Engine engine;
    seeds.push_back(encodeWorld(engine.world));
    // A world with geometry, anchors, damage, seams, a joint and a light.
    World world = engine.world;
    VoxelGrid a, b;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 2; ++z)
                a.set({x, y, z}, MaterialId(1 + (x + y + z) % 3));
    b.set({0, 0, 0}, 2);
    b.set({0, 1, 0}, 3);
    auto first = world.spawn("solid", std::move(a), {{1, 2, 3}, {}});
    auto second = world.spawn("pair", std::move(b), {}, true);
    auto &object = world.objects.at(first);
    object.anchors.insert({0, 0, 0});
    object.damage[{1, 1, 1}] = 0.5f;
    object.seams.insert(Seam{{0, 0, 0}, {1, 0, 0}});
    object.stress = 0.25f;
    world.joints.push_back({1, first, second, {0, 1, 0}, true, 5000.f});
    world.properties["game.state"] = 2;
    Light light;
    light.id = world.nextLight++;
    light.type = LightType::Spot;
    world.lights.emplace(light.id, light);
    seeds.push_back(encodeWorld(world));
    return seeds;
}

size_t asperaFuzzPreservedPrefix() {
    return 8; // magic + version
}

void asperaFuzzRepair(std::vector<uint8_t> &) {}
