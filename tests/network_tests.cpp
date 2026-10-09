#include "aspera/engine.hpp"

#include "udp_proxy.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aspera;
namespace {
constexpr uint16_t Port = 39763;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void runCase(const char *name, F &&caseFn) {
    try {
        caseFn();
    } catch (const std::exception &ex) {
        std::cerr << "network test failed in " << name << ": " << ex.what() << "\n";
        throw;
    }
}
void pollAll(Network &host, Engine &authority, std::vector<Network *> &clients,
             std::vector<Engine *> &replicas) {
    host.poll(authority);
    for (size_t i = 0; i < clients.size(); ++i)
        clients[i]->poll(*replicas[i]);
}
template <class Predicate>
bool waitFor(Network &host, Engine &authority, std::vector<Network *> &clients,
             std::vector<Engine *> &replicas, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        pollAll(host, authority, clients, replicas);
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
template <class Predicate>
bool waitWithFixedTicks(Network &host, Engine &authority, std::vector<Network *> &clients,
                        std::vector<Engine *> &replicas, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        pollAll(host, authority, clients, replicas);
        authority.step(1.f / 60.f);
        host.broadcast(authority);
        pollAll(host, authority, clients, replicas);
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
VoxelGrid oneVoxel() {
    VoxelGrid g;
    g.set({0, 0, 0}, 1);
    g.clearDirty();
    return g;
}

std::filesystem::path makePlayerProject() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("aspera-network-players-" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    std::ofstream(path / "project.lua")
        << "return { name = 'Network Players', player = { mode = 'first_person', "
           "build_material = 1, spawns = { { x = 0, y = 2, z = 0, yaw = 0, pitch = 0 }, "
           "{ x = 3, y = 2, z = 0, yaw = 0, pitch = 0 } }, "
           "bounds = { min = { x = -20, y = -10, z = -20 }, "
           "max = { x = 20, y = 20, z = 20 } } } }\n";
    // Keep the fixture grounded so prediction and the soak exercise actual
    // walking/tool behavior instead of an unbounded free fall.
    std::ofstream(path / "scene.lua") << "engine.spawn_box('floor',-6,-.2,-6,120,2,120,1,true)\n";
    std::ofstream(path / "server.lua") << "\n";
    return path;
}

EntityId spawnSoakWall(Engine &engine) {
    VoxelGrid wall;
    for (int x = -40; x <= 40; ++x)
        for (int y = 0; y < 25; ++y)
            for (int z = 30; z < 33; ++z)
                wall.set({x, y, z}, 1);
    auto id = engine.world.spawn("soak wall", std::move(wall));
    for (int x = -40; x <= 40; ++x)
        for (int z = 30; z < 33; ++z)
            engine.world.objects.at(id).anchors.insert({x, 0, z});
    return id;
}

void loopbackHandshakeAndBroadcast() {
    Engine authority, a, b;
    // The default project is intentionally used by every peer: handshake hashes
    // must agree before a peer becomes connected.
    EntityId cube = authority.world.spawn("network cube", oneVoxel());
    authority.world.objects.at(cube).anchors.insert({0, 0, 0});
    Network host, first, second;
    host.host(Port);
    first.connect("127.0.0.1", Port);
    second.connect("127.0.0.1", Port);
    std::vector<Network *> clients{&first, &second};
    std::vector<Engine *> replicas{&a, &b};
    require(waitFor(host, authority, clients, replicas,
                    [&] { return host.peers() == 2 && first.connected() && second.connected(); }),
            "two loopback clients complete handshake");
    host.broadcast(authority);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return a.world.checksum() == authority.world.checksum() &&
                               b.world.checksum() == authority.world.checksum();
                    }),
            "broadcast replicates exact world snapshot");
    require(first.isClient() && second.isClient() && host.status().size() > 0,
            "network roles and status available");
    auto environment = authority.world.environment;
    environment.skyIntensity = 12345;
    authority.setEnvironment(environment);
    auto light = Light{};
    light.type = LightType::Spot;
    light.position = {2, 3, 4};
    auto lightId = authority.addLight(light);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return a.world.lights.contains(lightId) &&
                                          b.world.lights.contains(lightId) &&
                                          a.world.environment.skyIntensity == 12345;
                               }),
            "lighting-only state changes replicate");

    Command edit;
    edit.type = CommandType::SetVoxel;
    edit.entity = cube;
    edit.cell = {1, 0, 0};
    edit.material = 1;
    edit.expectedRevision = a.world.objects.at(cube).revision;
    first.sendCommand(edit);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return authority.world.objects.at(cube).grid.get({1, 0, 0}) ==
                                              1 &&
                                          a.world.checksum() == authority.world.checksum() &&
                                          b.world.checksum() == authority.world.checksum();
                               }),
            "client edit transaction commits and replicates");

    Command damage;
    damage.type = CommandType::SphereDamage;
    damage.entity = cube;
    damage.point = {.05f, .05f, .05f};
    damage.radius = .09f;
    damage.strength = 1;
    damage.expectedRevision = b.world.objects.at(cube).revision;
    second.sendCommand(damage);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto it = authority.world.objects.find(cube);
                                   return it != authority.world.objects.end() &&
                                          !it->second.damage.empty() &&
                                          a.world.checksum() == authority.world.checksum() &&
                                          b.world.checksum() == authority.world.checksum();
                               }),
            "damage transaction and damage map replicate");

    Engine late;
    Network latePeer;
    latePeer.connect("127.0.0.1", Port);
    clients.push_back(&latePeer);
    replicas.push_back(&late);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return host.peers() == 3 && latePeer.connected() &&
                               late.world.checksum() == authority.world.checksum();
                    }),
            "late peer receives current damaged scene");
    require(!late.world.objects.at(cube).damage.empty(), "late join preserves accumulated damage");
    require(late.world.lights.contains(lightId) &&
                late.world.nextLight == authority.world.nextLight,
            "late join preserves lights and allocator");
    authority.removeLight(lightId);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return !a.world.lights.contains(lightId) &&
                                          !b.world.lights.contains(lightId) &&
                                          !late.world.lights.contains(lightId);
                               }),
            "light removals replicate");
}

