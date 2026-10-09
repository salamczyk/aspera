#include "aspera/voxel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace aspera {
namespace {
constexpr size_t MaxFile = 256u * 1024u * 1024u;
constexpr uint32_t MaxModels = 4096, MaxVoxels = 16u * 1024u * 1024u, MaxObjects = 16384;
[[noreturn]] void bad(const char *message) {
    throw std::runtime_error(std::string("invalid voxel asset: ") + message);
}

struct Reader {
    const std::vector<uint8_t> &b;
    size_t p{}, end{};
    Reader(const std::vector<uint8_t> &bytes, size_t begin = 0,
           size_t finish = std::numeric_limits<size_t>::max())
        : b(bytes), p(begin), end(std::min(finish, bytes.size())) {}
    void need(size_t n) {
        if (n > end - p)
            bad("truncated data");
    }
    uint8_t u8() {
        need(1);
        return b[p++];
    }
    uint16_t u16() {
        need(2);
        auto n = uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
        p += 2;
        return n;
    }
    uint32_t u32() {
        need(4);
        uint32_t n = uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 |
                     uint32_t(b[p + 3]) << 24;
        p += 4;
        return n;
    }
    int32_t i32() {
        return static_cast<int32_t>(u32());
    }
    float f32() {
        uint32_t n = u32();
        return std::bit_cast<float>(n);
    }
    std::string str(size_t cap = 65536) {
        int32_t n = i32();
        if (n < 0 || static_cast<size_t>(n) > cap) {
            bad("invalid string length");
        }
        need(n);
        std::string s(reinterpret_cast<const char *>(b.data() + p), n);
        p += n;
        return s;
    }
    std::string id() {
        need(4);
        std::string s(reinterpret_cast<const char *>(b.data() + p), 4);
        p += 4;
        return s;
    }
};
struct Writer {
    std::ofstream out;
    explicit Writer(const std::filesystem::path &path)
        : out(path, std::ios::binary | std::ios::trunc) {
        if (!out)
            throw std::runtime_error("could not write voxel asset");
    }
    void bytes(const char *p, size_t n) {
        out.write(p, static_cast<std::streamsize>(n));
        if (!out)
            throw std::runtime_error("could not write voxel asset");
    }
    void u8(uint8_t n) {
        bytes(reinterpret_cast<const char *>(&n), 1);
    }
    void u16(uint16_t n) {
        u8(n);
        u8(n >> 8);
    }
    void u32(uint32_t n) {
        for (int i = 0; i < 4; ++i)
            u8(n >> (8 * i));
    }
    void i32(int32_t n) {
        u32(static_cast<uint32_t>(n));
    }
    void f32(float n) {
        u32(std::bit_cast<uint32_t>(n));
    }
    void str(const std::string &s) {
        if (s.size() > 65536)
            throw std::invalid_argument("asset string too long");
        i32(static_cast<int32_t>(s.size()));
        bytes(s.data(), s.size());
    }
};
using Dict = std::map<std::string, std::string>;
Dict dict(Reader &r) {
    int32_t n = r.i32();
    if (n < 0 || n > 4096)
        bad("invalid dictionary");
    Dict d;
    while (n--) {
        auto k = r.str();
        d.emplace(std::move(k), r.str());
    }
    return d;
}
int number(const Dict &d, const char *key, int fallback = 0) {
    auto it = d.find(key);
    if (it == d.end())
        return fallback;
    size_t pos = 0;
    long n;
    try {
        n = std::stol(it->second, &pos);
    } catch (...) {
        return fallback;
    }
    if (pos != it->second.size() || n < std::numeric_limits<int>::min() ||
        n > std::numeric_limits<int>::max())
        return fallback;
    return static_cast<int>(n);
}
std::array<int, 3> translation(const Dict &d) {
    auto it = d.find("_t");
    if (it == d.end())
        return {};
    std::array<int, 3> a{};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        while (pos < it->second.size() && it->second[pos] == ' ')
            ++pos;
        size_t e = pos;
        while (e < it->second.size() && it->second[e] != ' ')
            ++e;
        try {
            a[i] = std::stoi(it->second.substr(pos, e - pos));
        } catch (...) {
            bad("bad transform translation");
        }
        pos = e;
    }
    return a;
}
Quat multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat rotation(int r) {
    if (r < 0 || r > 255)
        return {};
    int i = r & 3, j = (r >> 2) & 3;
    if (i > 2 || j > 2 || i == j)
        return {};
    int k = 3 - i - j, si = (r >> 4) & 1 ? -1 : 1, sj = (r >> 5) & 1 ? -1 : 1,
        sk = (r >> 6) & 1 ? -1 : 1;
    // VOX encodes the column index and sign of each matrix ROW.
    float m[3][3]{};
    m[0][i] = float(si);
    m[1][j] = float(sj);
    m[2][k] = float(sk);
    float determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (determinant < 0)
        bad("reflected transforms are not supported");
    float trace = m[0][0] + m[1][1] + m[2][2];
    Quat q;
    if (trace > 0) {
        float s = std::sqrt(trace + 1.f) * 2;
        q.w = .25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = .25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = .25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = .25f * s;
    }
    return q;
}
struct Model {
    int x{}, y{}, z{};
    std::vector<std::array<uint8_t, 4>> voxels;
};
struct Node {
    enum Kind { Xform, Group, Shape } kind;
    int id{}, child{};
    std::vector<int> children, models;
    Dict attrs;
    Transform transform;
};
std::vector<uint8_t> readAll(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("could not read .vox file");
    auto n = f.tellg();
    if (n < 0 || static_cast<uintmax_t>(n) > MaxFile)
        bad("file too large");
    std::vector<uint8_t> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(b.data()), n);
    if (!f && n)
        bad("could not read file");
    return b;
}
Material material(uint32_t rgba, int i) {
    Material m;
    m.name = "palette-" + std::to_string(i);
    m.rgba = rgba;
    return m;
}
std::array<uint32_t, 256> defaultPalette() {
    // MagicaVoxel's documented 6x6x6 colour cube (without black), then ramps.
    std::array<uint32_t, 256> p{};
    size_t i = 0;
    for (int r = 255; r >= 0; r -= 51)
        for (int g = 255; g >= 0; g -= 51)
            for (int b = 255; b >= 0; b -= 51)
                if (r || g || b)
                    p[i++] = (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | 255;
    for (int channel = 0; channel < 4; ++channel)
        for (uint32_t c : {238, 221, 187, 170, 136, 119, 85, 68, 34, 17})
            p[i++] = (channel == 0   ? c << 24
                      : channel == 1 ? c << 16
                      : channel == 2 ? c << 8
                                     : (c << 24) | (c << 16) | (c << 8)) |
                     255;
    return p;
}
} // namespace

