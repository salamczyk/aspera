#include "internal.hpp"
#include <bit>
#include <stdexcept>

namespace aspera {
uint64_t playerCollisionRevision(const World &w) {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(std::bit_cast<uint32_t>(w.voxelSize));
    for (const auto &[id, o] : w.objects) {
        mix(id);
        mix(o.grid.contentKey());
        mix(o.dynamic);
        if (!o.dynamic) {
            for (float f : {o.transform.position.x, o.transform.position.y, o.transform.position.z,
                            o.transform.rotation.x, o.transform.rotation.y, o.transform.rotation.z,
                            o.transform.rotation.w})
                mix(std::bit_cast<uint32_t>(f));
        }
    }
    return hash;
}
bool validPlayerInput(const PlayerInput &i) {
    return i.sequence && i.generation && std::isfinite(i.moveX) && std::isfinite(i.moveZ) &&
           std::isfinite(i.yaw) && std::isfinite(i.pitch) && std::abs(i.yaw) <= 10000 &&
           std::abs(i.pitch) <= 1.45f && i.moveX * i.moveX + i.moveZ * i.moveZ <= 1.001f;
}
bool PlayerSession::submit(PlayerId id, PlayerInput input) {
    auto state = states.find(id);
    if (state == states.end() || !validPlayerInput(input) ||
        input.generation != state->second.generation)
        return false;
    auto &queue = inputs[id];
    if (input.sequence <= queue.received || input.sequence - queue.received > 240 ||
        queue.frames.size() >= 120)
        return false;
    queue.received = input.sequence;
    queue.frames.push_back(input);
    return true;
}
void PlayerSession::recover(PlayerId id) {
    auto &state = states.at(id);
    ++state.generation;
    state.acknowledgedInput = 0;
    state.velocity = {};
    state.grounded = false;
    state.support = 0;
    state.spawnBlocked = true;
    inputs.erase(id);
    recovering.insert(id);
}
void PlayerSession::clearPrediction() {
    prediction.reset();
    predicted.reset();
    history.clear();
    reconciled = 0;
}
void PlayerSession::reset() {
    inputs.clear();
    clearPrediction();
    for (auto &[id, state] : states) {
        (void)state;
        recover(id);
    }
}
void PlayerSession::tick(Engine &engine, Physics &physics, float dt) {
    const auto &settings = engine.playerSettings();
    if (!settings.enabled)
        return;
    for (auto &[id, state] : states) {
        state.collisionRevision = physics.collisionRevision();
        auto &queue = inputs[id];
        PlayerInput input = queue.last;
        input.jump = input.damage = input.build = input.recover = false;
        if (!queue.frames.empty()) {
            input = queue.frames.front();
            queue.frames.pop_front();
            queue.last = input;
            queue.silence = 0;
            state.acknowledgedInput = input.sequence;
        } else {
            queue.silence += dt;
            if (queue.silence > .1f)
                input.moveX = input.moveZ = 0;
        }
        const auto out = [&](Vec3 v) {
            return v.y < settings.minimum.y || v.x < settings.minimum.x ||
                   v.z < settings.minimum.z || v.y > settings.maximum.y ||
                   v.x > settings.maximum.x || v.z > settings.maximum.z;
        };
        if ((input.recover || out(state.position)) && !recovering.contains(id)) {
            recover(id);
            physics.forgetPlayer(id);
            input = {};
        }
        if (recovering.contains(id)) {
            physics.forgetPlayer(id);
            bool found = false;
            for (size_t n = 0; n < settings.spawns.size() && !found; ++n) {
                const auto &spawn = settings.spawns[(n + id - 1) % settings.spawns.size()];
                for (Vec3 offset :
                     {Vec3{}, Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
                    Vec3 candidate = spawn.position + offset;
                    if (!out(candidate) && physics.playerClear(candidate)) {
                        state.position = candidate;
                        state.yaw = spawn.yaw;
                        state.pitch = spawn.pitch;
                        state.velocity = {};
                        state.spawnBlocked = false;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                state.spawnBlocked = true;
                continue;
            }
            recovering.erase(id);
            input = {};
            input.yaw = state.yaw;
            input.pitch = state.pitch;
            inputs[id].last = input;
        }
        physics.movePlayer(state, input, dt);
        if (!(input.damage || input.build))
            continue;
        const Vec3 eye = state.position + Vec3{0, 1.65f, 0};
        const Vec3 direction{std::sin(state.yaw) * std::cos(state.pitch), std::sin(state.pitch),
                             std::cos(state.yaw) * std::cos(state.pitch)};
        const auto hit = raycastWorld(engine.world, eye, direction, 4);
        if (!hit) {
            engine.emit("tool_rejected", 0, "No surface within 4 metres");
            continue;
        }
        Command command;
        command.player = id;
        command.entity = hit->entity;
        command.expectedRevision = engine.world.objects.at(hit->entity).revision;
        command.point = hit->worldPoint;
        if (input.damage) {
            command.type = CommandType::SphereDamage;
            command.radius = .45f;
            command.strength = 1;
        } else {
            const auto &object = engine.world.objects.at(hit->entity);
            if (object.dynamic) {
                engine.emit("tool_rejected", hit->entity, "Build on static surfaces");
                continue;
            }
            command.type = CommandType::SpawnPrefab;
            command.radius = .4f;
            command.material = settings.buildMaterial;
            command.cell = hit->hit.cell + hit->hit.normal;
            command.vector = {float(hit->hit.normal.x), float(hit->hit.normal.y),
                              float(hit->hit.normal.z)};
            // Match the prefab placement rule; keep the full cube out of all character capsules.
            int size = std::clamp(int(command.radius / engine.world.voxelSize), 1, 16);
            IVec3 start = command.cell;
            if (command.vector.x < 0)
                start.x -= size - 1;
            if (command.vector.y < 0)
                start.y -= size - 1;
            if (command.vector.z < 0)
                start.z -= size - 1;
            bool blocked = false;
            for (const auto &[otherId, other] : states) {
                (void)otherId;
                if (other.spawnBlocked)
                    continue;
                auto q = object.transform.rotation;
                auto local = rotate({-q.x, -q.y, -q.z, q.w},
                                    other.position + Vec3{0, .9f, 0} - object.transform.position);
                Vec3 minimum{start.x * engine.world.voxelSize, start.y * engine.world.voxelSize,
                             start.z * engine.world.voxelSize};
                Vec3 maximum =
                    minimum + Vec3{float(size), float(size), float(size)} * engine.world.voxelSize;
                // Conservative bounding sphere also covers rotated surfaces.
                Vec3 nearest{std::clamp(local.x, minimum.x, maximum.x),
                             std::clamp(local.y, minimum.y, maximum.y),
                             std::clamp(local.z, minimum.z, maximum.z)};
                if (length(local - nearest) < .95f)
                    blocked = true;
            }
            if (blocked) {
                engine.emit("tool_rejected", hit->entity, "Player blocks construction");
                continue;
            }
        }
        engine.submit(command);
    }
}
void PlayerSession::predict(Engine &engine, PlayerId id, PlayerInput input, float dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > .1f || !validPlayerInput(input))
        return;
    auto found = states.find(id);
    if (found == states.end()) {
        clearPrediction();
        return;
    }
    const auto &authority = found->second;
    if (!prediction) {
        prediction = std::make_unique<Physics>();
        prediction->setPrediction(true);
    }
    prediction->sync(engine.world);
    bool reset = !predicted || predicted->id != id ||
                 predicted->generation != authority.generation ||
                 predicted->spawnBlocked != authority.spawnBlocked || history.size() >= 120 ||
                 authority.collisionRevision != prediction->collisionRevision();
    if (reset) {
        predicted = authority;
        history.clear();
        reconciled = authority.acknowledgedInput;
        prediction->restorePlayer(*predicted);
    } else if (authority.acknowledgedInput > reconciled) {
        if (!history.empty() && authority.acknowledgedInput < history.front().sequence - 1)
            history.clear();
        while (!history.empty() && history.front().sequence <= authority.acknowledgedInput)
            history.pop_front();
        predicted = authority;
        prediction->restorePlayer(*predicted);
        for (const auto &frame : history)
            prediction->movePlayer(*predicted, frame, 1.f / 60.f);
        reconciled = authority.acknowledgedInput;
    }
    if (input.generation != authority.generation || authority.spawnBlocked ||
        authority.collisionRevision != prediction->collisionRevision())
        return;
    if (!history.empty() && input.sequence <= history.back().sequence)
        return;
    history.push_back(input);
    prediction->movePlayer(*predicted, input, dt);
    predicted->acknowledgedInput = input.sequence;
}
} // namespace aspera
