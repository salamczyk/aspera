#include "aspera/app_cli.hpp"
#include "render_scene.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace aspera;
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
const PtScene &settle(RenderSceneBuilder &b, const World &w) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        auto &s = b.update(w, {}, 0);
        if (b.ready())
            return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error("Mesh jobs did not settle");
}
void cli() {
    AppOptions o;
    auto parse = [&](const char *flag, const char *value) {
        char *a[] = {const_cast<char *>("app"), const_cast<char *>(flag),
                     const_cast<char *>(value)};
        int i = 1;
        parseRenderOption(o, flag, i, value ? 3 : 2, a);
    };
    parse("--renderer", "path-traced");
    parse("--pt-samples", "64");
    parse("--pt-bounces", "12");
    parse("--pt-capture-samples", "257");
    parse("--pt-no-denoise", nullptr);
    require(o.pathTracing && o.ptSamples == 64 && o.ptBounces == 12 && o.ptCaptureSamples == 257 &&
                o.ptNoDenoise,
            "Path tracer options");
    for (auto [f, v] : {std::pair{"--pt-samples", "0"},
                        {"--pt-bounces", "13"},
                        {"--renderer", "bogus"},
                        {"--pt-capture-samples", "-1"}}) {
        bool rejected = false;
        try {
            parse(f, v);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "Invalid PT option accepted");
    }
}
void scene() {
    World w;
    w.materials.resize(3);
    w.materials[1].rgba = 0xff0000ff;
    w.materials[2].emissive = {1, .2f, .1f};
    w.materials[2].emissionIntensity = 100;
    VoxelGrid grid;
    grid.set({31, 0, 0}, 1);
    grid.set({32, 0, 0}, 2);
    EntityId id = 1;
    Object object;
    object.id = id;
    object.name = "boundary";
    object.grid = std::move(grid);
    w.objects.emplace(id, std::move(object));
    RenderSceneBuilder b;
    auto s = settle(b, w);
    require(s.geometry.size() == 2 && s.instances.size() == 2, "Chunk structures");
    auto revision = s.geometryRevision;
    auto key1 = s.geometry[1].key;
    require(!s.emitters.empty() && s.emitters.back().bCdf[3] == 1, "Emissive sampling CDF");
    float pdf = 0;
    for (auto &e : s.emitters)
        pdf += e.radiance[3];
    require(std::abs(pdf - 1) < 1e-5, "Emitter selection PDF normalization");
    w.objects.at(id).transform.position = {2, 3, 4};
    auto moved = b.update(w, {}, 0);
    require(moved.geometryRevision == revision && moved.transformRevision != s.transformRevision,
            "Moving objects rebuilt geometry");
    require(moved.instances[0].model[12] == 2 && moved.instances[0].inverse[12] == -2,
            "Instance transform inversion");
    auto t = moved.transformRevision;
    w.materials[1].rgba = 0x0000ffff;
    auto painted = b.update(w, {}, 0);
    require(painted.geometryRevision == revision && painted.transformRevision == t,
            "Appearance edit rebuilt geometry");
    require(painted.materials[1].baseRoughness[2] == 1, "Material update missing");
    w.objects.at(id).grid.set({31, 0, 0}, 0);
    auto edited = settle(b, w);
    require(edited.geometryRevision != revision && edited.geometry.size() == 1,
            "Boundary edit invalidation");
    require(edited.geometry[0].key != key1, "Neighbor chunk mesh key did not change");
    RenderSceneBuilder deleting;
    deleting.update(w, {}, 0);
    World removed = w;
    removed.objects.clear();
    const auto &deleted = settle(deleting, removed);
    require(deleted.instances.empty(), "Deleted object retained a pending mesh request");
    b.invalidate();
    w.objects.clear();
    auto empty = settle(b, w);
    require(empty.geometry.empty() && empty.instances.empty() && empty.emitters.empty(),
            "Scene reset retained geometry");
    PlayerState player;
    player.position = {1, 0, 0};
    auto avatars = b.update(w, {{1, player}, {2, player}}, 1);
    require(avatars.instances.size() == 1, "Local player avatar included");
}
int main() {
    try {
        cli();
        scene();
        std::cout << "Path-tracing scene and CLI tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
