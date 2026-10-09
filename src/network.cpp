#include "aspera/engine.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <enet/enet.h>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace aspera {
namespace {
constexpr uint32_t Protocol = 4, MaxState = 128 * 1024 * 1024, Block = 32768;
constexpr uint32_t MaxPlayers = 4, MaxInputBatch = 8, MaxInputQueue = 120;
// Channel 0 carries reliable world transactions, channel 1 body poses, and
// channel 2 player membership, inputs, and snapshots.
constexpr uint8_t Channels = 3;
enum : uint8_t {
    Hello = 1,
    Welcome,
    Begin,
    Data,
    Bodies,
    Edit,
    Resync,
    PlayerInputs,
    PlayerSnapshot,
    PlayerRoster,
    RoundReset
};
enum : uint8_t {
    InputJump = 1 << 0,
    InputDamage = 1 << 1,
    InputBuild = 1 << 2,
    InputRecover = 1 << 3
};
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
uint8_t inputFlags(const PlayerInput &input) {
    return uint8_t(input.jump ? InputJump : 0) | uint8_t(input.damage ? InputDamage : 0) |
           uint8_t(input.build ? InputBuild : 0) | uint8_t(input.recover ? InputRecover : 0);
}
void validateInput(const PlayerInput &input) {
    if (!validPlayerInput(input))
        throw std::runtime_error("Invalid player input");
}
void putInput(Bytes &bytes, const PlayerInput &input) {
    validateInput(input);
    bytes.put(input.sequence);
    bytes.put(input.generation);
    bytes.put(input.moveX);
    bytes.put(input.moveZ);
    bytes.put(input.yaw);
    bytes.put(input.pitch);
    bytes.put(inputFlags(input));
}
PlayerInput getInput(Bytes &bytes) {
    PlayerInput input;
    input.sequence = bytes.get<uint64_t>();
    input.generation = bytes.get<uint32_t>();
    input.moveX = bytes.get<float>();
    input.moveZ = bytes.get<float>();
    input.yaw = bytes.get<float>();
    input.pitch = bytes.get<float>();
    auto flags = bytes.get<uint8_t>();
    if (flags & ~(InputJump | InputDamage | InputBuild | InputRecover))
        throw std::runtime_error("Unknown player input flag");
    input.jump = flags & InputJump;
    input.damage = flags & InputDamage;
    input.build = flags & InputBuild;
    input.recover = flags & InputRecover;
    validateInput(input);
    return input;
}
void putPlayerState(Bytes &bytes, const PlayerState &state) {
    if (state.id == 0 || state.id > MaxPlayers)
        throw std::runtime_error("Invalid player id");
    if (!std::isfinite(state.yaw) || !std::isfinite(state.pitch))
        throw std::runtime_error("Invalid player state view angles");
    bytes.put(state.id);
    bytes.vec(state.position);
    bytes.vec(state.velocity);
    bytes.put(state.yaw);
    bytes.put(state.pitch);
    bytes.put<uint8_t>(state.grounded ? 1 : 0);
    bytes.put<uint8_t>(state.spawnBlocked ? 1 : 0);
    bytes.put(state.support);
    bytes.put(state.acknowledgedInput);
    bytes.put(state.collisionRevision);
    bytes.put(state.generation);
}
PlayerState getPlayerState(Bytes &bytes) {
    PlayerState state;
    state.id = bytes.get<PlayerId>();
    if (state.id == 0 || state.id > MaxPlayers)
        throw std::runtime_error("Invalid player id");
    state.position = bytes.vec();
    state.velocity = bytes.vec();
    state.yaw = bytes.get<float>();
    state.pitch = bytes.get<float>();
    if (!std::isfinite(state.yaw) || !std::isfinite(state.pitch))
        throw std::runtime_error("Invalid player state view angles");
    auto grounded = bytes.get<uint8_t>();
    auto blocked = bytes.get<uint8_t>();
    if (grounded > 1 || blocked > 1)
        throw std::runtime_error("Invalid player state flags");
    state.grounded = grounded != 0;
    state.spawnBlocked = blocked != 0;
    state.support = bytes.get<EntityId>();
    state.acknowledgedInput = bytes.get<uint64_t>();
    state.collisionRevision = bytes.get<uint64_t>();
    state.generation = bytes.get<uint32_t>();
    if (state.generation == 0)
        throw std::runtime_error("Invalid player generation");
    return state;
}
std::map<PlayerId, PlayerState> getPlayerStates(Bytes &bytes) {
    auto count = bytes.get<uint32_t>();
    if (count > MaxPlayers)
        throw std::runtime_error("Player roster budget exceeded");
    std::map<PlayerId, PlayerState> states;
    for (uint32_t i = 0; i < count; ++i) {
        auto state = getPlayerState(bytes);
        if (!states.emplace(state.id, state).second)
            throw std::runtime_error("Duplicate player id");
    }
    return states;
}
void putPlayerStates(Bytes &bytes, const std::map<PlayerId, PlayerState> &states) {
    if (states.size() > MaxPlayers)
        throw std::runtime_error("Player roster budget exceeded");
    bytes.put<uint32_t>(states.size());
    for (const auto &[id, state] : states) {
        if (id != state.id)
            throw std::runtime_error("Player roster id mismatch");
        putPlayerState(bytes, state);
    }
}
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
        bool character{};
        PlayerId player{};
        uint32_t generation{1};
        uint64_t lastCommand{}, sequence{};
        uint32_t commandCount{};
        std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
        uint64_t lastInputSequence{};
        uint64_t lastAppliedInput{};
        std::chrono::steady_clock::time_point inputWindow = std::chrono::steady_clock::now();
        uint32_t inputCount{};
        uint32_t inputPackets{};
        std::chrono::steady_clock::time_point gapStarted{};
        std::map<EntityId, uint64_t> revisions;
        std::map<uint64_t, PlayerInput> inputQueue;
        bool full{true};
        std::vector<uint8_t> outgoing;
        size_t offset{};
        // One budget for every client-driven request, so a resync -- which costs
        // the authority a full-world re-encode -- cannot bypass the edit quota.
        void quota(uint32_t cost) {
            auto now = std::chrono::steady_clock::now();
            if (now - window > std::chrono::seconds(1)) {
                window = now;
                commandCount = 0;
            }
            commandCount += cost;
            if (commandCount > 60)
                throw std::runtime_error("Command rate exceeded");
        }
        void inputQuota(uint32_t cost) {
            auto now = std::chrono::steady_clock::now();
            if (now - inputWindow > std::chrono::seconds(1)) {
                inputWindow = now;
                inputCount = 0;
                inputPackets = 0;
            }
            inputCount += cost;
            ++inputPackets;
            if (inputCount > 512 || inputPackets > 90)
                throw std::runtime_error("Player input rate exceeded");
        }
    };
    std::map<ENetPeer *, Peer> peers;
    uint64_t nextCommand{}, lastBodyTick{}, lastBroadcast{UINT64_MAX};
    uint64_t incomingSequence{}, appliedSequence{};
    uint32_t incomingSize{};
    std::vector<uint8_t> incoming;
    // Body poses are scoped to the world snapshot sequence.  After a round
    // reset, delayed channel-one packets from the previous world must not
    // become the first pose applied to the new world.
    uint64_t bodySequenceFloor{};
    PlayerId local{};
    uint32_t localGeneration{1};
    uint64_t nextInput{};
    std::deque<PlayerInput> pendingInputs;
    std::chrono::steady_clock::time_point lastInputSend = std::chrono::steady_clock::now();
    uint64_t lastPlayerTick{};
    uint64_t rosterSequence{};
    uint64_t lastRosterSequence{};
    std::map<PlayerId, uint32_t> rosterGenerations;
    bool forceBroadcast{};
    struct PlayerFrame {
        PlayerState state;
        std::chrono::steady_clock::time_point received;
    };
    std::map<PlayerId, std::deque<PlayerFrame>> playerFrames;
    void recordPlayerFrames(const std::map<PlayerId, PlayerState> &states) {
        auto now = std::chrono::steady_clock::now();
        std::set<PlayerId> present;
        for (const auto &[id, state] : states) {
            present.insert(id);
            auto &frames = playerFrames[id];
            if (!frames.empty() && frames.back().state.generation != state.generation)
                frames.clear();
            frames.push_back({state, now});
            while (frames.size() > 64 ||
                   (!frames.empty() && now - frames.front().received > std::chrono::seconds(2)))
                frames.pop_front();
        }
        for (auto it = playerFrames.begin(); it != playerFrames.end();) {
            if (!present.contains(it->first))
                it = playerFrames.erase(it);
            else
                ++it;
        }
    }
    std::map<PlayerId, PlayerState> presentedPlayers() const {
        std::map<PlayerId, PlayerState> result;
        auto target = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        auto angle = [](float a, float b, float t) {
            constexpr float Pi = 3.14159265358979323846f;
            float delta = std::fmod(b - a + Pi, 2 * Pi);
            if (delta < 0)
                delta += 2 * Pi;
            delta -= Pi;
            return a + delta * t;
        };
        for (const auto &[id, frames] : playerFrames) {
            if (frames.empty())
                continue;
            if (id == local || frames.size() == 1) {
                result[id] = frames.back().state;
                continue;
            }
            if (target <= frames.front().received) {
                result[id] = frames.front().state;
                continue;
            }
            if (target >= frames.back().received) {
                auto state = frames.back().state;
                float dt = std::chrono::duration<float>(target - frames.back().received).count();
                dt = std::min(dt, .1f);
                state.position = state.position + state.velocity * dt;
                result[id] = state;
                continue;
            }
            auto upper = std::upper_bound(
                frames.begin(), frames.end(), target,
                [](auto time, const PlayerFrame &frame) { return time < frame.received; });
            auto &b = *upper;
            auto &a = *std::prev(upper);
            const float span = std::chrono::duration<float>(b.received - a.received).count();
            if (span <= 0) {
                result[id] = b.state;
                continue;
            }
            float t = std::chrono::duration<float>(target - a.received).count() / span;
            t = std::clamp(t, 0.f, 1.f);
            auto state = b.state;
            state.position = a.state.position + (b.state.position - a.state.position) * t;
            state.velocity = a.state.velocity + (b.state.velocity - a.state.velocity) * t;
            state.yaw = angle(a.state.yaw, b.state.yaw, t);
            state.pitch = a.state.pitch + (b.state.pitch - a.state.pitch) * t;
            result[id] = state;
        }
        return result;
    }
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
        auto players = e.players();
        // A full snapshot also follows a round/project reset while the client
        // remains connected.  Rebuild physics for the new world, then restore
        // the transient roster that resetPhysics intentionally clears.
        if (!ready || full) {
            e.resetPhysics();
            e.setPlayerStates(std::move(players));
        }
        ready = true;
        status = "Connected";
        appliedSequence = incomingSequence;
        e.emit("network", 0, full ? "World synchronized" : "World delta applied");
    }
    bool rosterChanged(const Engine &e) const {
        const auto &states = e.players();
        if (states.size() != rosterGenerations.size())
            return true;
        for (const auto &[id, state] : states) {
            auto it = rosterGenerations.find(id);
            if (it == rosterGenerations.end() || it->second != state.generation)
                return true;
        }
        return false;
    }
    void rememberRoster(const Engine &e) {
        rosterGenerations.clear();
        for (const auto &[id, state] : e.players())
            rosterGenerations[id] = state.generation;
    }
    void sendRoster(ENetPeer *peer, const Engine &e, uint8_t type) {
        Bytes bytes;
        bytes.put<uint8_t>(type);
        bytes.put<uint64_t>(++rosterSequence);
        if (type == RoundReset) {
            auto found = peers.find(peer);
            uint64_t floor = 1;
            if (found != peers.end())
                floor = found->second.sequence == std::numeric_limits<uint64_t>::max()
                            ? found->second.sequence
                            : found->second.sequence + 1;
            bytes.put(floor);
        }
        putPlayerStates(bytes, e.players());
        send(peer, 2, bytes);
    }
    void broadcastRoster(Engine &e, uint8_t type = PlayerRoster) {
        std::vector<ENetPeer *> failed;
        for (auto &[peer, state] : peers) {
            if (!state.ready)
                continue;
            try {
                sendRoster(peer, e, type);
            } catch (const std::exception &ex) {
                e.emit("network_error", 0, ex.what());
                failed.push_back(peer);
            }
        }
        rememberRoster(e);
        for (auto *peer : failed) {
            if (auto it = peers.find(peer); it != peers.end() && it->second.player)
                e.removePlayer(it->second.player);
            peers.erase(peer);
            enet_peer_disconnect_now(peer, 1);
        }
    }
    void sendPlayerSnapshot(ENetPeer *peer, const Engine &e) {
        Bytes bytes;
        bytes.put<uint8_t>(PlayerSnapshot);
        bytes.put<uint64_t>(e.world.tick);
        putPlayerStates(bytes, e.players());
        send(peer, 2, bytes, false);
    }
    PlayerId allocatePlayer(const Engine &e) const {
        // A listen host reserves slot one before remote handshakes.  A
        // dedicated character server has no local player, so its first remote
        // occupies slot one and all four slots remain available.
        const PlayerId first = e.players().contains(1) ? 2 : 1;
        for (PlayerId id = first; id <= MaxPlayers; ++id)
            if (!e.players().contains(id))
                return id;
        throw std::runtime_error("Player capacity reached");
    }
    void resetPeerInputs(Peer &peer, uint32_t generation) {
        peer.generation = generation;
        peer.lastInputSequence = 0;
        peer.lastAppliedInput = 0;
        peer.inputQueue.clear();
        peer.gapStarted = {};
    }
    void syncPeerGenerations(const Engine &e) {
        if (client)
            return;
        for (auto &[peer, state] : peers) {
            if (!state.ready || !state.character || !state.player)
                continue;
            auto it = e.players().find(state.player);
            // A project reload temporarily clears the transient roster.  Keep
            // the authenticated peer alive until resetRound reconstitutes it;
            // its queued inputs are discarded by resetRound.
            if (it == e.players().end())
                continue;
            if (it->second.generation != state.generation)
                resetPeerInputs(state, it->second.generation);
        }
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
    // ENet counts only remote peers here.  Keep room for four character
    // players; legacy editor-only sessions are explicitly capped below so
    // their historical three-peer budget remains unchanged.
    impl_->host = enet_host_create(&address, 4, Channels, 0, 0);
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
    // Recovery is performed by the fixed-step engine between polls.  Mirror
    // the resulting generation before consuming any packet so inputs from the
    // previous life cannot poison the authenticated peer queue.
    impl_->syncPeerGenerations(e);
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
                    hello.put<uint8_t>(e.playerSettings().enabled ? 1 : 0);
                    send(event.peer, 0, hello);
                } else
                    impl_->peers[event.peer] = {};
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                if (impl_->client) {
                    impl_->ready = false;
                    impl_->status = "Disconnected";
                    impl_->incoming.clear();
                    impl_->local = 0;
                    impl_->localGeneration = 1;
                    impl_->pendingInputs.clear();
                    impl_->playerFrames.clear();
                    impl_->lastPlayerTick = 0;
                    impl_->bodySequenceFloor = 0;
                    e.setPlayerStates({});
                    e.clearPrediction();
                } else if (auto it = impl_->peers.find(event.peer); it != impl_->peers.end()) {
                    if (it->second.player)
                        e.removePlayer(it->second.player);
                    impl_->peers.erase(it);
                }
                e.emit("player_disconnected");
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                Bytes b;
                b.b.assign(event.packet->data, event.packet->data + event.packet->dataLength);
                auto type = b.get<uint8_t>();
                if (!impl_->client) {
                    auto peerIt = impl_->peers.find(event.peer);
                    if (peerIt == impl_->peers.end())
                        throw std::runtime_error("Unknown peer");
                    auto &peer = peerIt->second;
                    if (type == Hello) {
                        if (peer.ready)
                            throw std::runtime_error("Handshake already completed");
                        if (event.peer->channelCount < Channels)
                            throw std::runtime_error("Protocol 4 requires three ENet channels");
                        if (b.get<uint32_t>() != Protocol)
                            throw std::runtime_error(
                                "Protocol 4 required: player replication is incompatible");
                        if (b.get<uint64_t>() != content(e))
                            throw std::runtime_error("Game content mismatch");
                        auto character = b.get<uint8_t>();
                        if (character > 1 || b.p != b.b.size())
                            throw std::runtime_error("Invalid character handshake");
                        if ((character != 0) != e.playerSettings().enabled)
                            throw std::runtime_error("Character handshake mismatch");
                        peer.character = character != 0;
                        if (!peer.character) {
                            size_t legacyPeers = 0;
                            for (const auto &[other, otherState] : impl_->peers)
                                legacyPeers += other != event.peer && otherState.ready &&
                                               !otherState.character;
                            if (legacyPeers >= 3)
                                throw std::runtime_error("Legacy peer capacity reached");
                        }
                        if (peer.character) {
                            peer.player = impl_->allocatePlayer(e);
                            e.addPlayer(peer.player);
                            peer.generation = e.players().at(peer.player).generation;
                        }
                        peer.ready = true;
                        Bytes welcome;
                        welcome.put<uint8_t>(Welcome);
                        welcome.put(peer.player);
                        welcome.put(peer.generation);
                        welcome.put<uint8_t>(peer.character ? 1 : 0);
                        welcome.put<uint64_t>(++impl_->rosterSequence);
                        putPlayerStates(welcome, e.players());
                        send(event.peer, 0, welcome);
                        impl_->state(event.peer, peer, e.world);
                        e.emit("player_connected");
                    } else if (!peer.ready)
                        throw std::runtime_error("Handshake required");
                    else if (type == Resync) {
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing resync bytes");
                        peer.quota(1);
                        peer.full = true;
                    } else if (type == Edit) {
                        if (peer.character)
                            throw std::runtime_error("Character tools require player input");
                        peer.quota(1);
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
                        c.player = peer.player ? peer.player : event.peer->incomingPeerID + 1;
                        e.submit(c);
                    } else if (type == PlayerInputs) {
                        if (event.channelID != 2 || !peer.character || !peer.player)
                            throw std::runtime_error("Player input from non-player peer");
                        auto n = b.get<uint32_t>();
                        if (n == 0 || n > MaxInputBatch)
                            throw std::runtime_error("Player input batch budget exceeded");
                        peer.inputQuota(n);
                        for (uint32_t i = 0; i < n; ++i) {
                            auto input = getInput(b);
                            if (input.generation < peer.generation)
                                continue;
                            if (input.generation != peer.generation)
                                throw std::runtime_error("Player input generation invalid");
                            if (!peer.lastInputSequence && input.sequence > MaxInputQueue)
                                throw std::runtime_error("Player input sequence invalid");
                            if (peer.lastInputSequence && input.sequence > peer.lastInputSequence &&
                                input.sequence - peer.lastInputSequence > MaxInputQueue)
                                throw std::runtime_error("Player input sequence jumped");
                            if (input.sequence <= peer.lastAppliedInput ||
                                peer.inputQueue.contains(input.sequence))
                                continue;
                            peer.lastInputSequence =
                                std::max(peer.lastInputSequence, input.sequence);
                            if (peer.inputQueue.size() >= MaxInputQueue)
                                peer.inputQueue.erase(peer.inputQueue.begin());
                            peer.inputQueue.emplace(input.sequence, input);
                            if (peer.lastAppliedInput &&
                                input.sequence > peer.lastAppliedInput + 1 &&
                                peer.gapStarted == std::chrono::steady_clock::time_point{})
                                peer.gapStarted = std::chrono::steady_clock::now();
                        }
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing player input bytes");
                    } else
                        throw std::runtime_error("Unexpected client packet");
                } else {
                    if (type == Welcome) {
                        auto assigned = b.get<PlayerId>();
                        auto generation = b.get<uint32_t>();
                        auto character = b.get<uint8_t>();
                        auto sequence = b.get<uint64_t>();
                        if (assigned > MaxPlayers || character > 1 || generation == 0 ||
                            sequence == 0)
                            throw std::runtime_error("Invalid player welcome");
                        auto states = getPlayerStates(b);
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing welcome bytes");
                        if ((character != 0) != e.playerSettings().enabled ||
                            (character != 0 && assigned == 0) || (character == 0 && assigned != 0))
                            throw std::runtime_error("Character mode mismatch");
                        impl_->local = assigned;
                        impl_->localGeneration = generation;
                        impl_->nextInput = 0;
                        impl_->pendingInputs.clear();
                        impl_->lastRosterSequence = std::max(impl_->lastRosterSequence, sequence);
                        impl_->recordPlayerFrames(states);
                        e.setPlayerStates(std::move(states));
                        e.clearPrediction();
                        impl_->status = "Synchronizing world";
                    } else if (type == PlayerRoster || type == RoundReset) {
                        if (event.channelID != 2)
                            throw std::runtime_error("Player roster on wrong channel");
                        auto sequence = b.get<uint64_t>();
                        if (sequence == 0 || sequence <= impl_->lastRosterSequence)
                            throw std::runtime_error("Player roster sequence invalid");
                        uint64_t bodyFloor = 0;
                        if (type == RoundReset) {
                            bodyFloor = b.get<uint64_t>();
                            if (!bodyFloor)
                                throw std::runtime_error("Invalid round body sequence");
                        }
                        auto states = getPlayerStates(b);
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing player roster bytes");
                        impl_->lastRosterSequence = sequence;
                        if (impl_->local) {
                            auto it = states.find(impl_->local);
                            if (it == states.end())
                                throw std::runtime_error("Local player missing from roster");
                            if (it->second.generation != impl_->localGeneration) {
                                impl_->localGeneration = it->second.generation;
                                impl_->nextInput = 0;
                                impl_->pendingInputs.clear();
                                e.clearPrediction();
                            }
                        }
                        if (type == RoundReset) {
                            impl_->pendingInputs.clear();
                            impl_->nextInput = 0;
                            impl_->lastPlayerTick = 0;
                            impl_->lastBodyTick = 0;
                            impl_->bodySequenceFloor =
                                std::max(impl_->bodySequenceFloor, bodyFloor);
                            impl_->targets.clear();
                            // Channel 0 owns the ordered Begin/Data assembler. A reset on
                            // channel 2 may arrive midway through either the old or new
                            // snapshot; the next Begin supersedes it without breaking offsets.
                            e.clearPrediction();
                        }
                        impl_->recordPlayerFrames(states);
                        e.setPlayerStates(std::move(states));
                    } else if (type == Begin) {
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
                        bool apply = impl_->ready && tick > impl_->lastBodyTick &&
                                     sequence >= impl_->bodySequenceFloor;
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
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing body snapshot bytes");
                    } else if (type == PlayerSnapshot) {
                        if (event.channelID != 2)
                            throw std::runtime_error("Player snapshot on wrong channel");
                        auto tick = b.get<uint64_t>();
                        auto states = getPlayerStates(b);
                        if (b.p != b.b.size())
                            throw std::runtime_error("Trailing player snapshot bytes");
                        // Reliable roster messages own membership and recovery generations.
                        // A late unreliable pose must not revive a departed player's generation,
                        // including when the slot has been re-used by a new connection.
                        bool stale = states.size() != e.players().size();
                        for (const auto &[id, state] : states) {
                            auto current = e.players().find(id);
                            if (current == e.players().end() ||
                                state.generation != current->second.generation)
                                stale = true;
                        }
                        if (!stale && impl_->ready &&
                            (!impl_->lastPlayerTick || tick > impl_->lastPlayerTick)) {
                            impl_->lastPlayerTick = tick;
                            if (impl_->local) {
                                auto it = states.find(impl_->local);
                                if (it != states.end()) {
                                    if (it->second.generation != impl_->localGeneration) {
                                        impl_->nextInput = 0;
                                        impl_->pendingInputs.clear();
                                        e.clearPrediction();
                                    }
                                    impl_->localGeneration = it->second.generation;
                                    while (!impl_->pendingInputs.empty() &&
                                           impl_->pendingInputs.front().sequence <=
                                               it->second.acknowledgedInput)
                                        impl_->pendingInputs.pop_front();
                                }
                            }
                            impl_->recordPlayerFrames(states);
                            e.setPlayerStates(std::move(states));
                        }
                    } else
                        throw std::runtime_error("Unexpected server packet");
                }
            }
        } catch (const std::exception &ex) {
            e.emit("network_error", 0, ex.what());
            if (impl_->client) {
                impl_->status = ex.what();
                impl_->ready = false;
                impl_->local = 0;
                impl_->localGeneration = 1;
                impl_->pendingInputs.clear();
                impl_->playerFrames.clear();
                impl_->lastPlayerTick = 0;
                impl_->bodySequenceFloor = 0;
                e.setPlayerStates({});
                e.clearPrediction();
            } else if (auto it = impl_->peers.find(event.peer); it != impl_->peers.end()) {
                if (it->second.player)
                    e.removePlayer(it->second.player);
                impl_->peers.erase(it);
            }
            enet_peer_disconnect_now(event.peer, 1);
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(event.packet);
    }
    if (!impl_->client) {
        const auto now = std::chrono::steady_clock::now();
        for (auto &[peer, state] : impl_->peers) {
            if (!state.ready || !state.character || !state.player)
                continue;
            while (!state.inputQueue.empty()) {
                auto first = state.inputQueue.begin();
                bool gap = first->first > state.lastAppliedInput &&
                           first->first - state.lastAppliedInput > 1;
                if (gap) {
                    if (state.gapStarted == std::chrono::steady_clock::time_point{})
                        state.gapStarted = now;
                    if (now - state.gapStarted < std::chrono::milliseconds(100))
                        break;
                    state.lastAppliedInput = first->first - 1;
                    state.gapStarted = {};
                } else {
                    state.gapStarted = {};
                }
                auto input = first->second;
                state.inputQueue.erase(first);
                if (input.sequence <= state.lastAppliedInput)
                    continue;
                state.lastAppliedInput = input.sequence;
                e.submitPlayerInput(state.player, input);
            }
        }
    }
    if (!impl_->client) {
        std::vector<ENetPeer *> failed;
        for (auto &[peer, state] : impl_->peers) {
            try {
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
            } catch (const std::exception &ex) {
                e.emit("network_error", 0, ex.what());
                failed.push_back(peer);
            }
        }
        for (auto *peer : failed) {
            if (auto it = impl_->peers.find(peer); it != impl_->peers.end() && it->second.player)
                e.removePlayer(it->second.player);
            impl_->peers.erase(peer);
            enet_peer_disconnect_now(peer, 1);
        }
    }
    if (!impl_->client && impl_->rosterChanged(e))
        impl_->broadcastRoster(e);
    if (impl_->client && impl_->ready && impl_->local && e.playerSettings().enabled) {
        auto now = std::chrono::steady_clock::now();
        if (!impl_->pendingInputs.empty() &&
            now - impl_->lastInputSend >= std::chrono::milliseconds(33)) {
            Bytes bytes;
            bytes.put<uint8_t>(PlayerInputs);
            // Keep one-shot actions in the retransmission set until the
            // authority acknowledges them.  A plain tail-of-eight would drop
            // an action after a burst of movement frames (about 133 ms at
            // 60 Hz).  Fill the remaining slots with the newest movement so
            // ordinary motion stays current under loss.
            std::vector<size_t> selected;
            selected.reserve(MaxInputBatch);
            for (size_t i = 0; i < impl_->pendingInputs.size() && selected.size() < MaxInputBatch;
                 ++i)
                if (inputFlags(impl_->pendingInputs[i]) &
                    (InputJump | InputDamage | InputBuild | InputRecover))
                    selected.push_back(i);
            for (size_t i = impl_->pendingInputs.size();
                 i-- > 0 && selected.size() < MaxInputBatch;)
                if (std::find(selected.begin(), selected.end(), i) == selected.end())
                    selected.push_back(i);
            std::sort(selected.begin(), selected.end());
            bytes.put<uint32_t>(selected.size());
            for (auto i : selected)
                putInput(bytes, impl_->pendingInputs[i]);
            send(impl_->server, 2, bytes, false);
            impl_->lastInputSend = now;
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
PlayerId Network::localPlayer() const {
    return impl_->local;
}
std::map<PlayerId, PlayerState> Network::presentedPlayers() const {
    return impl_->presentedPlayers();
}
void Network::sendPlayerInput(PlayerInput input) {
    if (!impl_->client || !impl_->ready || !impl_->local)
        return;
    if (input.sequence == 0 || input.generation != impl_->localGeneration ||
        input.sequence <= impl_->nextInput || impl_->pendingInputs.size() >= MaxInputQueue)
        return;
    try {
        validateInput(input);
    } catch (const std::exception &) {
        return;
    }
    impl_->nextInput = input.sequence;
    impl_->pendingInputs.push_back(input);
}
void Network::resetRound(Engine &e) {
    if (!impl_->host || impl_->client)
        return;
    // Capture the next generation before resetPlayers clears a roster during
    // project reload.  Existing states are advanced by resetPlayers itself;
    // absent remote states are re-created below with the same generation so
    // in-flight inputs from the previous round can never be accepted.
    std::map<PlayerId, uint32_t> nextGenerations;
    for (const auto &[peer, state] : impl_->peers) {
        (void)peer;
        if (!state.ready || !state.character || !state.player)
            continue;
        uint32_t previous = state.generation;
        if (auto it = e.players().find(state.player); it != e.players().end())
            previous = std::max(previous, it->second.generation);
        if (previous == std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Player generation exhausted");
        nextGenerations[state.player] = previous + 1;
    }
    e.resetPlayers();
    for (auto &[peer, state] : impl_->peers) {
        if (!state.ready || !state.character || !state.player)
            continue;
        if (!e.players().contains(state.player))
            e.addPlayer(state.player);
    }
    // resetPlayers increments states which survived a project reload.  For a
    // remote that was absent during loadProject, addPlayer created generation
    // one, so install the captured monotonic generations in one validated map.
    auto states = e.players();
    for (const auto &[peer, peerState] : impl_->peers) {
        (void)peer;
        if (!peerState.ready || !peerState.character || !peerState.player)
            continue;
        auto generationIt = nextGenerations.find(peerState.player);
        if (generationIt == nextGenerations.end())
            continue;
        auto generation = generationIt->second;
        auto it = states.find(peerState.player);
        if (it == states.end())
            continue;
        it->second.generation = generation;
        it->second.acknowledgedInput = 0;
        it->second.velocity = {};
        it->second.grounded = false;
        it->second.support = 0;
        it->second.spawnBlocked = true;
    }
    e.setPlayerStates(std::move(states));
    for (auto &[peer, state] : impl_->peers) {
        (void)peer;
        if (!state.ready || !state.character || !state.player)
            continue;
        auto generationIt = nextGenerations.find(state.player);
        if (generationIt == nextGenerations.end())
            continue;
        state.generation = generationIt->second;
        state.lastInputSequence = 0;
        state.lastAppliedInput = 0;
        state.inputQueue.clear();
        state.gapStarted = {};
        state.full = true;
        state.outgoing.clear();
        state.offset = 0;
        state.revisions.clear();
    }
    impl_->pendingInputs.clear();
    impl_->nextInput = 0;
    impl_->lastPlayerTick = 0;
    impl_->lastBroadcast = UINT64_MAX;
    impl_->forceBroadcast = true;
    impl_->rosterGenerations.clear();
    impl_->broadcastRoster(e, RoundReset);
}
void Network::broadcast(Engine &e) {
    if (!impl_->host || impl_->client)
        return;
    // A fixed-step recovery may have advanced a generation since the last
    // poll.  Synchronize the peer queues even on ticks where world snapshots
    // are throttled; the next packet then carries the new generation.
    impl_->syncPeerGenerations(e);
    if ((e.world.tick % 3 != 0 && !impl_->forceBroadcast) || impl_->lastBroadcast == e.world.tick)
        return;
    impl_->lastBroadcast = e.world.tick;
    std::vector<ENetPeer *> failed;
    for (auto &[peer, state] : impl_->peers) {
        if (!state.ready)
            continue;
        try {
            if (state.outgoing.empty()) {
                impl_->state(peer, state, e.world);
                Bytes b;
                b.put<uint8_t>(Bodies);
                b.put(e.world.tick);
                b.put(state.sequence);
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
            impl_->sendPlayerSnapshot(peer, e);
        } catch (const std::exception &ex) {
            e.emit("network_error", 0, ex.what());
            failed.push_back(peer);
        }
    }
    impl_->forceBroadcast = false;
    for (auto *peer : failed) {
        if (auto it = impl_->peers.find(peer); it != impl_->peers.end() && it->second.player)
            e.removePlayer(it->second.player);
        impl_->peers.erase(peer);
        enet_peer_disconnect_now(peer, 1);
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
