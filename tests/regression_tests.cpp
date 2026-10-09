#include "aspera/engine.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aspera;
namespace {
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
VoxelGrid unit(MaterialId material = 1) {
    VoxelGrid g;
    g.set({0, 0, 0}, material);
    return g;
}
bool emitted(const Engine &e, uint64_t command, const std::string &type) {
    for (const auto &event : e.events)
        if (event.command == command && event.type == type)
            return true;
    return false;
}

// A command that cannot be honoured must be rejected as an event, never as an
// exception out of step(): apps/server.cpp treats a throw from step() as fatal.
void prefabBudgetRejectsInsteadOfThrowing() {
    Engine e;
    e.world.objects.clear();
    e.world.nextEntity = 1;
    while (e.world.objects.size() < 4095)
        e.world.spawn("filler", unit(), {}, true);
    Command c;
    c.type = CommandType::SpawnPrefab;
    c.radius = .5f;
    c.material = 1;
    c.point = {0, 20, 0};
    auto accepted = e.submit(c);
    e.step();
    require(e.world.objects.size() == 4096, "prefab at the budget edge must still spawn");
    require(emitted(e, accepted, "committed"), "the last in-budget prefab must commit");
    auto refused = e.submit(c);
    e.step();
    require(e.world.objects.size() == 4096, "a prefab past the budget must not spawn");
    require(emitted(e, refused, "rejected"), "a prefab past the budget must emit rejected");
}

// Anything encodeWorld accepts must survive decodeWorld: saveWorld would
// otherwise write a file that loadWorld refuses, and the replication snapshot
// in src/network.cpp would be rejected by every client.
void encodeRejectsWhatDecodeCannotRead() {
    World w;
    w.materials = {{"air", 0, 0, 1, .7f, 1, 1, 1, 1},
                   {"concrete", 0xc5beb0ff, 1800, 3, .8f, 2000000, 20000000, 2000000, 3000000}};
    auto a = w.spawn("a", unit()), b = w.spawn("b", unit());
    auto roundTrips = [](const World &world) {
        return decodeWorld(encodeWorld(world)).checksum() == world.checksum();
    };
    require(roundTrips(w), "a minimal world must round-trip");

    World joints = w;
    for (int i = 0; i < 8192; ++i)
        joints.joints.push_back({uint64_t(i + 1), a, b, {}, false, 1000.f});
    require(roundTrips(joints), "8192 joints must round-trip");
    joints.joints.push_back({8193, a, b, {}, false, 1000.f});
    requireThrows([&] { return encodeWorld(joints); },
                  "encodeWorld must refuse more joints than decodeWorld accepts");

    World damaged = w;
    damaged.objects.at(a).damage[{0, 0, 0}] = -1.f;
    requireThrows([&] { return encodeWorld(damaged); },
                  "encodeWorld must refuse negative damage that decodeWorld rejects");

    World rotated = w;
    rotated.objects.at(a).transform.rotation = {0, 0, 0, 2};
    requireThrows([&] { return encodeWorld(rotated); },
                  "encodeWorld must refuse an unnormalized rotation that decodeWorld rejects");

    World properties = w;
    for (int i = 0; i < 4097; ++i)
        properties.properties["k" + std::to_string(i)] = double(i);
    requireThrows([&] { return encodeWorld(properties); },
                  "encodeWorld must refuse more properties than decodeWorld accepts");
}

// The same asymmetry at voxel scale: 4,000,001 cells encode to ~54 MiB, well
// inside the 128 MiB byte budget, but exceed decodeWorld's voxel budget.
void encodeRejectsUnreadableVoxelCounts() {
    World w;
    w.materials = {{"air", 0, 0, 1, .7f, 1, 1, 1, 1},
                   {"concrete", 0xc5beb0ff, 1800, 3, .8f, 2000000, 20000000, 2000000, 3000000}};
    VoxelGrid g;
    long long target = 4000001, n = 0;
    for (int cz = 0; cz < 16 && n < target; ++cz)
        for (int cy = 0; cy < 16 && n < target; ++cy)
            for (int cx = 0; cx < 16 && n < target; ++cx)
                for (int z = 0; z < 32 && n < target; ++z)
                    for (int y = 0; y < 32 && n < target; ++y)
                        for (int x = 0; x < 32 && n < target; ++x, ++n)
                            g.set({cx * 32 + x, cy * 32 + y, cz * 32 + z}, 1);
    require(g.count() == 4000001, "the fixture must exceed the snapshot voxel budget by one");
    w.spawn("big", std::move(g));
    requireThrows([&] { return encodeWorld(w); },
                  "encodeWorld must refuse more voxels than decodeWorld accepts");
}

// Cancelling a landscape request must not join its worker on the main thread.
void cancellingLandscapeDoesNotBlock() {
    Engine e;
    e.simulating = false;
    LandscapeSettings s;
    s.minimum = {-64, -32, -64};
    s.maximum = {64, 64, 64};
    auto command = e.requestLandscape(s);
    require(command != 0, "requestLandscape must return a command id");
    require(!emitted(e, command, "rejected"), "the landscape request must be admitted");
    auto start = std::chrono::steady_clock::now();
    e.resetPhysics();
    auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    require(elapsed < 50, "resetPhysics must retire a pending landscape without joining it");
    // The retired worker still has to be reaped, and publishing must not resume.
    for (int i = 0; i < 240; ++i)
        e.step();
    require(e.world.objects.empty(), "a cancelled landscape must never publish");
}

// error() accepts any Lua value; lua_tostring returns null for a non-string one
// and that null used to reach std::string, segfaulting the process.
void nonStringScriptErrorIsReported() {
    auto project = std::filesystem::temp_directory_path() / "aspera-regression-script";
    for (const char *body : {"error(nil)", "error({code = 7})", "error(setmetatable({}, {}))"}) {
        std::filesystem::remove_all(project);
        std::filesystem::create_directories(project);
        std::ofstream(project / "project.lua") << "return { name = 'regression' }\n";
        std::ofstream(project / "scene.lua") << "\n";
        std::ofstream(project / "server.lua") << "function update(dt)\n  " << body << "\nend\n";
        Engine e;
        e.loadProject(project);
        e.step();
        bool reported = false;
        for (const auto &event : e.events)
            reported |= event.type == "script_error" && !event.message.empty();
        require(reported, "a non-string script error must be reported, not crash");
    }
    // The same value thrown at load time must surface as an exception, not a crash.
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);
    std::ofstream(project / "project.lua") << "return { name = 'regression' }\n";
    std::ofstream(project / "scene.lua") << "error({})\n";
    std::ofstream(project / "server.lua") << "\n";
    Engine e;
    requireThrows([&] { e.loadProject(project); },
                  "a non-string load-time script error must throw, not crash");
    std::filesystem::remove_all(project);
}

