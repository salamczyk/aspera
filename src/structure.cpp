#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <queue>
#include <tuple>

namespace aspera {
namespace {
using Clock = std::chrono::steady_clock;
struct Timer {
    double *value;
    double *nested{};
    double *other{};
    double otherBefore{other ? *other : 0};
    double before{nested ? *nested : 0};
    Clock::time_point start{Clock::now()};
    ~Timer() {
        if (value)
            *value += std::chrono::duration<double, std::milli>(Clock::now() - start).count() -
                      (nested ? *nested - before : 0) - (other ? *other - otherBefore : 0);
    }
};
constexpr uint16_t Empty = UINT16_MAX;
constexpr std::array<IVec3, 3> Positive{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
int region(int p) {
    return p >= 0 ? p / 8 : (p - 7) / 8;
}
IVec3 tileOf(IVec3 p) {
    return {region(p.x), region(p.y), region(p.z)};
}
IVec3 origin(IVec3 t) {
    return {t.x * 8, t.y * 8, t.z * 8};
}
size_t localIndex(IVec3 p) {
    auto t = origin(tileOf(p));
    return size_t(p.x - t.x + 8 * (p.y - t.y + 8 * (p.z - t.z)));
}
IVec3 position(IVec3 tile, size_t i) {
    return origin(tile) + IVec3{int(i % 8), int(i / 8 % 8), int(i / 64)};
}
auto cellOrder(IVec3 p) {
    auto l = localOf(p);
    return std::pair{chunkOf(p), l.x + 32 * (l.y + 32 * l.z)};
}
auto faceOrder(const Seam &s) {
    auto d = s.b - s.a;
    return std::pair{s.a, d.x ? 0 : d.y ? 1 : 2};
}
std::shared_ptr<const StructuralRegion> makeRegion(IVec3 tile, std::array<MaterialId, 512> cells,
                                                   const Object &o, float size) {
    auto r = std::make_shared<StructuralRegion>();
    r->cells = cells;
    r->labels.fill(Empty);
    for (size_t start = 0; start < 512; ++start) {
        if (!cells[start] || r->labels[start] != Empty)
            continue;
        uint16_t index = uint16_t(r->nodes.size());
        StructuralNode node;
        node.material = cells[start];
        node.minimum = {INFINITY, INFINITY, INFINITY};
        node.maximum = {-INFINITY, -INFINITY, -INFINITY};
        std::array<size_t, 512> queue;
        size_t head = 0, tail = 0;
        queue[tail++] = start;
        r->labels[start] = index;
        while (head < tail) {
            auto i = queue[head++];
            auto p = position(tile, i);
            node.cells.push_back(p);
            auto center = cellCenter(p, size);
            node.center = node.center + center;
            float h = size * .5f;
            node.minimum = {std::min(node.minimum.x, center.x - h),
                            std::min(node.minimum.y, center.y - h),
                            std::min(node.minimum.z, center.z - h)};
            node.maximum = {std::max(node.maximum.x, center.x + h),
                            std::max(node.maximum.y, center.y + h),
                            std::max(node.maximum.z, center.z + h)};
            for (auto d : Neighbors) {
                auto q = p + d;
                if (tileOf(q) != tile)
                    continue;
                auto j = localIndex(q);
                if (cells[j] != node.material || r->labels[j] != Empty ||
                    (!o.seams.empty() && o.seams.contains(seam(p, q))))
                    continue;
                r->labels[j] = index;
                queue[tail++] = j;
            }
        }
        node.center = node.center / float(node.cells.size());
        r->nodes.push_back(std::move(node));
    }
    return r;
}
void paths(StructureGraph &g) {
    size_t n = g.nodes.size();
    g.adjacency.assign(n, {});
    for (size_t k = 0; k < g.bonds.size(); ++k) {
        auto &b = g.bonds[k];
        g.adjacency[b.a].push_back({b.b, k});
        g.adjacency[b.b].push_back({b.a, k});
    }
    g.distance.assign(n, -1);
    std::vector<size_t> queue;
    for (size_t i = 0; i < n; ++i)
        if (g.nodes[i].anchored) {
            g.distance[i] = 0;
            queue.push_back(i);
        }
    g.free = queue.empty();
    if (g.free && n) {
        queue.push_back(0);
        g.distance[0] = 0;
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        auto i = queue[head];
        for (auto [j, k] : g.adjacency[i])
            if (g.distance[j] < 0) {
                g.distance[j] = g.distance[i] + 1;
                queue.push_back(j);
            }
    }
    g.order.resize(n);
    std::iota(g.order.begin(), g.order.end(), 0);
    std::stable_sort(g.order.begin(), g.order.end(),
                     [&](size_t a, size_t b) { return g.distance[a] > g.distance[b]; });
    g.load.resize(n);
    g.moment.resize(n);
}
size_t cacheBytes(const StructureGraph &g) {
    size_t bytes =
        sizeof(g) + (g.anchors.size() * sizeof(IVec3) + g.seams.size() * sizeof(Seam)) * 4;
    bytes +=
        g.nodes.capacity() * sizeof(StructuralNode) + g.bonds.capacity() * sizeof(StructuralBond);
    for (auto &n : g.nodes)
        bytes += n.cells.capacity() * sizeof(IVec3);
    for (auto &b : g.bonds)
        bytes += b.faces.capacity() * sizeof(Seam);
    for (auto &[t, r] : g.regions) {
        bytes += sizeof(StructuralRegion) + r->nodes.capacity() * sizeof(StructuralNode);
        for (auto &n : r->nodes)
            bytes += n.cells.capacity() * sizeof(IVec3);
    }
    for (auto &[t, c] : g.contacts)
        bytes += c->capacity() * sizeof(StructuralFace);
    for (auto &[t, n] : g.regionNodes)
        bytes += n.capacity() * sizeof(size_t);
    for (auto &a : g.adjacency)
        bytes += a.capacity() * sizeof(std::pair<size_t, size_t>);
    bytes += g.adjacency.capacity() * sizeof(decltype(g.adjacency)::value_type);
    bytes += g.order.capacity() * sizeof(size_t) + g.distance.capacity() * sizeof(int);
    bytes += (g.load.capacity() + g.moment.capacity()) * sizeof(Vec3);
    return bytes;
}

StructureGraph partitionGraph(const StructureGraph &source, const Object &object,
                              std::vector<size_t> component, Stats &stats) {
    Timer timing{&stats.structurePreparationMs};
    StructureGraph g;
    g.revision = object.revision;
    g.geometryKey = object.grid.contentKey();
    g.anchors = object.anchors;
    g.seams = object.seams;
    g.voxelSize = source.voxelSize;
    g.connectivityChecked = true;
    for (auto &[co, c] : object.grid.chunks())
        g.chunkKeys[co] = c.contentKey;
    std::sort(component.begin(), component.end());
    std::vector<size_t> remap(source.nodes.size(), SIZE_MAX);
    for (auto n : component) {
        remap[n] = g.nodes.size();
        g.nodes.push_back(source.nodes[n]);
        g.regionNodes.try_emplace(tileOf(source.nodes[n].cells.front()));
    }
    for (auto &[tile, global] : g.regionNodes) {
        auto &old = *source.regions.at(tile);
        const auto &indices = source.regionNodes.at(tile);
        bool complete = std::all_of(indices.begin(), indices.end(),
                                    [&](auto n) { return remap[n] != SIZE_MAX; });
        if (complete) {
            g.regions[tile] = source.regions.at(tile);
            for (auto n : indices)
                global.push_back(remap[n]);
            ++stats.structuralCacheHits;
        } else {
            auto r = std::make_shared<StructuralRegion>();
            r->labels.fill(Empty);
            std::vector<uint16_t> labels(indices.size(), Empty);
            for (size_t i = 0; i < indices.size(); ++i)
                if (remap[indices[i]] != SIZE_MAX) {
                    labels[i] = uint16_t(r->nodes.size());
                    r->nodes.push_back(old.nodes[i]);
                    global.push_back(remap[indices[i]]);
                }
            for (size_t i = 0; i < 512; ++i)
                if (old.labels[i] != Empty && labels[old.labels[i]] != Empty) {
                    r->cells[i] = old.cells[i];
                    r->labels[i] = labels[old.labels[i]];
                }
            stats.structuralCellsScanned += 512;
            ++stats.structuralRegionRebuilds;
            g.regions[tile] = std::move(r);
        }
    }
    // Component extraction cannot split a node or a surviving bond. Retain their
    // exact geometry and accumulation order, remapping only ownership indices.
    for (auto &bond : source.bonds)
        if (remap[bond.a] != SIZE_MAX && remap[bond.b] != SIZE_MAX) {
            auto b = bond;
            b.a = remap[bond.a];
            b.b = remap[bond.b];
            b.damage = 0;
            g.bonds.push_back(std::move(b));
        }
    for (auto &[tile, r] : g.regions) {
        auto cached = source.contacts.find(tile);
        if (cached == source.contacts.end())
            continue;
        bool complete = r == source.regions.at(tile);
        if (complete)
            for (auto &f : *cached->second)
                if (f.boundary) {
                    auto other = g.regions.find(tile + Positive[f.axis]);
                    if (other == g.regions.end() ||
                        other->second != source.regions.at(other->first)) {
                        complete = false;
                        break;
                    }
                }
        if (complete) {
            g.contacts[tile] = cached->second;
            continue;
        }
        auto faces = std::make_shared<std::vector<StructuralFace>>();
        for (auto f : *cached->second) {
            auto other = g.regions.find(f.boundary ? tile + Positive[f.axis] : tile);
            if (other == g.regions.end())
                continue;
            f.a = r->labels[localIndex(f.face.a)];
            f.b = other->second->labels[localIndex(f.face.b)];
            if (f.a != Empty && f.b != Empty)
                faces->push_back(f);
        }
        g.contacts[tile] = std::move(faces);
    }
    paths(g);
    return g;
}
} // namespace

std::optional<size_t> StructureGraph::nodeAt(IVec3 p) const {
    auto tile = tileOf(p);
    auto it = regions.find(tile);
    if (it == regions.end())
        return {};
    auto label = it->second->labels[localIndex(p)];
    if (label == Empty)
        return {};
    return regionNodes.at(tile)[label];
}

StructureGraph Structure::prepare(const Object &o, const World &w, const StructureGraph *previous,
                                  Stats *stats, bool componentPreflight) const {
    Timer timing{stats ? &stats->structurePreparationMs : nullptr};
    StructureGraph g;
    g.revision = o.revision;
    g.geometryKey = o.grid.contentKey();
    g.anchors = o.anchors;
    g.seams = o.seams;
    g.voxelSize = w.voxelSize;
    if (previous && previous->voxelSize == w.voxelSize) {
        g.regions = previous->regions;
        g.contacts = previous->contacts;
    } else
        previous = nullptr;
    std::set<IVec3> changed, inspect;
    for (auto &[co, chunk] : o.grid.chunks()) {
        g.chunkKeys[co] = chunk.contentKey;
        auto old = previous ? previous->chunkKeys.find(co) : g.chunkKeys.end();
        if (!previous || old == previous->chunkKeys.end() || old->second != chunk.contentKey)
            for (int z = 0; z < 4; ++z)
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        inspect.insert({co.x * 4 + x, co.y * 4 + y, co.z * 4 + z});
    }
    if (previous) {
        // Removed chunks have no cells to scan. Retire only their occupied regions.
        for (auto it = g.regions.begin(); it != g.regions.end();) {
            if (!g.chunkKeys.contains(chunkOf(origin(it->first)))) {
                changed.insert(it->first);
                it = g.regions.erase(it);
            } else
                ++it;
        }
        // Only internal seams affect node membership. Cross-region seams only affect bonds.
        auto mark = [&](const Seam &s) {
            auto a = tileOf(s.a), b = tileOf(s.b);
            if (a == b) {
                if (g.chunkKeys.contains(chunkOf(origin(a))))
                    inspect.insert(a);
                changed.insert(a);
            } else
                g.contacts.erase(a);
        };
        auto a = o.seams.begin(), b = previous->seams.begin();
        while (a != o.seams.end() || b != previous->seams.end()) {
            if (b == previous->seams.end() || (a != o.seams.end() && *a < *b))
                mark(*a++);
            else if (a == o.seams.end() || *b < *a)
                mark(*b++);
            else {
                ++a;
                ++b;
            }
        }
    }
    for (auto tile : inspect) {
        std::array<MaterialId, 512> cells{};
        auto base = origin(tile), co = chunkOf(base), local = localOf(base);
        auto chunk = o.grid.chunks().find(co);
        bool any = false;
        if (chunk != o.grid.chunks().end())
            for (size_t i = 0; i < 512; ++i) {
                cells[i] =
                    chunk->second.cells[local.x + i % 8 +
                                        32 * (local.y + i / 8 % 8 + 32 * (local.z + i / 64))];
                any |= cells[i] != 0;
            }
        if (stats)
            stats->structuralCellsScanned += 512;
        auto old = g.regions.find(tile);
        if (!changed.contains(tile) && old != g.regions.end() && old->second->cells == cells) {
            continue;
        }
        if (!any) {
            if (old != g.regions.end()) {
                g.regions.erase(old);
                changed.insert(tile);
            }
            continue;
        }
        g.regions[tile] = makeRegion(tile, cells, o, w.voxelSize);
        changed.insert(tile);
        if (stats)
            ++stats->structuralRegionRebuilds;
    }
    for (auto tile : changed) {
        g.contacts.erase(tile);
        for (auto d : Positive)
            g.contacts.erase(tile + IVec3{-d.x, -d.y, -d.z});
    }
    std::erase_if(g.contacts, [&](auto &entry) { return !g.regions.contains(entry.first); });
    struct NodeRef {
        IVec3 tile;
        size_t local;
        IVec3 first;
    };
    std::vector<NodeRef> nodes;
    for (auto &[tile, r] : g.regions) {
        g.regionNodes[tile].resize(r->nodes.size());
        for (size_t i = 0; i < r->nodes.size(); ++i)
            nodes.push_back({tile, i, r->nodes[i].cells.front()});
        if (stats && !changed.contains(tile))
            ++stats->structuralCacheHits;
    }
    // A loaded/edited source may contain several individually valid fragments.
    // Only simulation connectivity preflight may exceed the per-fragment limit;
    // it never solves or publishes an oversized component.
    if (nodes.size() > 10000 && !componentPreflight)
        throw std::runtime_error("Structural node budget exceeded");
    std::sort(nodes.begin(), nodes.end(),
              [](auto &a, auto &b) { return cellOrder(a.first) < cellOrder(b.first); });
    g.nodes.reserve(nodes.size());
    for (auto ref : nodes) {
        g.regionNodes[ref.tile][ref.local] = g.nodes.size();
        auto node = g.regions.at(ref.tile)->nodes[ref.local];
        node.mass = w.materials.at(node.material).density * w.voxelSize * w.voxelSize *
                    w.voxelSize * node.cells.size();
        g.nodes.push_back(std::move(node));
    }
    for (auto p : o.anchors)
        if (auto i = g.nodeAt(p))
            g.nodes[*i].anchored = true;
    for (auto &[tile, r] : g.regions) {
        if (g.contacts.contains(tile))
            continue;
        auto faces = std::make_shared<std::vector<StructuralFace>>();
        std::array<const StructuralRegion *, 3> neighbors{};
        for (size_t axis = 0; axis < 3; ++axis) {
            auto n = g.regions.find(tile + Positive[axis]);
            if (n != g.regions.end())
                neighbors[axis] = n->second.get();
        }
        for (size_t i = 0; i < 512; ++i) {
            auto a = r->labels[i];
            if (a == Empty)
                continue;
            auto p = position(tile, i);
            for (uint8_t axis = 0; axis < 3; ++axis) {
                auto q = p + Positive[axis];
                bool boundary = tileOf(q) != tile;
                auto other = boundary ? neighbors[axis] : r.get();
                if (!other)
                    continue;
                auto b = other->labels[localIndex(q)];
                if (b == Empty || (!boundary && a == b) ||
                    (!o.seams.empty() && o.seams.contains(seam(p, q))))
                    continue;
                faces->push_back({seam(p, q), a, b, axis, boundary});
            }
        }
        g.contacts[tile] = std::move(faces);
    }
    // A bond belongs to one contact tile (its positive-facing cell owner).
    // Reuse its already ordered/reduced geometry when those contacts did not
    // change. This avoids reducing every terrain boundary face after a local cut.
    std::set<IVec3> reused;
    if (previous) {
        for (auto &[tile, faces] : g.contacts) {
            auto old = previous->contacts.find(tile);
            if (old != previous->contacts.end() && old->second == faces)
                reused.insert(tile);
        }
        for (auto &old : previous->bonds)
            if (reused.contains(tileOf(old.faces.front().a))) {
                auto bond = old;
                bond.a = *g.nodeAt(previous->nodes[old.a].cells.front());
                bond.b = *g.nodeAt(previous->nodes[old.b].cells.front());
                if (bond.a > bond.b)
                    std::swap(bond.a, bond.b);
                bond.damage = 0;
                g.bonds.push_back(std::move(bond));
            }
    }
    const auto reusedBonds = g.bonds.size();
    // Aggregate changed faces, preserving legacy coordinate/axis ordering.
    std::map<std::pair<size_t, size_t>, size_t> edges;
    for (auto &[tile, faces] : g.contacts) {
        if (reused.contains(tile))
            continue;
        for (auto &f : *faces) {
            auto a = g.regionNodes.at(tile)[f.a];
            auto b = g.regionNodes.at(f.boundary ? tile + Positive[f.axis] : tile)[f.b];
            auto pair = std::minmax(a, b);
            auto [it, inserted] = edges.try_emplace({pair.first, pair.second}, g.bonds.size());
            if (inserted) {
                StructuralBond bond;
                bond.a = pair.first;
                bond.b = pair.second;
                g.bonds.push_back(std::move(bond));
            }
            g.bonds[it->second].faces.push_back(f.face);
        }
    }
    for (size_t i = reusedBonds; i < g.bonds.size(); ++i) {
        auto &bond = g.bonds[i];
        std::sort(bond.faces.begin(), bond.faces.end(),
                  [](auto &a, auto &b) { return faceOrder(a) < faceOrder(b); });
        for (auto &f : bond.faces) {
            bond.area += w.voxelSize * w.voxelSize;
            bond.center =
                bond.center + (cellCenter(f.a, w.voxelSize) + cellCenter(f.b, w.voxelSize)) * .5f;
        }
        bond.center = bond.center / float(bond.faces.size());
        bond.normal = normalized(g.nodes[bond.b].center - g.nodes[bond.a].center);
    }
    std::sort(g.bonds.begin(), g.bonds.end(), [](auto &a, auto &b) {
        return faceOrder(a.faces.front()) < faceOrder(b.faces.front());
    });
    paths(g);
    if (stats)
        stats->structuralCacheBytes = cacheBytes(g);
    return g;
}

void Structure::validate(const Object &o, const World &w) const {
    (void)prepare(o, w);
}
void Structure::materialsChanged(const World &w) {
    for (auto &[id, g] : graphs) {
        auto o = w.objects.find(id);
        if (o == w.objects.end())
            continue;
        for (auto &n : g.nodes)
            n.mass = w.materials.at(n.material).density * w.voxelSize * w.voxelSize * w.voxelSize *
                     n.cells.size();
        for (auto &b : g.bonds)
            b.damage = 0;
        // Legacy material invalidation resets fatigue and checks support again.
        g.connectivityChecked = false;
    }
}

void Structure::split(Engine &e, EntityId id) {
    auto found = e.world.objects.find(id);
    if (found == e.world.objects.end())
        return;
    auto &source = found->second;
    auto &g = graphs.at(id);
    struct Component {
        std::vector<size_t> nodes;
        size_t cells{};
        bool anchored{};
    };
    std::vector<Component> groups;
    std::vector<size_t> owner(g.nodes.size(), SIZE_MAX);
    {
        Timer timing{&e.stats.structureConnectivityMs};
        for (size_t i = 0; i < g.nodes.size(); ++i) {
            if (owner[i] != SIZE_MAX)
                continue;
            auto group = groups.size();
            groups.emplace_back();
            auto &c = groups.back();
            c.nodes.push_back(i);
            owner[i] = group;
            for (size_t head = 0; head < c.nodes.size(); ++head) {
                auto n = c.nodes[head];
                c.cells += g.nodes[n].cells.size();
                c.anchored |= g.nodes[n].anchored;
                for (auto [j, k] : g.adjacency[n])
                    if (owner[j] == SIZE_MAX) {
                        owner[j] = group;
                        c.nodes.push_back(j);
                    }
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
        if (source.anchors.empty() && !source.dynamic) {
            source.dynamic = true;
            ++source.revision;
            ++e.world.revision;
        }
        g.revision = source.revision;
        g.connectivityChecked = true;
        return;
    }
    Timer timing{&e.stats.structureFragmentMs, &e.stats.structurePreparationMs,
                 &e.stats.collisionPreparationMs};
    if (e.world.objects.size() + groups.size() - 1 > 4096) {
        g.connectivityChecked = true;
        e.emit("budget", id, "Fragment budget reached; split deferred");
        return;
    }
    if (e.world.nextEntity > std::numeric_limits<EntityId>::max() - (groups.size() - 1))
        throw std::runtime_error("Fragment world budget exceeded; split deferred");
    for (const auto &component : groups)
        if (component.nodes.size() > 10000)
            throw std::runtime_error("Structural node budget exceeded");
    std::stable_sort(groups.begin(), groups.end(), [](auto &a, auto &b) {
        return a.anchored != b.anchored ? a.anchored : a.cells > b.cells;
    });
    for (size_t i = 0; i < groups.size(); ++i)
        for (auto n : groups[i].nodes)
            owner[n] = i;
    std::vector<Object> fragments(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        auto &f = fragments[i];
        f.id = i ? e.world.nextEntity + i - 1 : id;
        f.name = i ? source.name + " fragment" : source.name;
        f.transform = source.transform;
        f.revision = source.revision + 1;
        f.angularVelocity = source.angularVelocity;
        f.velocity = source.velocity;
        f.dynamic = !groups[i].anchored;
    }
    // Partition whole chunks without per-cell insertion; only shared chunks need masks.
    std::map<IVec3, std::set<size_t>> chunkOwners;
    for (auto &[tile, indices] : g.regionNodes)
        for (auto n : indices)
            chunkOwners[chunkOf(origin(tile))].insert(owner[n]);
    for (auto &[co, owners] : chunkOwners) {
        const auto &chunk = source.grid.chunks().at(co);
        if (owners.size() == 1) {
            auto &grid = fragments[*owners.begin()].grid;
            grid.chunks_.emplace(co, chunk);
            grid.contentKey_ ^= chunk.contentKey;
            grid.dirty_.insert(co);
        } else {
            for (size_t i = 0; i < chunk.cells.size(); ++i)
                if (chunk.cells[i]) {
                    IVec3 p{co.x * 32 + int(i % 32), co.y * 32 + int(i / 32 % 32),
                            co.z * 32 + int(i / 1024)};
                    fragments[owner[*g.nodeAt(p)]].grid.set(p, chunk.cells[i]);
                }
        }
    }
    for (auto p : source.anchors)
        if (auto n = g.nodeAt(p))
            fragments[owner[*n]].anchors.insert(p);
    for (auto [p, d] : source.damage)
        if (auto n = g.nodeAt(p))
            fragments[owner[*n]].damage[p] = d;
    for (auto s : source.seams) {
        auto a = g.nodeAt(s.a), b = g.nodeAt(s.b);
        if (a && b && owner[*a] == owner[*b])
            fragments[owner[*a]].seams.insert(s);
    }
    // Only rotating bodies need fragment centres. Retain the reference cell BFS
    // accumulation order here to avoid altering angular-to-linear velocity transfer.
    if (dot(source.angularVelocity, source.angularVelocity) > 0) {
        Vec3 oldCenter{};
        float oldMass = 0;
        for (auto [p, m] : source.grid.occupied()) {
            float mass = e.world.materials[m].density;
            oldCenter = oldCenter + cellCenter(p, e.world.voxelSize) * mass;
            oldMass += mass;
        }
        if (oldMass > 0)
            oldCenter = oldCenter / oldMass;
        std::map<IVec3, std::array<bool, 512>> visited;
        for (size_t i = 0; i < groups.size(); ++i) {
            size_t first = *std::min_element(groups[i].nodes.begin(), groups[i].nodes.end());
            std::vector<IVec3> queue{g.nodes[first].cells.front()};
            visited[tileOf(queue[0])][localIndex(queue[0])] = true;
            Vec3 center{};
            float mass = 0;
            for (size_t head = 0; head < queue.size(); ++head) {
                auto p = queue[head];
                float m = e.world.materials[source.grid.get(p)].density;
                center = center + cellCenter(p, e.world.voxelSize) * m;
                mass += m;
                for (auto d : Neighbors) {
                    auto q = p + d;
                    auto n = g.nodeAt(q);
                    if (!n || owner[*n] != i || source.seams.contains(seam(p, q)))
                        continue;
                    auto &seen = visited[tileOf(q)][localIndex(q)];
                    if (!seen) {
                        seen = true;
                        queue.push_back(q);
                    }
                }
            }
            fragments[i].velocity =
                source.velocity + cross(source.angularVelocity,
                                        rotate(source.transform.rotation,
                                               mass > 0 ? center / mass - oldCenter : Vec3{}));
        }
    }
    std::vector<StructureGraph> prepared;
    std::vector<std::map<IVec3, std::vector<CollisionBox>>> collisions;
    size_t chunks = 0;
    for (auto &[other, object] : e.world.objects)
        if (other != id)
            chunks += object.grid.chunks().size();
    // Fragment validation and all allocations precede world/allocator publication.
    for (size_t i = 0; i < fragments.size(); ++i) {
        auto &f = fragments[i];
        chunks += f.grid.chunks().size();
        auto graph = partitionGraph(g, f, groups[i].nodes, e.stats);
        graph.connectivityChecked = true;
        prepared.push_back(std::move(graph));
        std::map<IVec3, std::vector<CollisionBox>> boxes;
        {
            Timer collisionTiming{&e.stats.collisionPreparationMs};
            if (physics)
                boxes = physics->prepareFragmentCollisions(id, f.grid, e.world.voxelSize);
            else
                for (auto &[co, chunk] : f.grid.chunks())
                    boxes[co] = collisionBoxesChunk(f.grid, co, e.world.voxelSize);
        }
        size_t count = 0;
        for (auto &[co, chunk] : boxes)
            count += chunk.size();
        if (count > 32768)
            throw std::runtime_error("Fragment collision budget exceeded; split deferred");
        collisions.push_back(std::move(boxes));
    }
    if (chunks > 4096)
        throw std::runtime_error("Fragment world budget exceeded; split deferred");
    auto joints = e.world.joints;
    for (auto &j : joints) {
        if (j.a != id && j.b != id)
            continue;
        auto q = source.transform.rotation;
        auto local = rotate({-q.x, -q.y, -q.z, q.w}, j.anchor - source.transform.position);
        IVec3 p{int(std::floor(local.x / e.world.voxelSize)),
                int(std::floor(local.y / e.world.voxelSize)),
                int(std::floor(local.z / e.world.voxelSize))};
        auto n = g.nodeAt(p);
        auto newId = n ? fragments[owner[*n]].id : EntityId{};
        if (j.a == id)
            j.a = newId;
        if (j.b == id)
            j.b = newId;
    }
    std::erase_if(joints, [](auto j) { return !j.a || !j.b || j.a == j.b; });
    // Preallocate map nodes so no allocation occurs while publishing world ownership.
    std::map<EntityId, Object> stagedObjects;
    std::map<EntityId, StructureGraph> stagedGraphs;
    std::vector<Physics::CollisionSeed> seeds;
    seeds.reserve(fragments.size());
    for (size_t i = 0; i < fragments.size(); ++i) {
        auto fid = fragments[i].id;
        stagedObjects.emplace(fid, std::move(fragments[i]));
        stagedGraphs.emplace(fid, std::move(prepared[i]));
        seeds.push_back({fid, &stagedObjects.at(fid).grid, &collisions[i]});
    }
    if (physics)
        physics->seedFragmentCollisionCaches(seeds, e.world.voxelSize);
    e.world.objects.erase(id);
    graphs.erase(id);
    e.world.objects.merge(stagedObjects);
    graphs.merge(stagedGraphs);
    e.world.nextEntity += groups.size() - 1;
    e.world.joints = std::move(joints);
    ++e.world.revision;
    e.emit("fracture", id, std::to_string(groups.size()) + " fragments");
}

void Structure::update(Engine &e, float dt,
                       const std::map<EntityId, std::map<IVec3, Vec3>> &forces) {
    auto start = Clock::now();
    e.stats.structurePreparationMs = e.stats.structureConnectivityMs = e.stats.structureSolveMs =
        e.stats.structureFragmentMs = 0;
    e.stats.structuralRegionRebuilds = e.stats.structuralCellsScanned =
        e.stats.structuralCacheHits = 0;
    e.stats.structuralNodes = e.stats.bonds = 0;
    std::vector<EntityId> ids;
    for (auto &[id, o] : e.world.objects)
        ids.push_back(id);
    for (auto id : ids) {
        auto it = e.world.objects.find(id);
        if (it == e.world.objects.end())
            continue;
        auto previous = graphs.find(id);
        bool rebuild = previous == graphs.end();
        if (!rebuild) {
            auto &g = previous->second;
            auto &o = it->second;
            rebuild = g.geometryKey != o.grid.contentKey() || g.voxelSize != e.world.voxelSize ||
                      g.anchors != o.anchors || g.seams != o.seams;
            if (!rebuild)
                g.revision = o.revision;
        }
        if (rebuild)
            graphs[id] =
                prepare(it->second, e.world, previous == graphs.end() ? nullptr : &previous->second,
                        &e.stats, true);
        if (!graphs[id].connectivityChecked) {
            try {
                split(e, id);
            } catch (...) {
                // Oversized source graphs exist only for connectivity preflight.
                // Failed validation must not retain one as a live cache.
                auto failed = graphs.find(id);
                if (failed != graphs.end() && failed->second.nodes.size() > 10000)
                    graphs.erase(failed);
                throw;
            }
            it = e.world.objects.find(id);
            if (it == e.world.objects.end())
                continue;
        }
        auto &o = it->second;
        auto &g = graphs.at(id);
        size_t n = g.nodes.size();
        if (n > 10000) {
            graphs.erase(id);
            throw std::runtime_error("Structural node budget exceeded");
        }
        e.stats.structuralNodes += uint32_t(n);
        e.stats.bonds += uint32_t(g.bonds.size());
        if (!n)
            continue;
        auto solveStart = Clock::now();
        o.stressRegions.resize(n);
        for (size_t i = 0; i < n; ++i) {
            o.stressRegions[i].minimum = g.nodes[i].minimum;
            o.stressRegions[i].maximum = g.nodes[i].maximum;
            o.stressRegions[i].utilization = 0;
        }
        auto &adjacency = g.adjacency;
        auto &distance = g.distance;
        auto &order = g.order;
        bool free = g.free;
        auto fit = forces.find(id);
        if (free && (fit == forces.end() || fit->second.empty())) {
            o.stress = 0;
            e.stats.structureSolveMs +=
                std::chrono::duration<double, std::milli>(Clock::now() - solveStart).count();
            continue;
        }
        auto &load = g.load;
        auto &moment = g.moment;
        std::fill(load.begin(), load.end(), Vec3{});
        std::fill(moment.begin(), moment.end(), Vec3{});
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

        e.stats.structureSolveMs +=
            std::chrono::duration<double, std::milli>(Clock::now() - solveStart).count();
        if (!broken.empty()) {
            for (auto k : broken)
                for (auto s : g.bonds[k].faces)
                    o.seams.insert(s);
            ++o.revision;
            ++e.world.revision;
            graphs[id] = prepare(o, e.world, &g, &e.stats, true);
            split(e, id);
        }
    }
    std::erase_if(graphs, [&](auto &kv) { return !e.world.objects.contains(kv.first); });
    e.stats.structuralCacheBytes = 0;
    for (auto &[id, g] : graphs)
        e.stats.structuralCacheBytes += cacheBytes(g);
    e.stats.structureMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
} // namespace aspera
