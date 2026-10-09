// A .vox file is fully attacker-controlled: it arrives from MagicaVoxel, an
// asset pipeline, or a downloaded model, and the editor imports it directly.
#include "aspera/voxel.hpp"
#include "fuzz.hpp"

#include <cstring>
#include <exception>

using namespace aspera;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);
    try {
        (void)importVox(bytes, 0.1f);
    } catch (const std::exception &) {
    }
    return 0;
}

namespace {
void put32(std::vector<uint8_t> &out, int32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(uint32_t(value) >> (i * 8)));
}
void chunk(std::vector<uint8_t> &out, const char *id, const std::vector<uint8_t> &content) {
    out.insert(out.end(), id, id + 4);
    put32(out, int32_t(content.size()));
    put32(out, 0);
    out.insert(out.end(), content.begin(), content.end());
}
std::vector<uint8_t> file(const std::vector<uint8_t> &body) {
    std::vector<uint8_t> out{'V', 'O', 'X', ' '};
    put32(out, 150);
    out.insert(out.end(), {'M', 'A', 'I', 'N'});
    put32(out, 0);
    put32(out, int32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
} // namespace

std::vector<std::vector<uint8_t>> asperaFuzzSeeds() {
    std::vector<std::vector<uint8_t>> seeds;

    // A plain model with no scene graph.
    std::vector<uint8_t> body, size, xyzi;
    put32(size, 2);
    put32(size, 2);
    put32(size, 2);
    chunk(body, "SIZE", size);
    put32(xyzi, 3);
    for (uint8_t cell : {0, 0, 0, 1, 1, 0, 0, 2, 0, 1, 1, 255})
        xyzi.push_back(cell);
    chunk(body, "XYZI", xyzi);
    seeds.push_back(file(body));

    // The same model with a palette, so RGBA parsing is covered.
    std::vector<uint8_t> palette;
    for (int i = 0; i < 256; ++i)
        for (uint8_t channel : {uint8_t(i), uint8_t(255 - i), uint8_t(i / 2), uint8_t(255)})
            palette.push_back(channel);
    auto withPalette = body;
    chunk(withPalette, "RGBA", palette);
    seeds.push_back(file(withPalette));

    // A transform -> group -> shape graph, the instancing path that hid the
    // exponential fan-out: a group whose child list repeats an entry.
    auto graph = body;
    std::vector<uint8_t> transform, group, shape;
    put32(transform, 0);
    put32(transform, 1); // one attribute: _t
    put32(transform, 2);
    transform.insert(transform.end(), {'_', 't'});
    const char *offset = "2 3 4";
    put32(transform, int32_t(std::strlen(offset)));
    transform.insert(transform.end(), offset, offset + std::strlen(offset));
    put32(transform, 1); // child
    put32(transform, -1);
    put32(transform, 0);
    put32(transform, 1); // one frame
    put32(transform, 0); // empty frame dictionary
    chunk(graph, "nTRN", transform);
    put32(group, 1);
    put32(group, 0);
    put32(group, 2);
    put32(group, 2);
    put32(group, 2);
    chunk(graph, "nGRP", group);
    put32(shape, 2);
    put32(shape, 0);
    put32(shape, 1);
    put32(shape, 0); // model 0
    put32(shape, 0); // empty model dictionary
    chunk(graph, "nSHP", shape);
    seeds.push_back(file(graph));

    return seeds;
}

size_t asperaFuzzPreservedPrefix() {
    return 12; // "VOX " + version + "MAIN"
}

void asperaFuzzRepair(std::vector<uint8_t> &bytes) {
    // Rewrite MAIN's own/children sizes to match the actual body, so mutations
    // inside the nested chunks are reached instead of dying on the outer bounds
    // check. MAIN's header is at 12: own size at 12, children size at 16.
    if (bytes.size() < 20)
        return;
    const uint32_t own = 0, children = uint32_t(bytes.size() - 20);
    std::memcpy(bytes.data() + 12, &own, sizeof(own));
    std::memcpy(bytes.data() + 16, &children, sizeof(children));
}