ImportedScene importVox(const std::filesystem::path &path, float voxelSize) {
    return importVox(readAll(path), voxelSize);
}

ImportedScene importVox(const std::vector<uint8_t> &bytes, float voxelSize) {
    if (!(voxelSize > 0.f) || !std::isfinite(voxelSize))
        throw std::invalid_argument("voxel size must be positive");
    Reader file(bytes);
    if (file.id() != "VOX ")
        bad("missing VOX signature");
    if (file.i32() < 150)
        bad("unsupported VOX version");
    if (file.id() != "MAIN")
        bad("missing MAIN chunk");
    int32_t own = file.i32(), kids = file.i32();
    if (own < 0 || kids < 0 ||
        static_cast<size_t>(own) + static_cast<size_t>(kids) > bytes.size() - file.p)
        bad("invalid MAIN chunk");
    size_t childStart = file.p + static_cast<size_t>(own),
           childEnd = childStart + static_cast<size_t>(kids);
    Reader r(bytes, childStart, childEnd);
    std::vector<Model> models;
    std::map<int, Node> nodes;
    auto palette = defaultPalette();
    std::array<int, 3> pendingSize{};
    bool haveSize = false;
    while (r.p < r.end) {
        std::string id = r.id();
        int32_t content = r.i32(), children = r.i32();
        if (content < 0 || children < 0 ||
            static_cast<size_t>(content) + static_cast<size_t>(children) > r.end - r.p)
            bad("bad chunk bounds");
        size_t ce = r.p + static_cast<size_t>(content);
        Reader c(bytes, r.p, ce);
        r.p = ce + static_cast<size_t>(children);
        bool parsed = false;
        if (id == "SIZE") {
            parsed = true;
            if (content != 12)
                bad("bad SIZE");
            pendingSize = {c.i32(), c.i32(), c.i32()};
            if (pendingSize[0] <= 0 || pendingSize[1] <= 0 || pendingSize[2] <= 0 ||
                pendingSize[0] > 4096 || pendingSize[1] > 4096 || pendingSize[2] > 4096)
                bad("SIZE out of range");
            haveSize = true;
        } else if (id == "XYZI") {
            parsed = true;
            if (!haveSize)
                bad("XYZI without SIZE");
            uint32_t n = c.u32();
            if (n > MaxVoxels || content != static_cast<int32_t>(4 + 4ull * n))
                bad("bad XYZI");
            Model m;
            m.x = pendingSize[0];
            m.y = pendingSize[1];
            m.z = pendingSize[2];
            m.voxels.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                auto x = c.u8(), y = c.u8(), z = c.u8(), color = c.u8();
                if (x >= m.x || y >= m.y || z >= m.z || !color)
                    bad("XYZI coordinate/color out of range");
                m.voxels.push_back({x, y, z, color});
            }
            if (models.size() >= MaxModels)
                bad("too many models");
            models.push_back(std::move(m));
            haveSize = false;
        } else if (id == "RGBA") {
            parsed = true;
            if (content != 1024)
                bad("bad RGBA");
            for (auto &color : palette) {
                uint8_t rr = c.u8(), gg = c.u8(), bb = c.u8(), aa = c.u8();
                color = (uint32_t(rr) << 24) | (uint32_t(gg) << 16) | (uint32_t(bb) << 8) | aa;
            }
        } else if (id == "nTRN") {
            parsed = true;
            Node n;
            n.kind = Node::Xform;
            n.id = c.i32();
            n.attrs = dict(c);
            n.child = c.i32();
            (void)c.i32();
            (void)c.i32();
            int32_t frames = c.i32();
            if (frames <= 0 || frames > 1024)
                bad("bad nTRN frames");
            Dict frame;
            for (int i = 0; i < frames; ++i) {
                auto f = dict(c);
                if (i == 0)
                    frame = std::move(f);
            }
            auto t = translation(frame);
            n.transform.position = {t[0] * voxelSize, t[1] * voxelSize, t[2] * voxelSize};
            n.transform.rotation = rotation(number(frame, "_r", 4));
            if (!nodes.emplace(n.id, std::move(n)).second)
                bad("duplicate node");
        } else if (id == "nGRP") {
            parsed = true;
            Node n;
            n.kind = Node::Group;
            n.id = c.i32();
            n.attrs = dict(c);
            int32_t count = c.i32();
            if (count < 0 || count > static_cast<int32_t>(MaxObjects))
                bad("bad nGRP children");
            n.children.reserve(count);
            while (count--)
                n.children.push_back(c.i32());
            if (!nodes.emplace(n.id, std::move(n)).second)
                bad("duplicate node");
        } else if (id == "nSHP") {
            parsed = true;
            Node n;
            n.kind = Node::Shape;
            n.id = c.i32();
            n.attrs = dict(c);
            int32_t count = c.i32();
            if (count < 0 || count > static_cast<int32_t>(MaxModels))
                bad("bad nSHP models");
            while (count--) {
                int idm = c.i32();
                (void)dict(c);
                n.models.push_back(idm);
            }
            if (!nodes.emplace(n.id, std::move(n)).second)
                bad("duplicate node");
        }
        if (parsed && c.p != c.end)
            bad("malformed chunk payload");
    }
    if (haveSize)
        bad("SIZE without XYZI");
    ImportedScene scene;
    scene.materials.reserve(256);
    scene.materials.push_back(material(0, 0));
    for (int i = 1; i < 256; ++i)
        scene.materials.push_back(material(palette[i - 1], i));
    size_t emittedCells = 0, emittedChunks = 0;
    auto addModel = [&](int mi, Transform transform, std::string name) {
        if (mi < 0 || static_cast<size_t>(mi) >= models.size())
            bad("shape references missing model");
        if (scene.objects.size() >= MaxObjects)
            bad("too many objects");
        VoxelAsset a;
        a.name = name.empty() ? "model-" + std::to_string(mi) : name;
        a.transform = transform;
        emittedCells += models[mi].voxels.size();
        if (emittedCells > 4000000)
            bad("instanced scene voxel budget exceeded");
        if (!nodes.empty()) {
            const auto &m = models[mi];
            a.transform.position =
                a.transform.position +
                rotate(transform.rotation,
                       {-m.x * voxelSize * .5f, -m.y * voxelSize * .5f, -m.z * voxelSize * .5f});
        }
        for (auto v : models[mi].voxels)
            a.grid.set({v[0], v[1], v[2]}, v[3]);
        a.grid.clearDirty();
        emittedChunks += a.grid.chunks().size();
        if (emittedChunks > 4096)
            bad("instanced scene chunk budget exceeded");
        // MagicaVoxel Z-up -> engine Y-up, preserving a right-handed basis.
        const Quat conversion{-std::sqrt(.5f), 0, 0, std::sqrt(.5f)};
        a.transform.position = rotate(conversion, a.transform.position);
        a.transform.rotation = multiply(conversion, a.transform.rotation);
        scene.objects.push_back(std::move(a));
    };
    if (nodes.empty()) {
        for (size_t i = 0; i < models.size(); ++i)
            addModel(static_cast<int>(i), {}, "model-" + std::to_string(i));
        return scene;
    }
    std::set<int> childIds;
    for (auto &[_, n] : nodes) {
        if (n.kind == Node::Xform)
            childIds.insert(n.child);
        for (int c : n.children)
            childIds.insert(c);
    }
    std::set<int> visiting, visited;
    // Instancing means a node may legitimately be expanded under several
    // transforms, so this is a total-work budget rather than a per-node visit
    // flag: it keeps a DAG's fan-out from growing exponentially in depth.
    size_t expansions = 0;
    std::function<void(int, Transform, const std::string &)> visit =
        [&](int id, Transform parent, const std::string &inherited) {
            auto it = nodes.find(id);
            if (it == nodes.end())
                bad("node references missing child");
            if (visiting.size() >= 256)
                bad("scene graph too deep");
            if (++expansions > 4u * MaxObjects)
                bad("scene graph expands to too many instances");
            if (!visiting.insert(id).second)
                bad("scene graph cycle");
            visited.insert(id);
            Node &n = it->second;
            std::string name = n.attrs.contains("_name") ? n.attrs.at("_name") : inherited;
            if (n.kind == Node::Xform) {
                Transform t;
                t.rotation = multiply(parent.rotation, n.transform.rotation);
                t.position = parent.position + rotate(parent.rotation, n.transform.position);
                visit(n.child, t, name);
            } else if (n.kind == Node::Group) {
                for (int c : n.children)
                    visit(c, parent, name);
            } else
                for (int mi : n.models)
                    addModel(mi, parent, name);
            visiting.erase(id);
        };
    for (auto &[id, _] : nodes)
        if (!childIds.contains(id))
            visit(id, {}, "");
    if (visited.size() != nodes.size())
        bad("unreachable scene graph cycle");
    if (scene.objects.empty())
        bad("scene has no roots");
    return scene;
}