void hostPeerBudgetIncludesHost() {
    Engine authority, e1, e2, e3, e4;
    Network host, n1, n2, n3, n4;
    host.host(Port);
    n1.connect("127.0.0.1", Port);
    n2.connect("127.0.0.1", Port);
    n3.connect("127.0.0.1", Port);
    n4.connect("127.0.0.1", Port);
    std::vector<Network *> clients{&n1, &n2, &n3, &n4};
    std::vector<Engine *> engines{&e1, &e2, &e3, &e4};
    require(waitFor(host, authority, clients, engines, [&] { return host.peers() >= 3; }),
            "host accepts peers up to its capacity");
    // ENet's configured peer count includes the listen host.  The fourth remote
    // must not be admitted when the host was created with the four-peer budget.
    require(host.peers() == 3, "host peer budget admits only three remote peers");
}

void impairedEditsAndLateJoin() {
    Engine authority, a, b, late;
    authority.simulating = false;
    VoxelGrid grid;
    for (int x = 0; x < 20; ++x)
        for (int z = 0; z < 10; ++z)
            grid.set({x, 0, z}, 1);
    auto id = authority.world.spawn("editable floor", std::move(grid));
    Network host, first, second, third;
    host.host(Port);
    UdpProxy p1(Port + 1, Port), p2(Port + 2, Port), p3(Port + 3, Port);
    first.connect("127.0.0.1", Port + 1);
    second.connect("127.0.0.1", Port + 2);
    std::vector<Network *> clients{&first, &second};
    std::vector<Engine *> replicas{&a, &b};
    require(waitFor(host, authority, clients, replicas,
                    [&] { return first.connected() && second.connected(); }),
            "impaired clients synchronize");
    for (int i = 0; i < 10; ++i) {
        Command c;
        c.type = CommandType::SetVoxel;
        c.entity = id;
        c.cell = {i, 0, 0};
        c.material = 0;
        first.sendCommand(c);
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool converged = false;
    while (std::chrono::steady_clock::now() < until) {
        pollAll(host, authority, clients, replicas);
        authority.step();
        host.broadcast(authority);
        if (authority.world.objects.at(id).grid.count() == 190 &&
            a.world.objects.at(id).grid.checksum() ==
                authority.world.objects.at(id).grid.checksum() &&
            b.world.objects.at(id).grid.checksum() ==
                authority.world.objects.at(id).grid.checksum()) {
            converged = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    require(
        converged,
        "reliable edits converge with 100 ms RTT, jitter, reordering and 2 percent packet loss");
    third.connect("127.0.0.1", Port + 3);
    clients.push_back(&third);
    replicas.push_back(&late);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return third.connected() &&
                               late.world.objects.at(id).grid.checksum() ==
                                   authority.world.objects.at(id).grid.checksum();
                    }),
            "late join receives damaged world through impairment");
}
void impairedCollisionAndLateJoin() {
    Engine authority, a, b, late;
    authority.world.materials[1].impactToughness = 1000;
    auto target = authority.world.spawn("impact target", oneVoxel());
    auto striker = authority.world.spawn("striker", oneVoxel(), {{0, 1.5f, 0}, {}});
    authority.world.objects.at(target).anchors.insert({0, 0, 0});
    authority.world.objects.at(striker).anchors.insert({0, 0, 0});
    Network host, first, second, third;
    host.host(Port);
    UdpProxy p1(Port + 1, Port), p2(Port + 2, Port), p3(Port + 3, Port);
    first.connect("127.0.0.1", Port + 1);
    second.connect("127.0.0.1", Port + 2);
    std::vector<Network *> clients{&first, &second};
    std::vector<Engine *> replicas{&a, &b};
    require(waitFor(host, authority, clients, replicas,
                    [&] { return first.connected() && second.connected(); }),
            "collision clients connect under impairment");
    Command launch;
    launch.type = CommandType::ReleaseBody;
    launch.entity = striker;
    launch.vector = {0, -40, 0};
    first.sendCommand(launch);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] { return authority.world.objects.empty(); }),
            "remote launch causes authoritative impact destruction");
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return a.world.objects.empty() && b.world.objects.empty() &&
                                          a.world.materials[1].impactToughness == 1000 &&
                                          b.world.materials[1].impactToughness == 1000;
                               }),
            "collision outcomes converge under loss and jitter");
    third.connect("127.0.0.1", Port + 3);
    clients.push_back(&third);
    replicas.push_back(&late);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return third.connected() &&
                               late.world.checksum() == authority.world.checksum();
                    }),
            "fourth player late-joins impact-destroyed world");
}
void landscapeEditsAndCollapse() {
    Engine authority, replica, late;
    authority.simulating = false;
    LandscapeSettings settings;
    settings.minimum = {0, -8, 0};
    settings.maximum = {64, 8, 8};
    settings.surfaceHeight = 4;
    settings.hillAmplitude = 0;
    settings.overhangStrength = 0;
    settings.caveThreshold = 1;
    auto terrain = authority.generateLandscape(settings);
    auto &object = authority.world.objects.at(terrain);
    for (int y = 4; y < 9; ++y)
        object.grid.set({29, y, 0}, 7);
    for (int x = 29; x < 36; ++x)
        object.grid.set({x, 9, 0}, 7);
    ++object.revision;
    ++authority.world.revision;
    Network host, first, second;
    host.host(Port + 4);
    first.connect("127.0.0.1", Port + 4);
    std::vector<Network *> clients{&first};
    std::vector<Engine *> replicas{&replica};
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return first.connected() &&
                               replica.world.checksum() == authority.world.checksum();
                    }),
            "generated multi-chunk landscape late snapshot matches authority");
    Command edit;
    edit.id = 1;
    edit.entity = terrain;
    edit.type = CommandType::SetVoxel;
    edit.cell = {31, 0, 0};
    edit.material = 0;
    first.sendCommand(edit);
    require(
        waitWithFixedTicks(host, authority, clients, replicas,
                           [&] { return !replica.world.objects.at(terrain).grid.get(edit.cell); }),
        "remote terrain boundary edit replicates");
    edit.id = 2;
    edit.cell = {29, 6, 0};
    first.sendCommand(edit);
    authority.simulating = true;
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return authority.world.objects.size() > 1 &&
                                          replica.world.objects.size() > 1;
                               }),
            "terrain support removal replicates falling cross-chunk fragment");
    authority.simulating = false;
    require(waitWithFixedTicks(
                host, authority, clients, replicas,
                [&] {
                    // Live clients interpolate body transforms. Check the authoritative
                    // serialized state with only those presentation fields normalized,
                    // while independently requiring sub-millimetre pose convergence.
                    World canonical = replica.world;
                    for (const auto &[id, object] : authority.world.objects) {
                        auto found = canonical.objects.find(id);
                        if (found == canonical.objects.end())
                            return false;
                        if (!object.dynamic)
                            continue;
                        auto &copy = found->second;
                        if (length(copy.transform.position - object.transform.position) > .001f ||
                            length(copy.velocity - object.velocity) > .001f ||
                            length(copy.angularVelocity - object.angularVelocity) > .001f)
                            return false;
                        const auto a = copy.transform.rotation, b = object.transform.rotation;
                        if (std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < .99999f)
                            return false;
                        copy.transform = object.transform;
                        copy.velocity = object.velocity;
                        copy.angularVelocity = object.angularVelocity;
                    }
                    return canonical.checksum() == authority.world.checksum();
                }),
            "terrain authoritative checksum matches after accounting for interpolated poses");
    second.connect("127.0.0.1", Port + 4);
    clients.push_back(&second);
    replicas.push_back(&late);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return second.connected() &&
                               late.world.checksum() == authority.world.checksum();
                    }),
            "late join retains terrain excavation, anchors, and fragmented ownership");
}

