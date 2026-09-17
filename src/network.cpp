#include "aspera/engine.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <enet/enet.h>
#include <stdexcept>

namespace aspera {
namespace {
constexpr uint32_t Protocol = 3, MaxState = 128 * 1024 * 1024, Block = 32768;
enum : uint8_t { Hello = 1, Welcome, Begin, Data, Bodies, Edit, Resync };
struct Bytes {
    std::vector<uint8_t> b;
    size_t p{};
    template <class T> void put(T v) {
        if constexpr (std::is_floating_point_v<T>)
            put(std::bit_cast<uint32_t>(v));
        else
            for (size_t i = 0; i < sizeof(T); ++i)
                b.push_back(uint8_t(uint64_t(v) >> (i * 8)));
    }
    template <class T> T get() {
        if constexpr (std::is_floating_point_v<T>) {
            float f = std::bit_cast<float>(get<uint32_t>());
            if (!std::isfinite(f))
                throw std::runtime_error("Non-finite packet");
            return f;
        } else {
            if (p + sizeof(T) > b.size())
                throw std::runtime_error("Truncated packet");
            uint64_t v = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                v |= uint64_t(b[p++]) << (i * 8);
            return T(v);
        }
    }
    void vec(Vec3 v) {
        put(v.x);
        put(v.y);
        put(v.z);
    }
    Vec3 vec() {
        return {get<float>(), get<float>(), get<float>()};
    }
};
void send(ENetPeer *peer, uint8_t channel, const Bytes &b, bool reliable = true) {
    auto packet = enet_packet_create(b.b.data(), b.b.size(),
                                     reliable ? ENET_PACKET_FLAG_RELIABLE
                                              : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (!packet || enet_peer_send(peer, channel, packet) < 0) {
        if (packet)
            enet_packet_destroy(packet);
        throw std::runtime_error("Network send failed");
    }
}
uint64_t content(const Engine &e) {
    auto p = e.world.properties.find("_content_hash");
    if (p != e.world.properties.end())
        return uint64_t(p->second);
    uint64_t h = 1469598103934665603ull;
    for (auto c : e.projectName()) {
        h ^= uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}
uint64_t topology(const World &w) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    for (auto &[id, o] : w.objects) {
        mix(id);
        mix(o.revision);
        mix(o.grid.checksum());
        for (auto s : o.seams)
            for (auto p : {s.a, s.b}) {
                mix(uint64_t(p.x));
                mix(uint64_t(p.y));
                mix(uint64_t(p.z));
            }
    }
    return h;
}
} // namespace
struct Network::Impl {
    struct Pose {
        Transform transform;
        Vec3 velocity, angular;
        uint64_t revision;
    };
    std::map<EntityId, Pose> targets;
    std::chrono::steady_clock::time_point lastPoll = std::chrono::steady_clock::now();
    ENetHost *host{};
    ENetPeer *server{};
    bool client{}, ready{};
    std::string status{"Offline"};
    struct Peer {
        bool ready{};
        uint64_t lastCommand{}, sequence{};
        uint32_t commandCount{};
        std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
        std::map<EntityId, uint64_t> revisions;
        bool full{true};
        std::vector<uint8_t> outgoing;
        size_t offset{};
    };
    std::map<ENetPeer *, Peer> peers;
    uint64_t nextCommand{}, lastBodyTick{}, lastBroadcast{UINT64_MAX};
    uint64_t incomingSequence{}, appliedSequence{};
    uint32_t incomingSize{};
    std::vector<uint8_t> incoming;
    void state(ENetPeer *peer, Peer &state, const World &w) {
        World patch;
        patch.voxelSize = w.voxelSize;
        patch.tick = w.tick;
        patch.revision = w.revision;
        patch.nextEntity = w.nextEntity;
        patch.materials = w.materials;
        patch.properties = w.properties;
        patch.environment = w.environment;
        patch.lights = w.lights;
        patch.nextLight = w.nextLight;
        std::vector<EntityId> removed;
        for (auto &[id, rev] : state.revisions)
            if (!w.objects.contains(id))
                removed.push_back(id);
        for (auto &[id, o] : w.objects)
            if (state.full || !state.revisions.contains(id) || state.revisions[id] != o.revision)
                patch.objects[id] = o;
        auto payload = encodeWorld(patch);
        Bytes bytes;
        bytes.put<uint8_t>(state.full);
        bytes.put<uint64_t>(topology(w));
        bytes.put<uint32_t>(removed.size());
        for (auto id : removed)
            bytes.put(id);
        bytes.put<uint32_t>(payload.size());
        bytes.b.insert(bytes.b.end(), payload.begin(), payload.end());
        bytes.put<uint32_t>(w.joints.size());
        for (auto j : w.joints) {
            bytes.put(j.id);
            bytes.put(j.a);
            bytes.put(j.b);
            bytes.vec(j.anchor);
            bytes.put<uint8_t>(j.hinge);
            bytes.put(j.strength);
        }
        if (bytes.b.size() > MaxState)
            throw std::runtime_error("Replication snapshot budget exceeded");
        state.outgoing = std::move(bytes.b);
        state.offset = 0;
        state.revisions.clear();
        for (auto &[id, o] : w.objects)
            state.revisions[id] = o.revision;
        state.full = false;
        Bytes begin;
        begin.put<uint8_t>(Begin);
        begin.put<uint64_t>(++state.sequence);
        begin.put<uint32_t>(state.outgoing.size());
        send(peer, 0, begin);
    }
    void apply(Engine &e) {
        Bytes bytes;
        bytes.b = std::move(incoming);
        bool full = bytes.get<uint8_t>() != 0;
        auto expectedHash = bytes.get<uint64_t>();
        auto removedCount = bytes.get<uint32_t>();
        if (removedCount > 4096)
            throw std::runtime_error("Invalid removal count");
        std::vector<EntityId> removed;
        for (uint32_t i = 0; i < removedCount; ++i)
            removed.push_back(bytes.get<uint64_t>());
        auto size = bytes.get<uint32_t>();
        if (size > MaxState || bytes.p + size > bytes.b.size())
            throw std::runtime_error("Invalid state payload");
        std::vector<uint8_t> payload(bytes.b.begin() + bytes.p, bytes.b.begin() + bytes.p + size);
        bytes.p += size;
        World patch = decodeWorld(payload);
        World candidate = full ? World{} : e.world;
        candidate.voxelSize = patch.voxelSize;
        candidate.tick = patch.tick;
        candidate.revision = patch.revision;
        candidate.nextEntity = patch.nextEntity;
        candidate.materials = std::move(patch.materials);
        candidate.properties = std::move(patch.properties);
        candidate.environment = patch.environment;
        candidate.lights = std::move(patch.lights);
        candidate.nextLight = patch.nextLight;
        for (auto id : removed)
            candidate.objects.erase(id);
        for (auto &[id, o] : patch.objects)
            candidate.objects[id] = std::move(o);
        candidate.joints.clear();
        auto nj = bytes.get<uint32_t>();
        if (nj > 8192)
            throw std::runtime_error("Invalid joint count");
        for (uint32_t i = 0; i < nj; ++i) {
            Joint j;
            j.id = bytes.get<uint64_t>();
            j.a = bytes.get<uint64_t>();
            j.b = bytes.get<uint64_t>();
            j.anchor = bytes.vec();
            j.hinge = bytes.get<uint8_t>() != 0;
            j.strength = bytes.get<float>();
            if (!candidate.objects.contains(j.a) || !candidate.objects.contains(j.b) ||
                j.strength <= 0)
                throw std::runtime_error("Invalid replicated joint");
            candidate.joints.push_back(j);
        }
        if (bytes.p != bytes.b.size())
            throw std::runtime_error("Trailing state bytes");
        if (topology(candidate) != expectedHash) {
            Bytes request;
            request.put<uint8_t>(Resync);
            send(server, 0, request);
            status = "Resynchronizing";
            return;
        }
        e.world = std::move(candidate);
        if (!ready)
            e.resetPhysics();
        ready = true;
        status = "Connected";
        appliedSequence = incomingSequence;
        e.emit("network", 0, full ? "World synchronized" : "World delta applied");
    }
    ~Impl() {
        if (host)
            enet_host_destroy(host);
    }
};
Network::Network() : impl_(std::make_unique<Impl>()) {
    static struct Lifetime {
        Lifetime() {
            if (enet_initialize() != 0)
                throw std::runtime_error("ENet initialization failed");
        }
        ~Lifetime() {
            enet_deinitialize();
        }
    } lifetime;
}
Network::~Network() = default;
void Network::host(uint16_t port) {
    if (impl_->host)
        throw std::runtime_error("Session already active");
    ENetAddress address{ENET_HOST_ANY, port};
    impl_->host = enet_host_create(&address, 3, 3, 0, 0);
    if (!impl_->host)
        throw std::runtime_error("Cannot host on port " + std::to_string(port));
    impl_->host->maximumPacketSize = 65536;
    impl_->host->maximumWaitingData = 8 * 1024 * 1024;
    impl_->status = "Hosting :" + std::to_string(port);
    impl_->ready = true;
}
void Network::connect(const std::string &address, uint16_t port) {
    if (impl_->host)
        throw std::runtime_error("Session already active");
    impl_->host = enet_host_create(nullptr, 1, 3, 0, 0);
    if (!impl_->host)
        throw std::runtime_error("Cannot create client");
    impl_->host->maximumPacketSize = 65536;
    impl_->host->maximumWaitingData = 8 * 1024 * 1024;
    ENetAddress target{};
    if (enet_address_set_host(&target, address.c_str()) < 0)
        throw std::runtime_error("Cannot resolve server");
    target.port = port;
    impl_->server = enet_host_connect(impl_->host, &target, 3, Protocol);
    if (!impl_->server)
        throw std::runtime_error("Cannot connect");
    impl_->client = true;
    impl_->status = "Connecting";
}
void Network::poll(Engine &e) {
    if (!impl_->host)
        return;
    ENetEvent event{};
    int count = 0;
    while (count++ < 256 && enet_host_service(impl_->host, &event, 0) > 0) {
        try {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                if (impl_->client) {
                    Bytes hello;
                    hello.put<uint8_t>(Hello);
                    hello.put(Protocol);
                    hello.put(content(e));
                    send(event.peer, 0, hello);
                } else
                    impl_->peers[event.peer] = {};
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                if (impl_->client) {
                    impl_->ready = false;
                    impl_->status = "Disconnected";
                    impl_->incoming.clear();
                } else
                    impl_->peers.erase(event.peer);
                e.emit("player_disconnected");
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                Bytes b;
                b.b.assign(event.packet->data, event.packet->data + event.packet->dataLength);
                auto type = b.get<uint8_t>();
                if (!impl_->client) {
                    auto &peer = impl_->peers.at(event.peer);
                    if (type == Hello) {
                        if (peer.ready)
                            throw std::runtime_error("Handshake already completed");
                        if (b.get<uint32_t>() != Protocol)
                            throw std::runtime_error(
                                "Protocol 3 required: lighting replication is incompatible");
                        if (b.get<uint64_t>() != content(e) || b.p != b.b.size())
                            throw std::runtime_error("Game content mismatch");
                        peer.ready = true;
                        Bytes welcome;
                        welcome.put<uint8_t>(Welcome);
                        send(event.peer, 0, welcome);
                        impl_->state(event.peer, peer, e.world);
                        e.emit("player_connected");
                    } else if (!peer.ready)
                        throw std::runtime_error("Handshake required");
                    else if (type == Resync) {
                        peer.full = true;
                    } else if (type == Edit) {
                        auto now = std::chrono::steady_clock::now();
                        if (now - peer.window > std::chrono::seconds(1)) {
                            peer.window = now;
                            peer.commandCount = 0;
                        }
                        if (++peer.commandCount > 60)
                            throw std::runtime_error("Command rate exceeded");
                        Command c;
                        c.id = b.get<uint64_t>();
                        if (c.id <= peer.lastCommand)
                            throw std::runtime_error("Command sequence invalid");
                        peer.lastCommand = c.id;
                        auto t = b.get<uint8_t>();
                        if (t > uint8_t(CommandType::ReleaseBody))
                            throw std::runtime_error("Unknown command");
                        c.type = CommandType(t);
                        c.entity = b.get<uint64_t>();
                        c.cell = {b.get<int32_t>(), b.get<int32_t>(), b.get<int32_t>()};
                        c.material = b.get<uint16_t>();
                        c.point = b.vec();
                        c.vector = b.vec();
                        c.radius = b.get<float>();
                        c.strength = b.get<float>();
                        c.expectedRevision = b.get<uint64_t>();
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing command bytes");
                        c.player = event.peer->incomingPeerID + 1;
                        e.submit(c);
                    } else
                        throw std::runtime_error("Unexpected client packet");
                } else {
                    if (type == Welcome)
                        impl_->status = "Synchronizing world";
                    else if (type == Begin) {
                        auto sequence = b.get<uint64_t>();
                        auto size = b.get<uint32_t>();
                        if (sequence <= impl_->appliedSequence || size > MaxState || size == 0)
                            throw std::runtime_error("Invalid snapshot header");
                        impl_->incomingSequence = sequence;
                        impl_->incomingSize = size;
                        impl_->incoming.clear();
                        impl_->incoming.reserve(size);
                    } else if (type == Data) {
                        auto sequence = b.get<uint64_t>();
                        auto offset = b.get<uint32_t>();
                        if (sequence != impl_->incomingSequence ||
                            offset != impl_->incoming.size() ||
                            impl_->incoming.size() + b.b.size() - b.p > impl_->incomingSize)
                            throw std::runtime_error("Invalid snapshot block");
                        impl_->incoming.insert(impl_->incoming.end(), b.b.begin() + b.p, b.b.end());
                        if (impl_->incoming.size() == impl_->incomingSize)
                            impl_->apply(e);
                    } else if (type == Bodies) {
                        auto tick = b.get<uint64_t>();
                        auto sequence = b.get<uint64_t>();
                        auto n = b.get<uint32_t>();
                        if (n > 500)
                            throw std::runtime_error("Body snapshot budget exceeded");
                        bool apply = impl_->ready && tick > impl_->lastBodyTick;
                        (void)sequence;
                        for (uint32_t i = 0; i < n; ++i) {
                            auto id = b.get<uint64_t>();
                            auto revision = b.get<uint64_t>();
                            auto position = b.vec();
                            Quat q{b.get<float>(), b.get<float>(), b.get<float>(), b.get<float>()};
                            auto velocity = b.vec();
                            auto angular = b.vec();
                            auto it = e.world.objects.find(id);
                            float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                            if (std::abs(norm - 1) > 0.01f)
                                throw std::runtime_error("Invalid body rotation");
                            if (apply && it != e.world.objects.end() &&
                                revision == it->second.revision) {
                                impl_->targets[id] = {{position, q}, velocity, angular, revision};
                            }
                        }
                        if (apply)
                            impl_->lastBodyTick = tick;
                    } else
                        throw std::runtime_error("Unexpected server packet");
                }
            }
        } catch (const std::exception &ex) {
            e.emit("network_error", 0, ex.what());
            if (impl_->client) {
                impl_->status = ex.what();
                impl_->ready = false;
            } else
                impl_->peers.erase(event.peer);
            enet_peer_disconnect_now(event.peer, 1);
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(event.packet);
    }
    if (!impl_->client) {
        for (auto &[peer, state] : impl_->peers) {
            size_t blocks = 0;
            while (state.offset < state.outgoing.size() && blocks++ < 8 &&
                   enet_list_size(&peer->outgoingCommands) < 64) {
                Bytes b;
                b.put<uint8_t>(Data);
                b.put(state.sequence);
                b.put<uint32_t>(state.offset);
                size_t n = std::min<size_t>(Block, state.outgoing.size() - state.offset);
                b.b.insert(b.b.end(), state.outgoing.begin() + state.offset,
                           state.outgoing.begin() + state.offset + n);
                send(peer, 0, b);
                state.offset += n;
            }
            if (state.offset == state.outgoing.size())
                state.outgoing.clear();
        }
    }
    if (impl_->client && impl_->ready) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - impl_->lastPoll).count();
        impl_->lastPoll = now;
        float f = 1 - std::exp(-std::min(dt, .1f) * 20);
        for (auto it = impl_->targets.begin(); it != impl_->targets.end();) {
            auto obj = e.world.objects.find(it->first);
            if (obj == e.world.objects.end() || obj->second.revision != it->second.revision) {
                it = impl_->targets.erase(it);
                continue;
            }
            auto &o = obj->second;
            auto &target = it->second;
            o.transform.position =
                o.transform.position + (target.transform.position - o.transform.position) * f;
            auto a = o.transform.rotation, b = target.transform.rotation;
            float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? -1.f : 1.f;
            Quat q{a.x + (b.x * sign - a.x) * f, a.y + (b.y * sign - a.y) * f,
                   a.z + (b.z * sign - a.z) * f, a.w + (b.w * sign - a.w) * f};
            float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            o.transform.rotation = {q.x / norm, q.y / norm, q.z / norm, q.w / norm};
            o.velocity = target.velocity;
            o.angularVelocity = target.angular;
            ++it;
        }
    }
    enet_host_flush(impl_->host);
}
void Network::sendCommand(Command c) {
    if (!impl_->client || !impl_->ready)
        return;
    Bytes b;
    b.put<uint8_t>(Edit);
    b.put(++impl_->nextCommand);
    b.put<uint8_t>(uint8_t(c.type));
    b.put(c.entity);
    b.put<int32_t>(c.cell.x);
    b.put<int32_t>(c.cell.y);
    b.put<int32_t>(c.cell.z);
    b.put(c.material);
    b.vec(c.point);
    b.vec(c.vector);
    b.put(c.radius);
    b.put(c.strength);
    b.put(c.expectedRevision);
    send(impl_->server, 0, b);
}
void Network::broadcast(Engine &e) {
    if (!impl_->host || impl_->client || e.world.tick % 3 != 0 ||
        impl_->lastBroadcast == e.world.tick)
        return;
    impl_->lastBroadcast = e.world.tick;
    for (auto &[peer, state] : impl_->peers) {
        if (!state.ready || !state.outgoing.empty())
            continue;
        impl_->state(peer, state, e.world);
        Bytes b;
        b.put<uint8_t>(Bodies);
        b.put(e.world.tick);
        b.put(state.sequence - 1);
        uint32_t count = 0;
        for (auto &[id, o] : e.world.objects)
            if (o.dynamic && count < 500)
                ++count;
        b.put(count);
        for (auto &[id, o] : e.world.objects) {
            if (!o.dynamic || !count)
                continue;
            --count;
            b.put(id);
            b.put(o.revision);
            b.vec(o.transform.position);
            auto q = o.transform.rotation;
            b.put(q.x);
            b.put(q.y);
            b.put(q.z);
            b.put(q.w);
            b.vec(o.velocity);
            b.vec(o.angularVelocity);
        }
        send(peer, 1, b, false);
    }
    e.stats.networkKB = impl_->host->totalSentData / 1024.;
}
bool Network::isClient() const {
    return impl_->client;
}
bool Network::connected() const {
    return impl_->ready;
}
size_t Network::peers() const {
    size_t n = 0;
    for (auto &[p, s] : impl_->peers)
        n += s.ready;
    return impl_->client ? (impl_->ready ? 1 : 0) : n;
}
std::string Network::status() const {
    return impl_->status;
}
} // namespace aspera
