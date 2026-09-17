#include "aspera/engine.hpp"

#include "udp_proxy.hpp"
#include <chrono>
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

} // namespace

int main() {
    loopbackHandshakeAndBroadcast();
    hostPeerBudgetIncludesHost();
    impairedEditsAndLateJoin();
    impairedCollisionAndLateJoin();
    landscapeEditsAndCollapse();
}