void playerHandshakeInputsAndReset() {
    auto project = makePlayerProject();
    Engine authority, replica;
    authority.loadProject(project);
    replica.loadProject(project);
    authority.addPlayer(1); // The listen host owns the first slot.
    Network host, client;
    host.host(Port + 5);
    client.connect("127.0.0.1", Port + 5);
    std::vector<Network *> clients{&client};
    std::vector<Engine *> replicas{&replica};
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return host.peers() == 1 && client.connected() &&
                               client.localPlayer() == 2 && authority.players().contains(2) &&
                               replica.players().contains(2);
                    }),
            "character handshake assigns a remote player and preserves the roster");
    require(client.presentedPlayers().contains(2),
            "client keeps a presentation frame for the remote player");

    PlayerInput input;
    input.sequence = 1;
    input.generation = replica.players().at(client.localPlayer()).generation;
    input.moveZ = 1;
    input.yaw = input.pitch = 0;
    client.sendPlayerInput(input);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto remote = authority.players().find(2);
                                   auto copy = replica.players().find(2);
                                   return remote != authority.players().end() &&
                                          copy != replica.players().end() &&
                                          remote->second.acknowledgedInput >= 1 &&
                                          copy->second.acknowledgedInput >=
                                              remote->second.acknowledgedInput;
                               }),
            "player input is acknowledged by the authority and replicated");

    auto generation = authority.players().at(2).generation;
    host.resetRound(authority);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        auto remote = authority.players().find(2);
                        auto copy = replica.players().find(2);
                        return remote != authority.players().end() &&
                               copy != replica.players().end() &&
                               remote->second.generation > generation &&
                               copy->second.generation == remote->second.generation;
                    }),
            "round reset reliably advances recovery generations on every peer");

    // A one-shot recover advances the generation during the following fixed
    // tick.  The peer must accept the next generation rather than disconnect
    // on a delayed frame from the previous life.
    auto beforeManual = authority.players().at(2).generation;
    PlayerInput recover;
    recover.sequence = 1;
    recover.generation = replica.players().at(2).generation;
    recover.recover = true;
    client.sendPlayerInput(recover);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto remote = authority.players().find(2);
                                   auto copy = replica.players().find(2);
                                   return client.connected() &&
                                          remote != authority.players().end() &&
                                          copy != replica.players().end() &&
                                          remote->second.generation > beforeManual &&
                                          copy->second.generation == remote->second.generation;
                               }),
            "manual recovery advances generation without disconnecting the peer");
    PlayerInput afterManual;
    afterManual.sequence = 1;
    afterManual.generation = replica.players().at(2).generation;
    afterManual.moveZ = 1;
    client.sendPlayerInput(afterManual);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto remote = authority.players().find(2);
                                   return remote != authority.players().end() &&
                                          remote->second.acknowledgedInput >= afterManual.sequence;
                               }),
            "movement resumes after manual recovery");

    // Simulate an out-of-bounds state to exercise automatic recovery and the
    // same generation handoff path independently of the recover action.
    auto beforeAutomatic = authority.players().at(2).generation;
    auto outOfBounds = authority.players();
    outOfBounds.at(2).position.y = authority.playerSettings().minimum.y - 1;
    authority.setPlayerStates(std::move(outOfBounds));
    authority.step(1.f / 60.f);
    host.broadcast(authority);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        auto remote = authority.players().find(2);
                        auto copy = replica.players().find(2);
                        return client.connected() && remote != authority.players().end() &&
                               copy != replica.players().end() &&
                               remote->second.generation > beforeAutomatic &&
                               copy->second.generation == remote->second.generation;
                    }),
            "automatic recovery advances generation without disconnecting the peer");
    PlayerInput afterAutomatic;
    afterAutomatic.sequence = 1;
    afterAutomatic.generation = replica.players().at(2).generation;
    afterAutomatic.moveX = .25f;
    client.sendPlayerInput(afterAutomatic);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto remote = authority.players().find(2);
                                   return remote != authority.players().end() &&
                                          remote->second.acknowledgedInput >=
                                              afterAutomatic.sequence;
                               }),
            "movement resumes after automatic recovery");

    // loadProject clears transient players.  The host re-adds its local slot,
    // then resetRound must recreate remote slots with monotonically advanced
    // generations instead of accepting a stale generation-one input.
    auto beforeReload = authority.players().at(2).generation;
    authority.loadProject(project);
    authority.addPlayer(1);
    host.resetRound(authority);
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        auto remote = authority.players().find(2);
                        auto copy = replica.players().find(2);
                        return client.connected() && remote != authority.players().end() &&
                               copy != replica.players().end() &&
                               remote->second.generation > beforeReload &&
                               copy->second.generation == remote->second.generation;
                    }),
            "project reload preserves remote ownership with a new generation");
    PlayerInput afterReload;
    afterReload.sequence = 1;
    afterReload.generation = replica.players().at(2).generation;
    afterReload.moveZ = 1;
    client.sendPlayerInput(afterReload);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   auto remote = authority.players().find(2);
                                   return remote != authority.players().end() &&
                                          remote->second.acknowledgedInput >= afterReload.sequence;
                               }),
            "movement resumes after project reload reset");
    std::filesystem::remove_all(project);
}