void writeAsset(const ImportedScene &scene, const std::filesystem::path &path) {
    if (scene.materials.size() > 65535 || scene.objects.size() > MaxObjects)
        throw std::invalid_argument("asset collection exceeds limits");
    size_t totalCells = 0, totalChunks = 0;
    for (const auto &a : scene.objects) {
        totalCells += a.grid.count();
        totalChunks += a.grid.chunks().size();
        const auto &q = a.transform.rotation;
        const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (!std::isfinite(norm) || std::abs(norm - 1) > 0.01f)
            throw std::invalid_argument("asset rotation must be a unit quaternion");
    }
    // readAsset refuses beyond these, and an asset its own reader rejects is
    // indistinguishable from a corrupt file.
    if (totalCells > 4000000 || totalChunks > 4096)
        throw std::invalid_argument("asset exceeds the readable voxel/chunk budget");
    Writer w(path);
    w.bytes("ASPA", 4);
    w.u32(3);
    w.u32(static_cast<uint32_t>(scene.materials.size()));
    for (const auto &m : scene.materials) {
        if (m.name.empty() || m.name.size() > 4096 || m.density < 0 || m.resistance <= 0 ||
            m.friction < 0 || m.tension <= 0 || m.compression <= 0 || m.shear <= 0 ||
            m.bending <= 0)
            throw std::invalid_argument("invalid material physics settings");
        w.str(m.name);
        w.u32(m.rgba);
        for (float f :
             {m.density, m.resistance, m.friction, m.tension, m.compression, m.shear, m.bending}) {
            if (!std::isfinite(f))
                throw std::invalid_argument("non-finite material");
            w.f32(f);
        }
        if (!std::isfinite(m.impactToughness) || m.impactToughness <= 0)
            throw std::invalid_argument("invalid material impact toughness");
        w.f32(m.impactToughness);
        if (!std::isfinite(m.roughness) || !std::isfinite(m.metallic) ||
            !std::isfinite(m.emissive.x) || !std::isfinite(m.emissive.y) ||
            !std::isfinite(m.emissive.z) || !std::isfinite(m.emissionIntensity) ||
            m.roughness < 0 || m.roughness > 1 || m.metallic < 0 || m.metallic > 1 ||
            m.emissive.x < 0 || m.emissive.y < 0 || m.emissive.z < 0 || m.emissionIntensity < 0)
            throw std::invalid_argument("invalid material PBR settings");
        w.f32(m.roughness);
        w.f32(m.metallic);
        w.f32(m.emissive.x);
        w.f32(m.emissive.y);
        w.f32(m.emissive.z);
        w.f32(m.emissionIntensity);
    }
    w.u32(static_cast<uint32_t>(scene.objects.size()));
    for (const auto &a : scene.objects) {
        w.str(a.name);
        for (float f : {a.transform.position.x, a.transform.position.y, a.transform.position.z,
                        a.transform.rotation.x, a.transform.rotation.y, a.transform.rotation.z,
                        a.transform.rotation.w}) {
            if (!std::isfinite(f))
                throw std::invalid_argument("non-finite transform");
            w.f32(f);
        }
        auto cells = a.grid.occupied();
        if (cells.size() > MaxVoxels)
            throw std::invalid_argument("too many voxels");
        w.u32(static_cast<uint32_t>(cells.size()));
        for (auto [p, m] : cells) {
            if (m >= scene.materials.size())
                throw std::invalid_argument("voxel material is not in asset palette");
            w.i32(p.x);
            w.i32(p.y);
            w.i32(p.z);
            w.u16(m);
        }
    }
}

