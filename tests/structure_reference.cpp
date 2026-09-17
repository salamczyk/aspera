#include "structure_reference.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <queue>

namespace aspera::test {
namespace {
Seam seam(IVec3 a, IVec3 b) {
    return a < b ? Seam{a, b} : Seam{b, a};
}
Vec3 cellCenter(IVec3 p, float s) {
    return {(p.x + .5f) * s, (p.y + .5f) * s, (p.z + .5f) * s};
}
int region(int p) {
    return p >= 0 ? p / 8 : (p - 7) / 8;
}
ReferenceStructureGraph build(const Object &o, const World &w) {
    ReferenceStructureGraph g;
    g.revision = o.revision;
    g.geometryKey = o.grid.contentKey();
    g.anchors = o.anchors;
    g.seams = o.seams;
    float s = w.voxelSize;
    for (auto [start, material] : o.grid.occupied()) {
        if (g.lookup.contains(start))
            continue;
        if (g.nodes.size() >= 10000)
            throw std::runtime_error("Structural node budget exceeded");
        size_t index = g.nodes.size();
        ReferenceStructuralNode node;
        node.material = material;
        std::queue<IVec3> q;
        q.push(start);
        g.lookup[start] = index;
        IVec3 tile{region(start.x), region(start.y), region(start.z)};
        while (!q.empty()) {
            auto p = q.front();
            q.pop();
            node.cells.push_back(p);
            node.center = node.center + cellCenter(p, s);
            node.anchored |= o.anchors.contains(p);
            for (auto d : Neighbors) {
                auto n = p + d;
                if (IVec3{region(n.x), region(n.y), region(n.z)} != tile ||
                    o.grid.get(n) != material || g.lookup.contains(n) ||
                    o.seams.contains(seam(p, n)))
                    continue;
                g.lookup[n] = index;
                q.push(n);
            }
        }
        node.center = node.center / float(node.cells.size());
        node.mass = w.materials.at(material).density * s * s * s * node.cells.size();
        g.nodes.push_back(std::move(node));
    }
    std::map<std::pair<size_t, size_t>, size_t> edges;
    for (auto &[p, index] : g.lookup)
        for (auto d : std::array<IVec3, 3>{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}) {
            auto n = p + d;
            auto it = g.lookup.find(n);
            if (it == g.lookup.end() || index == it->second || o.seams.contains(seam(p, n)))
                continue;
            auto pair = std::minmax(index, it->second);
            auto key = std::pair{pair.first, pair.second};
            if (!edges.contains(key)) {
                edges[key] = g.bonds.size();
                ReferenceStructuralBond b;
                b.a = key.first;
                b.b = key.second;
                g.bonds.push_back(b);
            }
            auto &b = g.bonds[edges[key]];
            b.faces.push_back(seam(p, n));
            b.area += s * s;
            b.center = b.center + (cellCenter(p, s) + cellCenter(n, s)) * .5f;
        }
    for (auto &b : g.bonds) {
        b.center = b.center / float(b.faces.size());
        b.normal = normalized(g.nodes[b.b].center - g.nodes[b.a].center);
    }
    return g;
}
} // namespace
void ReferenceStructure::validate(const Object &object, const World &world) const {
    (void)build(object, world);
}

void ReferenceStructure::split(Engine &e, EntityId id) {
    auto it = e.world.objects.find(id);
    if (it == e.world.objects.end())
        return;
    auto &original = it->second;
    std::set<IVec3> visited;
    std::vector<std::vector<IVec3>> groups;
    for (auto [start, material] : original.grid.occupied()) {
        if (visited.contains(start))
            continue;
        groups.emplace_back();
        std::queue<IVec3> q;
        q.push(start);
        visited.insert(start);
        while (!q.empty()) {
            auto p = q.front();
            q.pop();
            groups.back().push_back(p);
            for (auto d : Neighbors) {
                auto n = p + d;
                if (!original.grid.get(n) || visited.contains(n) ||
                    original.seams.contains(seam(p, n)))
                    continue;
                visited.insert(n);
                q.push(n);
            }
        }
    }
    if (groups.empty()) {
        e.world.objects.erase(id);
        std::erase_if(e.world.joints, [&](auto j) { return j.a == id || j.b == id; });
        graphs.erase(id);
        ++e.world.revision;
        return;
    }
    if (groups.size() == 1) {
        if (original.anchors.empty() && !original.dynamic) {
            original.dynamic = true;
            ++original.revision;
            ++e.world.revision;
        }
        return;
    }
    if (e.world.objects.size() + groups.size() - 1 > 4096) {
        e.emit("budget", id, "Fragment budget reached; split deferred");
        return;
    }
    std::stable_sort(groups.begin(), groups.end(), [&](auto &a, auto &b) {
        bool aa =
            std::any_of(a.begin(), a.end(), [&](auto p) { return original.anchors.contains(p); });
        bool ba =
            std::any_of(b.begin(), b.end(), [&](auto p) { return original.anchors.contains(p); });
        return aa != ba ? aa : a.size() > b.size();
    });
    Object source = original;
    std::map<IVec3, EntityId> ownership;
    Vec3 oldCenter{};
    float oldMass = 0;
    for (auto [p, m] : source.grid.occupied()) {
        float mass = e.world.materials[m].density;
        oldCenter = oldCenter + cellCenter(p, e.world.voxelSize) * mass;
        oldMass += mass;
    }
    if (oldMass > 0)
        oldCenter = oldCenter / oldMass;
    std::vector<Object> fragments;
    fragments.reserve(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        Object fragment;
        fragment.id = i ? e.world.nextEntity + i - 1 : id;
        fragment.name = i ? source.name + " fragment" : source.name;
        fragment.transform = source.transform;
        fragment.revision = source.revision + 1;
        fragment.angularVelocity = source.angularVelocity;
        Vec3 center{};
        float mass = 0;
        for (auto p : groups[i]) {
            auto mat = source.grid.get(p);
            fragment.grid.set(p, mat);
            ownership[p] = fragment.id;
            if (source.anchors.contains(p))
                fragment.anchors.insert(p);
            if (source.damage.contains(p))
                fragment.damage[p] = source.damage.at(p);
            float m = e.world.materials[mat].density;
            center = center + cellCenter(p, e.world.voxelSize) * m;
            mass += m;
        }
        for (auto s : source.seams)
            if (fragment.grid.get(s.a) && fragment.grid.get(s.b))
                fragment.seams.insert(s);
        fragment.dynamic = fragment.anchors.empty();
        fragment.velocity =
            source.velocity +
            cross(source.angularVelocity,
                  rotate(source.transform.rotation, mass > 0 ? center / mass - oldCenter : Vec3{}));
        fragments.push_back(std::move(fragment));
    }
    // Preflight every fragment before publishing any ownership or allocator changes.
    size_t chunks = 0;
    for (auto &[otherId, object] : e.world.objects)
        if (otherId != id)
            chunks += object.grid.chunks().size();
    for (const auto &fragment : fragments) {
        chunks += fragment.grid.chunks().size();
        (void)build(fragment, e.world);
        size_t boxes = 0;
        for (auto &[coordinate, chunk] : fragment.grid.chunks())
            boxes += collisionBoxesChunk(fragment.grid, coordinate, e.world.voxelSize).size();
        if (boxes > 32768)
            throw std::runtime_error("Fragment collision budget exceeded; split deferred");
    }
    if (chunks > 4096 ||
        e.world.nextEntity > std::numeric_limits<EntityId>::max() - (groups.size() - 1))
        throw std::runtime_error("Fragment world budget exceeded; split deferred");
    auto updatedJoints = e.world.joints;
    for (auto &j : updatedJoints) {
        if (j.a != id && j.b != id)
            continue;
        auto q = source.transform.rotation;
        auto local = rotate({-q.x, -q.y, -q.z, q.w}, j.anchor - source.transform.position);
        IVec3 p{int(std::floor(local.x / e.world.voxelSize)),
                int(std::floor(local.y / e.world.voxelSize)),
                int(std::floor(local.z / e.world.voxelSize))};
        auto owner = ownership.find(p);
        if (owner == ownership.end()) {
            if (j.a == id)
                j.a = 0;
            if (j.b == id)
                j.b = 0;
        } else {
            if (j.a == id)
                j.a = owner->second;
            if (j.b == id)
                j.b = owner->second;
        }
    }
    std::erase_if(updatedJoints, [](auto j) { return !j.a || !j.b || j.a == j.b; });
    for (auto &fragment : fragments)
        e.world.objects[fragment.id] = std::move(fragment);
    e.world.nextEntity += groups.size() - 1;
    e.world.joints = std::move(updatedJoints);
    graphs.erase(id);
    ++e.world.revision;
    e.emit("fracture", id, std::to_string(groups.size()) + " fragments");
}
void ReferenceStructure::update(Engine &e, float dt,
                                const std::map<EntityId, std::map<IVec3, Vec3>> &forces) {
    auto start = std::chrono::steady_clock::now();
    std::vector<EntityId> ids;
    for (auto &[id, o] : e.world.objects)
        ids.push_back(id);
    e.stats.structuralNodes = 0;
    e.stats.bonds = 0;
    for (auto id : ids) {
        auto it = e.world.objects.find(id);
        if (it == e.world.objects.end())
            continue;
        if (auto previous = graphs.find(id); previous != graphs.end()) {
            auto &g = previous->second;
            const auto &object = it->second;
            if (g.revision != object.revision && g.geometryKey == object.grid.contentKey() &&
                g.anchors == object.anchors && g.seams == object.seams)
                g.revision = object.revision;
        }
        bool rebuild = !graphs.contains(id) || graphs[id].revision != it->second.revision;
        if (rebuild) {
            split(e, id);
            it = e.world.objects.find(id);
            if (it == e.world.objects.end())
                continue;
            graphs[id] = build(it->second, e.world);
        }
        auto &o = it->second;
        auto &g = graphs[id];
        size_t n = g.nodes.size();
        e.stats.structuralNodes += uint32_t(n);
        e.stats.bonds += uint32_t(g.bonds.size());
        if (!n)
            continue;
        bool refreshBounds = rebuild || o.stressRegions.size() != n;
        o.stressRegions.resize(n);
        for (size_t i = 0; i < n; ++i) {
            auto &region = o.stressRegions[i];
            region.utilization = 0;
            if (!refreshBounds)
                continue;
            region.minimum = {INFINITY, INFINITY, INFINITY};
            region.maximum = {-INFINITY, -INFINITY, -INFINITY};
            for (auto cell : g.nodes[i].cells) {
                auto p = cellCenter(cell, e.world.voxelSize);
                float half = e.world.voxelSize * .5f;
                region.minimum = {std::min(region.minimum.x, p.x - half),
                                  std::min(region.minimum.y, p.y - half),
                                  std::min(region.minimum.z, p.z - half)};
                region.maximum = {std::max(region.maximum.x, p.x + half),
                                  std::max(region.maximum.y, p.y + half),
                                  std::max(region.maximum.z, p.z + half)};
            }
        }
        std::vector<std::vector<std::pair<size_t, size_t>>> adjacency(n);
        for (size_t k = 0; k < g.bonds.size(); ++k) {
            auto &b = g.bonds[k];
            adjacency[b.a].push_back({b.b, k});
            adjacency[b.b].push_back({b.a, k});
        }
        std::vector<int> distance(n, -1);
        std::queue<size_t> queue;
        for (size_t i = 0; i < n; ++i)
            if (g.nodes[i].anchored) {
                distance[i] = 0;
                queue.push(i);
            }
        bool free = queue.empty();
        auto fit = forces.find(id);
        if (free && (fit == forces.end() || fit->second.empty())) {
            o.stress = 0;
            continue;
        }
        // In a free body, solve relative to one node and remove centre-of-mass acceleration.
        if (free) {
            distance[0] = 0;
            queue.push(0);
        }
        while (!queue.empty()) {
            auto i = queue.front();
            queue.pop();
            for (auto [j, k] : adjacency[i])
                if (distance[j] < 0) {
                    distance[j] = distance[i] + 1;
                    queue.push(j);
                }
        }
        std::vector<Vec3> load(n), moment(n);
        Vec3 gravity = rotate({-o.transform.rotation.x, -o.transform.rotation.y,
                               -o.transform.rotation.z, o.transform.rotation.w},
                              {0, -9.81f, 0});
        float totalMass = 0;
        for (size_t i = 0; i < n; ++i) {
            totalMass += g.nodes[i].mass;
            if (!free)
                load[i] = gravity * g.nodes[i].mass;
        }
        Vec3 net{};
        if (fit != forces.end())
            for (auto [cell, force] : fit->second) {
                size_t closest = 0;
                float best = std::numeric_limits<float>::max();
                auto p = cellCenter(cell, e.world.voxelSize);
                for (size_t i = 0; i < n; ++i) {
                    float d = dot(g.nodes[i].center - p, g.nodes[i].center - p);
                    if (d < best) {
                        best = d;
                        closest = i;
                    }
                }
                load[closest] = load[closest] + force;
                moment[closest] = moment[closest] + cross(p - g.nodes[closest].center, force);
                net = net + force;
            }
        if (free && totalMass > 0)
            for (size_t i = 0; i < n; ++i)
                load[i] = load[i] - net * (g.nodes[i].mass / totalMass);
        std::vector<size_t> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return distance[a] > distance[b]; });
        o.stress = 0;
        std::vector<size_t> broken;
        // Bounded support-directed equilibrium: forces and bending moments divide among
        // support paths by contact stiffness. This is a gameplay load approximation.
        for (auto i : order) {
            if (distance[i] <= 0)
                continue;
            float weight = 0;
            for (auto [j, k] : adjacency[i])
                if (distance[j] >= 0 && distance[j] < distance[i])
                    weight +=
                        g.bonds[k].area /
                        std::max(e.world.voxelSize, length(g.nodes[i].center - g.nodes[j].center));
            if (weight == 0)
                continue;
            for (auto [j, k] : adjacency[i]) {
                if (distance[j] < 0 || distance[j] >= distance[i])
                    continue;
                auto &b = g.bonds[k];
                float share = (b.area / std::max(e.world.voxelSize,
                                                 length(g.nodes[i].center - g.nodes[j].center))) /
                              weight;
                auto force = load[i] * share;
                auto torque = (moment[i] + cross(g.nodes[i].center - b.center, load[i])) * share;
                load[j] = load[j] + force;
                moment[j] = moment[j] + torque + cross(b.center - g.nodes[j].center, force);
                auto normal = normalized(g.nodes[i].center - g.nodes[j].center);
                float normalForce = dot(force, normal);
                auto shearForce = force - normal * normalForce;
                auto &ma = e.world.materials[g.nodes[i].material];
                auto &mb = e.world.materials[g.nodes[j].material];
                float normalStrength = normalForce > 0 ? std::min(ma.tension, mb.tension)
                                                       : std::min(ma.compression, mb.compression);
                float ratio = std::abs(normalForce) / (b.area * normalStrength);
                ratio =
                    std::max(ratio, length(shearForce) / (b.area * std::min(ma.shear, mb.shear)));
                float section = std::max(1e-9f, b.area * std::sqrt(b.area) / 6.f);
                ratio =
                    std::max(ratio, length(torque) / (section * std::min(ma.bending, mb.bending)));
                o.stress = std::max(o.stress, ratio);
                o.stressRegions[i].utilization = std::max(o.stressRegions[i].utilization, ratio);
                o.stressRegions[j].utilization = std::max(o.stressRegions[j].utilization, ratio);
                if (ratio > 1)
                    b.damage += dt * (ratio - 1);
                else
                    b.damage = std::max(0.f, b.damage - dt * .1f);
                if (ratio >= 4 || b.damage >= .2f)
                    broken.push_back(k);
            }
        }
        if (!broken.empty()) {
            for (auto k : broken)
                for (auto s : g.bonds[k].faces)
                    o.seams.insert(s);
            ++o.revision;
            ++e.world.revision;
            split(e, id);
        }
    }
    std::erase_if(graphs, [&](auto &kv) { return !e.world.objects.contains(kv.first); });
    e.stats.structureMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace aspera::test