void impairedPlayerPredictionConverges() {
    auto project = makePlayerProject();
    Engine authority, replica;
    authority.loadProject(project);
    replica.loadProject(project);
    authority.addPlayer(1);
    Network host, client;
    host.host(Port + 6);
    UdpProxy proxy(Port + 7, Port + 6);
    client.connect("127.0.0.1", Port + 7);
    std::vector<Network *> clients{&client};
    std::vector<Engine *> replicas{&replica};
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return client.connected() && client.localPlayer() == 2 &&
                               authority.players().contains(2) && replica.players().contains(2);
                    }),
            "impaired player client completes character handshake");

    uint64_t sequence = 0;
    auto generation = replica.players().at(2).generation;
    auto tick = [&](bool moving, bool firstPerson = false) {
        pollAll(host, authority, clients, replicas);
        require(replica.players().contains(2), "prediction replica lost player 2");
        auto state = replica.players().at(2);
        if (state.generation != generation) {
            generation = state.generation;
            sequence = 0;
        }
        PlayerInput input;
        input.sequence = ++sequence;
        input.generation = generation;
        input.moveZ = moving ? .75f : 0.f;
        input.moveX = moving ? .25f : 0.f;
        input.yaw = firstPerson ? .1f : 0.f;
        input.pitch = 0;
        client.sendPlayerInput(input);
        replica.predictPlayer(2, input);
        authority.step(1.f / 60.f);
        host.broadcast(authority);
        pollAll(host, authority, clients, replicas);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    };
    for (int i = 0; i < 60; ++i)
        tick(true, true);
    for (int i = 0; i < 60; ++i)
        tick(false);
    auto predicted = replica.predictedPlayer();
    require(predicted && length(predicted->position - authority.players().at(2).position) < .1f,
            "predicted player converges within one second after impaired movement stops");
    std::filesystem::remove_all(project);
}