ImportedScene readAsset(const std::filesystem::path &path) {
    return readAsset(readAll(path));
}

ImportedScene readAsset(const std::vector<uint8_t> &b) {
    Reader r(b);
    if (r.id() != "ASPA")
        bad("unknown asset signature");
    auto version = r.u32();
    if (version != 1 && version != 2 && version != 3)
        bad("unsupported asset version");
    ImportedScene s;
    size_t totalChunks = 0, totalCells = 0;
    uint32_t nm = r.u32();
    if (nm > 65535)
        bad("too many materials");
    s.materials.reserve(nm);
    for (uint32_t i = 0; i < nm; ++i) {
        Material m;
        m.name = r.str();
        m.rgba = r.u32();
        float *fs[] = {&m.density,     &m.resistance, &m.friction, &m.tension,
                       &m.compression, &m.shear,      &m.bending};
        for (auto f : fs) {
            *f = r.f32();
            if (!std::isfinite(*f))
                bad("non-finite material");
        }
        if (m.name.empty() || m.density < 0 || m.resistance <= 0 || m.friction < 0 ||
            m.tension <= 0 || m.compression <= 0 || m.shear <= 0 || m.bending <= 0)
            bad("invalid material physics settings");
        m.impactToughness = version >= 2 ? r.f32() : defaultImpactToughness(m.name);
        if (!std::isfinite(m.impactToughness) || m.impactToughness <= 0)
            bad("invalid material impact toughness");
        if (version >= 3) {
            m.roughness = r.f32();
            m.metallic = r.f32();
            m.emissive = {r.f32(), r.f32(), r.f32()};
            m.emissionIntensity = r.f32();
        }
        if (!std::isfinite(m.roughness) || !std::isfinite(m.metallic) ||
            !std::isfinite(m.emissive.x) || !std::isfinite(m.emissive.y) ||
            !std::isfinite(m.emissive.z) || !std::isfinite(m.emissionIntensity) ||
            m.roughness < 0 || m.roughness > 1 || m.metallic < 0 || m.metallic > 1 ||
            m.emissive.x < 0 || m.emissive.y < 0 || m.emissive.z < 0 || m.emissionIntensity < 0)
            bad("invalid material PBR settings");
        s.materials.push_back(std::move(m));
    }
    uint32_t no = r.u32();
    if (no > MaxObjects)
        bad("too many objects");
    s.objects.reserve(no);
    for (uint32_t i = 0; i < no; ++i) {
        VoxelAsset a;
        a.name = r.str();
        float *fs[] = {&a.transform.position.x, &a.transform.position.y, &a.transform.position.z,
                       &a.transform.rotation.x, &a.transform.rotation.y, &a.transform.rotation.z,
                       &a.transform.rotation.w};
        for (auto f : fs) {
            *f = r.f32();
            if (!std::isfinite(*f))
                bad("non-finite transform");
        }
        uint32_t nv = r.u32();
        totalCells += nv;
        if (nv > MaxVoxels || totalCells > 4000000)
            bad("too many voxels");
        std::set<IVec3> seen;
        for (uint32_t j = 0; j < nv; ++j) {
            IVec3 p{r.i32(), r.i32(), r.i32()};
            if (std::abs(int64_t(p.x)) > 131072 || std::abs(int64_t(p.y)) > 131072 ||
                std::abs(int64_t(p.z)) > 131072)
                bad("cell coordinate out of range");
            MaterialId m = r.u16();
            if (!m || m >= s.materials.size() || !seen.insert(p).second)
                bad("invalid duplicate/empty voxel");
            auto before = a.grid.chunks().size();
            a.grid.set(p, m);
            totalChunks += a.grid.chunks().size() - before;
            if (totalChunks > 4096)
                bad("too many chunks");
        }
        a.grid.clearDirty();
        s.objects.push_back(std::move(a));
    }
    if (r.p != r.end)
        bad("trailing asset data");
    return s;
}
} // namespace aspera