// A group that lists the same child twice is a DAG, not a cycle: the depth guard
// bounds depth, not breadth, so fan-out used to grow as 2^depth.
void voxSceneGraphFanOutIsBounded() {
    auto path = std::filesystem::temp_directory_path() / "aspera-regression-fanout.vox";
    auto i32 = [](std::vector<uint8_t> &out, int32_t v) {
        for (int k = 0; k < 4; ++k)
            out.push_back(uint8_t(uint32_t(v) >> (k * 8)));
    };
    std::vector<uint8_t> body;
    auto chunk = [&](const char *id, const std::vector<uint8_t> &content) {
        for (int k = 0; k < 4; ++k)
            body.push_back(uint8_t(id[k]));
        i32(body, int32_t(content.size()));
        i32(body, 0);
        body.insert(body.end(), content.begin(), content.end());
    };
    std::vector<uint8_t> size;
    i32(size, 1);
    i32(size, 1);
    i32(size, 1);
    chunk("SIZE", size);
    std::vector<uint8_t> xyzi;
    i32(xyzi, 1);
    for (uint8_t v : {0, 0, 0, 1})
        xyzi.push_back(v);
    chunk("XYZI", xyzi);
    const int depth = 30;
    for (int n = 0; n < depth; ++n) {
        std::vector<uint8_t> group;
        i32(group, n);
        i32(group, 0); // empty attribute dictionary
        i32(group, 2); // the same child twice
        i32(group, n + 1);
        i32(group, n + 1);
        chunk("nGRP", group);
    }
    std::vector<uint8_t> leaf;
    i32(leaf, depth);
    i32(leaf, 0);
    i32(leaf, 0);
    chunk("nGRP", leaf);

    std::vector<uint8_t> file;
    for (char c : std::string("VOX "))
        file.push_back(uint8_t(c));
    i32(file, 150);
    for (char c : std::string("MAIN"))
        file.push_back(uint8_t(c));
    i32(file, 0);
    i32(file, int32_t(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));

    auto start = std::chrono::steady_clock::now();
    requireThrows([&] { return importVox(path); }, "an exponential scene graph must be rejected");
    auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    require(ms < 5000, "rejecting an exponential scene graph must not hang the importer");
    std::filesystem::remove(path);
}
} // namespace

int main() {
    prefabBudgetRejectsInsteadOfThrowing();
    encodeRejectsWhatDecodeCannotRead();
    encodeRejectsUnreadableVoxelCounts();
    cancellingLandscapeDoesNotBlock();
    nonStringScriptErrorIsReported();
    voxSceneGraphFanOutIsBounded();
}