void dedicatedCharacterPeerBudget() {
    auto project = makePlayerProject();
    Engine authority, a, b, c, d;
    authority.loadProject(project);
    a.loadProject(project);
    b.loadProject(project);
    c.loadProject(project);
    d.loadProject(project);
    Network host, n1, n2, n3, n4;
    host.host(Port + 9);
    n1.connect("127.0.0.1", Port + 9);
    n2.connect("127.0.0.1", Port + 9);
    n3.connect("127.0.0.1", Port + 9);
    n4.connect("127.0.0.1", Port + 9);
    std::vector<Network *> clients{&n1, &n2, &n3, &n4};
    std::vector<Engine *> replicas{&a, &b, &c, &d};
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return host.peers() == 4 && n1.connected() && n2.connected() &&
                               n3.connected() && n4.connected() &&
                               authority.players().size() == 4 && a.players().size() == 4 &&
                               b.players().size() == 4 && c.players().size() == 4 &&
                               d.players().size() == 4;
                    }),
            "dedicated character server admits four remote players");
    for (PlayerId id = 1; id <= 4; ++id) {
        require(authority.players().contains(id), "dedicated roster uses every player slot");
        for (const auto *replica : replicas)
            require(replica->players().contains(id), "dedicated roster reaches every client");
    }
    std::filesystem::remove_all(project);
}

