#include "aspera/engine.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace aspera {
namespace {
constexpr size_t MaxBytes = 128 * 1024 * 1024, MaxVoxels = 4000000;
struct Writer {
    std::vector<uint8_t> bytes;
    template <class T> void put(T v) {
        if constexpr (std::is_floating_point_v<T>)
            put(std::bit_cast<uint32_t>(v));
        else
            for (size_t i = 0; i < sizeof(T); ++i)
                bytes.push_back(uint8_t(uint64_t(v) >> (i * 8)));
        if (bytes.size() > MaxBytes)
            throw std::runtime_error("World exceeds 128 MiB snapshot budget");
    }
    void text(const std::string &s) {
        if (s.size() > 4096)
            throw std::runtime_error("String too long");
        put<uint32_t>(s.size());
        for (auto c : s)
            put<uint8_t>(c);
    }
    void vec(Vec3 v) {
        put(v.x);
        put(v.y);
        put(v.z);
    }
    void ivec(IVec3 v) {
        put<int32_t>(v.x);
        put<int32_t>(v.y);
        put<int32_t>(v.z);
    }
};
struct Reader {
    const std::vector<uint8_t> &b;
    size_t pos{}, voxels{}, chunks{};
    template <class T> T get() {
        if constexpr (std::is_floating_point_v<T>) {
            float f = std::bit_cast<float>(get<uint32_t>());
            if (!std::isfinite(f))
                throw std::runtime_error("Non-finite snapshot value");
            return f;
        } else {
            if (pos + sizeof(T) > b.size())
                throw std::runtime_error("Truncated snapshot");
            uint64_t x = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                x |= uint64_t(b[pos++]) << (8 * i);
            return T(x);
        }
    }
    uint32_t count(uint32_t limit) {
        auto n = get<uint32_t>();
        if (n > limit)
            throw std::runtime_error("Snapshot count exceeds limit");
        return n;
    }
    std::string text() {
        auto n = count(4096);
        if (pos + n > b.size())
            throw std::runtime_error("Truncated string");
        std::string s(b.begin() + pos, b.begin() + pos + n);
        pos += n;
        return s;
    }
    Vec3 vec() {
        return {get<float>(), get<float>(), get<float>()};
    }
    IVec3 ivec() {
        IVec3 p{get<int32_t>(), get<int32_t>(), get<int32_t>()};
        if (std::abs(int64_t(p.x)) > 131072 || std::abs(int64_t(p.y)) > 131072 ||
            std::abs(int64_t(p.z)) > 131072)
            throw std::runtime_error("Voxel coordinate out of range");
        return p;
    }
};
bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool validMaterial(const Material &m) {
    return !m.name.empty() && m.name.size() <= 4096 && std::isfinite(m.density) &&
           std::isfinite(m.resistance) && std::isfinite(m.friction) && std::isfinite(m.tension) &&
           std::isfinite(m.compression) && std::isfinite(m.shear) && std::isfinite(m.bending) &&
           std::isfinite(m.impactToughness) && std::isfinite(m.roughness) &&
           std::isfinite(m.metallic) && finite(m.emissive) && std::isfinite(m.emissionIntensity) &&
           m.density >= 0 && m.resistance > 0 && m.friction >= 0 && m.tension > 0 &&
           m.compression > 0 && m.shear > 0 && m.bending > 0 && m.impactToughness > 0 &&
           m.roughness >= 0 && m.roughness <= 1 && m.metallic >= 0 && m.metallic <= 1 &&
           m.emissive.x >= 0 && m.emissive.y >= 0 && m.emissive.z >= 0 && m.emissionIntensity >= 0;
}
bool validTransform(const Transform &t) {
    if (!finite(t.position) || !std::isfinite(t.rotation.x) || !std::isfinite(t.rotation.y) ||
        !std::isfinite(t.rotation.z) || !std::isfinite(t.rotation.w))
        return false;
    const auto &q = t.rotation;
    return std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) <= 0.01f;
}
// decodeWorld enforces these ceilings; a writer that ignores them produces files
// and snapshots its own reader rejects, which reads as silent data loss.
void checkReadable(const World &world) {
    if (world.materials.size() < 2 || world.materials.size() > 4096)
        throw std::runtime_error("World material count is not loadable");
    if (world.objects.size() > 4096)
        throw std::runtime_error("World object count is not loadable");
    if (world.joints.size() > 8192)
        throw std::runtime_error("World joint count is not loadable");
    if (world.properties.size() > 4096)
        throw std::runtime_error("World property count is not loadable");
    if (world.voxelSize < .01f || world.voxelSize > 10)
        throw std::runtime_error("World voxel size is not loadable");
    size_t voxels = 0;
    for (const auto &[id, o] : world.objects) {
        if (!id || id >= world.nextEntity)
            throw std::runtime_error("Object identifier is not loadable");
        if (!validTransform(o.transform))
            throw std::runtime_error("Object transform is not loadable");
        if (!finite(o.velocity) || !finite(o.angularVelocity) || !std::isfinite(o.stress))
            throw std::runtime_error("Object motion is not loadable");
        voxels += o.grid.count();
        if (voxels > MaxVoxels)
            throw std::runtime_error("World exceeds the loadable voxel budget");
        if (o.anchors.size() > o.grid.count() || o.damage.size() > o.grid.count())
            throw std::runtime_error("Object anchor/damage count is not loadable");
        for (const auto &[cell, amount] : o.damage)
            if (!(amount >= 0) || !std::isfinite(amount))
                throw std::runtime_error("Object damage is not loadable");
    }
}
bool validEnvironment(const EnvironmentSettings &v) {
    return finite(v.sunDirection) && length(v.sunDirection) > .0001f && finite(v.sunColor) &&
           finite(v.skyColor) && std::isfinite(v.sunIntensity) && std::isfinite(v.skyIntensity) &&
           std::isfinite(v.exposure) && std::isfinite(v.bloomStrength) && v.sunColor.x >= 0 &&
           v.sunColor.y >= 0 && v.sunColor.z >= 0 && v.skyColor.x >= 0 && v.skyColor.y >= 0 &&
           v.skyColor.z >= 0 && v.sunIntensity >= 0 && v.skyIntensity >= 0 && v.exposure >= -20 &&
           v.exposure <= 30 && v.bloomStrength >= 0 && v.bloomStrength <= 10;
}
bool validLight(const Light &v) {
    return (v.type == LightType::Point || v.type == LightType::Spot) && finite(v.position) &&
           finite(v.direction) && length(v.direction) > .0001f && finite(v.color) &&
           std::isfinite(v.intensity) && std::isfinite(v.range) && std::isfinite(v.innerAngle) &&
           std::isfinite(v.outerAngle) && v.color.x >= 0 && v.color.y >= 0 && v.color.z >= 0 &&
           v.intensity >= 0 && v.range > 0 && v.range <= 10000 && v.innerAngle >= 0 &&
           v.outerAngle >= v.innerAngle && v.outerAngle <= 89.9f &&
           std::abs(int64_t(v.shadowPriority)) <= 100000;
}
} // namespace
EntityId World::spawn(std::string name, VoxelGrid grid, Transform transform, bool dynamic) {
    if (objects.size() >= 4096)
        throw std::runtime_error("Object budget exceeded");
    if (!nextEntity || nextEntity == std::numeric_limits<EntityId>::max())
        throw std::runtime_error("Entity identifier space exhausted");
    auto id = nextEntity++;
    Object o;
    o.id = id;
    o.name = std::move(name);
    o.grid = std::move(grid);
    o.transform = transform;
    o.dynamic = dynamic;
    if (!objects.emplace(id, std::move(o)).second)
        throw std::runtime_error("Entity identifier already exists");
    ++revision;
    return id;
}
uint64_t World::checksum() const {
    uint64_t h = 1469598103934665603ull;
    for (auto b : encodeWorld(*this)) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
std::vector<uint8_t> encodeWorld(const World &world) {
    checkReadable(world);
    Writer w;
    w.put<uint32_t>(0x41535057);
    w.put<uint32_t>(3);
    w.put(world.voxelSize);
    w.put(world.tick);
    w.put(world.revision);
    w.put(world.nextEntity);
    w.put<uint32_t>(world.materials.size());
    for (auto &m : world.materials) {
        if (!validMaterial(m))
            throw std::runtime_error("Invalid material");
        w.text(m.name);
        w.put(m.rgba);
        for (auto f : {m.density, m.resistance, m.friction, m.tension, m.compression, m.shear,
                       m.bending, m.impactToughness})
            w.put(f);
        w.put(m.roughness);
        w.put(m.metallic);
        w.vec(m.emissive);
        w.put(m.emissionIntensity);
    }
    w.put<uint32_t>(world.objects.size());
    for (auto &[id, o] : world.objects) {
        w.put(id);
        w.text(o.name);
        w.put(o.revision);
        w.put<uint8_t>(o.dynamic);
        w.vec(o.transform.position);
        auto q = o.transform.rotation;
        w.put(q.x);
        w.put(q.y);
        w.put(q.z);
        w.put(q.w);
        w.vec(o.velocity);
        w.vec(o.angularVelocity);
        w.put(o.stress);
        auto cells = o.grid.occupied();
        w.put<uint32_t>(cells.size());
        for (auto [p, m] : cells) {
            w.ivec(p);
            w.put(m);
        }
        w.put<uint32_t>(o.anchors.size());
        for (auto p : o.anchors)
            w.ivec(p);
        w.put<uint32_t>(o.seams.size());
        for (auto s : o.seams) {
            w.ivec(s.a);
            w.ivec(s.b);
        }
        w.put<uint32_t>(o.damage.size());
        for (auto [p, d] : o.damage) {
            w.ivec(p);
            w.put(d);
        }
    }
    w.put<uint32_t>(world.joints.size());
    for (auto j : world.joints) {
        w.put(j.id);
        w.put(j.a);
        w.put(j.b);
        w.vec(j.anchor);
        w.put<uint8_t>(j.hinge);
        w.put(j.strength);
    }
    w.put<uint32_t>(world.properties.size());
    for (auto &[k, v] : world.properties) {
        w.text(k);
        w.put(std::bit_cast<uint64_t>(v));
    }
    if (!validEnvironment(world.environment) || world.nextLight == 0 ||
        world.nextLight == std::numeric_limits<LightId>::max() || world.lights.size() > 64)
        throw std::runtime_error("Invalid lighting settings");
    w.vec(world.environment.sunDirection);
    w.vec(world.environment.sunColor);
    w.vec(world.environment.skyColor);
    w.put(world.environment.sunIntensity);
    w.put(world.environment.skyIntensity);
    w.put(world.environment.exposure);
    w.put(world.environment.bloomStrength);
    w.put(world.nextLight);
    w.put<uint32_t>(world.lights.size());
    for (auto &[id, light] : world.lights) {
        if (id != light.id || id >= world.nextLight || !validLight(light))
            throw std::runtime_error("Invalid light");
        w.put(id);
        w.put<uint8_t>(uint8_t(light.type));
        w.vec(light.position);
        w.vec(light.direction);
        w.vec(light.color);
        w.put(light.intensity);
        w.put(light.range);
        w.put(light.innerAngle);
        w.put(light.outerAngle);
        w.put<uint8_t>(light.shadows);
        w.put(light.shadowPriority);
    }
    return std::move(w.bytes);
}
World decodeWorld(const std::vector<uint8_t> &bytes) {
    if (bytes.size() > MaxBytes)
        throw std::runtime_error("Snapshot exceeds limit");
    Reader r{bytes};
    if (r.get<uint32_t>() != 0x41535057)
        throw std::runtime_error("Unsupported world format");
    auto version = r.get<uint32_t>();
    if (version != 1 && version != 2 && version != 3)
        throw std::runtime_error("Unsupported world format");
    World w;
    w.voxelSize = r.get<float>();
    if (w.voxelSize < .01f || w.voxelSize > 10)
        throw std::runtime_error("Invalid voxel size");
    w.tick = r.get<uint64_t>();
    w.revision = r.get<uint64_t>();
    w.nextEntity = r.get<uint64_t>();
    if (!w.nextEntity || w.nextEntity == std::numeric_limits<EntityId>::max())
        throw std::runtime_error("Invalid entity allocator");
    auto nm = r.count(4096);
    if (nm < 2)
        throw std::runtime_error("Missing materials");
    for (uint32_t i = 0; i < nm; ++i) {
        Material m;
        m.name = r.text();
        m.rgba = r.get<uint32_t>();
        m.density = r.get<float>();
        m.resistance = r.get<float>();
        m.friction = r.get<float>();
        m.tension = r.get<float>();
        m.compression = r.get<float>();
        m.shear = r.get<float>();
        m.bending = r.get<float>();
        m.impactToughness = version >= 2 ? r.get<float>() : defaultImpactToughness(m.name);
        if (version >= 3) {
            m.roughness = r.get<float>();
            m.metallic = r.get<float>();
            m.emissive = r.vec();
            m.emissionIntensity = r.get<float>();
        }
        if (!validMaterial(m))
            throw std::runtime_error("Invalid material");
        w.materials.push_back(m);
    }
    auto no = r.count(4096);
    for (uint32_t i = 0; i < no; ++i) {
        Object o;
        o.id = r.get<uint64_t>();
        o.name = r.text();
        o.revision = r.get<uint64_t>();
        auto dyn = r.get<uint8_t>();
        if (dyn > 1)
            throw std::runtime_error("Invalid body flag");
        o.dynamic = dyn;
        o.transform.position = r.vec();
        o.transform.rotation = {r.get<float>(), r.get<float>(), r.get<float>(), r.get<float>()};
        auto q = o.transform.rotation;
        float qlen = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (std::abs(qlen - 1) > 0.01f)
            throw std::runtime_error("Invalid rotation");
        o.velocity = r.vec();
        o.angularVelocity = r.vec();
        o.stress = r.get<float>();
        auto n = r.count(MaxVoxels);
        r.voxels += n;
        if (r.voxels > MaxVoxels)
            throw std::runtime_error("Too many voxels");
        for (uint32_t j = 0; j < n; ++j) {
            auto p = r.ivec();
            auto m = r.get<MaterialId>();
            if (m == 0 || m >= nm || o.grid.get(p))
                throw std::runtime_error("Invalid voxel material or duplicate");
            auto before = o.grid.chunks().size();
            o.grid.set(p, m);
            r.chunks += o.grid.chunks().size() - before;
            if (r.chunks > 4096)
                throw std::runtime_error("Snapshot chunk budget exceeded");
        }
        auto na = r.count(n);
        for (uint32_t j = 0; j < na; ++j) {
            auto p = r.ivec();
            if (!o.grid.get(p) || !o.anchors.insert(p).second)
                throw std::runtime_error("Invalid anchor");
        }
        auto ns = r.count(n * 3);
        for (uint32_t j = 0; j < ns; ++j) {
            auto a = r.ivec(), b = r.ivec();
            auto d = b - a;
            if (!(a < b) || std::abs(d.x) + std::abs(d.y) + std::abs(d.z) != 1 || !o.grid.get(a) ||
                !o.grid.get(b) || !o.seams.insert({a, b}).second)
                throw std::runtime_error("Invalid seam");
        }
        auto nd = r.count(n);
        for (uint32_t j = 0; j < nd; ++j) {
            auto p = r.ivec();
            auto d = r.get<float>();
            if (!o.grid.get(p) || d < 0)
                throw std::runtime_error("Invalid damage");
            o.damage[p] = d;
        }
        if (!o.id || o.id >= w.nextEntity || !w.objects.emplace(o.id, std::move(o)).second)
            throw std::runtime_error("Invalid entity identifier");
    }
    auto nj = r.count(8192);
    std::set<uint64_t> ids;
    for (uint32_t i = 0; i < nj; ++i) {
        Joint j;
        j.id = r.get<uint64_t>();
        j.a = r.get<uint64_t>();
        j.b = r.get<uint64_t>();
        j.anchor = r.vec();
        j.hinge = r.get<uint8_t>() != 0;
        j.strength = r.get<float>();
        if (!j.id || !ids.insert(j.id).second || !w.objects.contains(j.a) ||
            !w.objects.contains(j.b) || j.a == j.b || j.strength <= 0)
            throw std::runtime_error("Invalid joint");
        w.joints.push_back(j);
    }
    auto np = r.count(4096);
    for (uint32_t i = 0; i < np; ++i) {
        auto key = r.text();
        auto value = std::bit_cast<double>(r.get<uint64_t>());
        if (!std::isfinite(value) || !w.properties.emplace(key, value).second)
            throw std::runtime_error("Invalid property");
    }
    if (version >= 3) {
        w.environment.sunDirection = r.vec();
        w.environment.sunColor = r.vec();
        w.environment.skyColor = r.vec();
        w.environment.sunIntensity = r.get<float>();
        w.environment.skyIntensity = r.get<float>();
        w.environment.exposure = r.get<float>();
        w.environment.bloomStrength = r.get<float>();
        w.nextLight = r.get<LightId>();
        if (!validEnvironment(w.environment) || w.nextLight == 0 ||
            w.nextLight == std::numeric_limits<LightId>::max())
            throw std::runtime_error("Invalid lighting settings");
        auto nl = r.count(64);
        for (uint32_t i = 0; i < nl; ++i) {
            Light light;
            light.id = r.get<LightId>();
            auto type = r.get<uint8_t>();
            if (type > uint8_t(LightType::Spot))
                throw std::runtime_error("Invalid light type");
            light.type = LightType(type);
            light.position = r.vec();
            light.direction = r.vec();
            light.color = r.vec();
            light.intensity = r.get<float>();
            light.range = r.get<float>();
            light.innerAngle = r.get<float>();
            light.outerAngle = r.get<float>();
            auto shadows = r.get<uint8_t>();
            if (shadows > 1)
                throw std::runtime_error("Invalid shadow flag");
            light.shadows = shadows;
            light.shadowPriority = r.get<int32_t>();
            if (!light.id || light.id >= w.nextLight || !validLight(light) ||
                !w.lights.emplace(light.id, light).second)
                throw std::runtime_error("Invalid light");
        }
    }
    if (r.pos != bytes.size())
        throw std::runtime_error("Trailing snapshot data");
    return w;
}
void saveWorld(const World &w, const std::filesystem::path &path) {
    auto bytes = encodeWorld(w);
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        f.flush();
        if (!f)
            throw std::runtime_error("Failed to write save");
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Failed to replace save");
#else
    std::filesystem::rename(temp, path);
#endif
}
World loadWorld(const std::filesystem::path &path) {
    auto n = std::filesystem::file_size(path);
    if (n > MaxBytes)
        throw std::runtime_error("Save too large");
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b(n);
    f.read(reinterpret_cast<char *>(b.data()), n);
    if (!f)
        throw std::runtime_error("Failed to read save");
    return decodeWorld(b);
}
std::optional<SceneHit> raycastWorld(const World &w, Vec3 origin, Vec3 direction,
                                     float maxDistance) {
    std::optional<SceneHit> best;
    for (auto &[id, o] : w.objects) {
        auto q = o.transform.rotation;
        Quat inverse{-q.x, -q.y, -q.z, q.w};
        auto hit = raycast(o.grid, w.voxelSize, rotate(inverse, origin - o.transform.position),
                           rotate(inverse, direction), maxDistance);
        if (hit && (!best || hit->distance < best->hit.distance))
            best = SceneHit{id, *hit, origin + normalized(direction) * hit->distance};
    }
    return best;
}
} // namespace aspera