void impairedRoundResetKeepsWorldStream() {
    auto project = makePlayerProject();
    Engine authority, replica;
    authority.loadProject(project);
    replica.loadProject(project);
    authority.addPlayer(1);
    Network host, client;
    host.host(Port + 10);
    UdpProxy proxy(Port + 11, Port + 10);
    client.connect("127.0.0.1", Port + 11);
    std::vector<Network *> clients{&client};
    std::vector<Engine *> replicas{&replica};
    auto waitRealtime = [&](auto done) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            pollAll(host, authority, clients, replicas);
            authority.step();
            host.broadcast(authority);
            pollAll(host, authority, clients, replicas);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    require(waitRealtime([&] { return client.connected() && replica.players().contains(2); }),
            "impaired reset fixture connects");
    for (int n = 0; n < 4; ++n) {
        auto generation = authority.players().at(2).generation;
        authority.loadProject(project);
        authority.addPlayer(1);
        host.resetRound(authority);
        // The app flushes the player reset before publishing the next world snapshot.
        // Different reliable channels can be delivered in either order under jitter.
        pollAll(host, authority, clients, replicas);
        authority.step();
        host.broadcast(authority);
        bool synchronized = waitRealtime([&] {
            return client.connected() && replica.players().contains(2) &&
                   replica.players().at(2).generation > generation && [&] {
                       auto canonical = replica.world;
                       canonical.tick = authority.world.tick;
                       return canonical.checksum() == authority.world.checksum();
                   }();
        });
        if (!synchronized) {
            std::cerr << "reset " << n << " client=" << client.status() << " peers=" << host.peers()
                      << " expectedGen>" << generation << " rosterSize=" << replica.players().size()
                      << "\n";
            for (const auto &event : replica.events)
                if (event.type == "network_error")
                    std::cerr << "client error: " << event.message << "\n";
            for (const auto &event : authority.events)
                if (event.type == "network_error")
                    std::cerr << "host error: " << event.message << "\n";
            auto canonical = replica.world;
            canonical.tick = authority.world.tick;
            std::cerr << "worlds: " << canonical.checksum() << " / " << authority.world.checksum()
                      << " revisions: " << canonical.revision << " / " << authority.world.revision
                      << " objects: " << canonical.objects.size() << " / "
                      << authority.world.objects.size() << "\n";
        }
        require(synchronized, "impaired round reset preserves ordered world snapshots");
    }
    std::filesystem::remove_all(project);
}
void latePlayerPoseCannotReplaceRoster() {
    auto project = makePlayerProject();
    Engine authority, replica;
    authority.loadProject(project);
    replica.loadProject(project);
    authority.addPlayer(1);
    Network host, client;
    host.host(Port + 12);
    client.connect("127.0.0.1", Port + 12);
    std::vector<Network *> clients{&client};
    std::vector<Engine *> replicas{&replica};
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] { return client.connected() && replica.players().contains(2); }),
            "late pose fixture connects");
    auto states = authority.players();
    states.at(2).generation = 5;
    authority.setPlayerStates(states);
    require(waitFor(host, authority, clients, replicas,
                    [&] { return replica.players().at(2).generation == 5; }),
            "prior player generation reaches roster");
    // Re-use a departed slot, then emulate a delayed pose from its former occupant.
    authority.removePlayer(2);
    authority.addPlayer(2);
    require(waitWithFixedTicks(host, authority, clients, replicas,
                               [&] {
                                   return replica.players().at(2).generation == 1 &&
                                          !authority.players().at(2).spawnBlocked;
                               }),
            "replacement player reaches reliable roster");
    auto current = authority.players();
    auto delayed = current;
    delayed.at(2).generation = 5;
    delayed.at(2).position = {99, 99, 99};
    authority.setPlayerStates(delayed);
    authority.world.tick = (authority.world.tick / 3 + 1) * 3;
    host.broadcast(authority);
    authority.setPlayerStates(current);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
        pollAll(host, authority, clients, replicas);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    require(client.connected() && replica.players().at(2).generation == 1 &&
                length(replica.players().at(2).position) < 20,
            "late player pose cannot revive an old connection generation");
    std::filesystem::remove_all(project);
}
int playerSoak(int seconds) {
    auto project = makePlayerProject();
    Engine authority, a, b, c;
    authority.loadProject(project);
    a.loadProject(project);
    b.loadProject(project);
    c.loadProject(project);
    spawnSoakWall(authority);
    authority.addPlayer(1);
    Network host, n1, n2, n3;
    host.host(Port + 8);
    n1.connect("127.0.0.1", Port + 8);
    n2.connect("127.0.0.1", Port + 8);
    n3.connect("127.0.0.1", Port + 8);
    std::vector<Network *> clients{&n1, &n2, &n3};
    std::vector<Engine *> replicas{&a, &b, &c};
    require(waitFor(host, authority, clients, replicas,
                    [&] {
                        return host.peers() == 3 && n1.connected() && n2.connected() &&
                               n3.connected() && authority.players().size() == 4 &&
                               a.players().size() == 4 && b.players().size() == 4 &&
                               c.players().size() == 4;
                    }),
            "four-player soak reaches a complete roster");

    std::array<uint64_t, 3> sequences{};
    std::array<uint32_t, 3> generations{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t frames = 0, resets = 0, toolIntents = 0, committedEdits = 0;
    uint64_t lastCountedEvent = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollAll(host, authority, clients, replicas);
        for (size_t n = 0; n < clients.size(); ++n) {
            auto id = clients[n]->localPlayer();
            require(id != 0 && replicas[n]->players().contains(id),
                    "player soak lost a client roster entry");
            auto state = replicas[n]->players().at(id);
            if (state.generation != generations[n]) {
                generations[n] = state.generation;
                sequences[n] = 0;
            }
            PlayerInput input;
            input.sequence = ++sequences[n];
            input.generation = generations[n];
            const float phase = float(frames % 240) / 240.f * 6.2831853f + float(n);
            input.moveX = .6f * std::sin(phase);
            input.moveZ = .6f * std::cos(phase);
            input.yaw = phase;
            input.pitch = 0;
            input.jump = frames % 180 == 45;
            input.damage = frames % 30 == (10 + n);
            input.build = frames % 90 == (20 + n);
            clients[n]->sendPlayerInput(input);
            replicas[n]->predictPlayer(id, input);
            toolIntents += input.damage || input.build;
            require(replicas[n]->predictionHistorySize() <= 120,
                    "prediction history remains bounded during soak");
        }
        authority.step(1.f / 60.f);
        for (const auto &event : authority.events) {
            if (event.sequence > lastCountedEvent &&
                (event.type == "damage" || event.type == "construction"))
                ++committedEdits;
            lastCountedEvent = std::max(lastCountedEvent, event.sequence);
        }
        host.broadcast(authority);
        pollAll(host, authority, clients, replicas);
        require(authority.players().size() <= 4 && a.players().size() <= 4 &&
                    b.players().size() <= 4 && c.players().size() <= 4,
                "player roster remains bounded during soak");
        if (++frames % 600 == 0) {
            authority.loadProject(project);
            spawnSoakWall(authority);
            authority.addPlayer(1);
            host.resetRound(authority);
            host.broadcast(authority);

            ++resets;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::cout << "player soak: " << seconds << "s, " << frames << " frames, " << toolIntents
              << " tool intents, " << committedEdits << " committed edits, " << resets
              << " resets\n";
    require(committedEdits > 0, "four-player soak commits authoritative tool edits");
    require(authority.players().size() == 4 && a.players().size() == 4 && b.players().size() == 4 &&
                c.players().size() == 4,
            "four-player soak ends with a complete roster");
    std::filesystem::remove_all(project);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--player-soak-seconds") {
        int seconds = std::stoi(argv[2]);
        if (seconds <= 0 || seconds > 3600)
            throw std::runtime_error("player soak seconds must be between 1 and 3600");
        try {
            return playerSoak(seconds);
        } catch (const std::exception &ex) {
            std::cerr << "player soak failed: " << ex.what() << "\n";
            return 1;
        }
    }
    if (argc != 1)
        throw std::runtime_error("usage: network_tests [--player-soak-seconds N]");
    runCase("loopbackHandshakeAndBroadcast", loopbackHandshakeAndBroadcast);
    runCase("hostPeerBudgetIncludesHost", hostPeerBudgetIncludesHost);
    runCase("impairedEditsAndLateJoin", impairedEditsAndLateJoin);
    runCase("impairedCollisionAndLateJoin", impairedCollisionAndLateJoin);
    runCase("landscapeEditsAndCollapse", landscapeEditsAndCollapse);
    runCase("playerHandshakeInputsAndReset", playerHandshakeInputsAndReset);
    runCase("impairedPlayerPredictionConverges", impairedPlayerPredictionConverges);
    runCase("dedicatedCharacterPeerBudget", dedicatedCharacterPeerBudget);
    runCase("impairedRoundResetKeepsWorldStream", impairedRoundResetKeepsWorldStream);
    runCase("latePlayerPoseCannotReplaceRoster", latePlayerPoseCannotReplaceRoster);
}
