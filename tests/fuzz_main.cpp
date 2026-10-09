// Portable driver for the fuzz targets, used when libFuzzer is unavailable.
//
// It is deliberately not a coverage-guided fuzzer. It replays a corpus and a
// fixed, seeded mutation sweep so the same inputs run on every machine and in
// CI: a failure here is always reproducible from the printed case index. Build
// with -DASPERA_FUZZ=ON on a toolchain that has libFuzzer to get the real thing.
#include "fuzz.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
// SplitMix64: a fixed generator so the mutation sweep is identical everywhere.
struct Random {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    size_t below(size_t n) {
        return n ? size_t(next() % n) : 0;
    }
};

void mutate(std::vector<uint8_t> &bytes, Random &random, size_t prefix) {
    if (bytes.size() <= prefix)
        return;
    // Offsets are chosen past the preserved framing prefix.
    const size_t span = bytes.size() - prefix;
    const auto at = [&] { return prefix + random.below(span); };
    switch (random.below(6)) {
    case 0: // Flip a bit: exercises field-level validation.
        bytes[at()] ^= uint8_t(1u << random.below(8));
        break;
    case 1: // Replace a byte with an extreme value.
        bytes[at()] = random.below(2) ? 0xff : 0x00;
        break;
    case 2: // Truncate: every reader must survive a short buffer.
        bytes.resize(prefix + random.below(span));
        break;
    case 3: // Splice in 0xff runs, which read as huge counts and lengths.
        bytes.insert(bytes.begin() + ptrdiff_t(at()), 1 + random.below(8), 0xff);
        break;
    case 4: // Overwrite a 32-bit field with a boundary value.
        if (span >= 4) {
            static const uint32_t interesting[]{0u,          1u,          0x7fffffffu, 0x80000000u,
                                                0xffffffffu, 0x0000ffffu, 0x00ffffffu};
            uint32_t value = interesting[random.below(sizeof(interesting) / sizeof(*interesting))];
            std::memcpy(bytes.data() + prefix + random.below(span - 3), &value, sizeof(value));
        }
        break;
    default: // Drop a byte, shifting every field that follows.
        bytes.erase(bytes.begin() + ptrdiff_t(at()));
        break;
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char **argv) {
    size_t iterations = 20000;
    std::vector<std::filesystem::path> corpus;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("-runs=", 0) == 0) {
            iterations = size_t(std::stoull(arg.substr(6)));
            continue;
        }
        if (!arg.empty() && arg[0] == '-')
            continue; // Ignore the libFuzzer flags CI may pass through.
        corpus.push_back(arg);
    }

    std::vector<std::vector<uint8_t>> seeds = asperaFuzzSeeds();
    for (const auto &entry : corpus) {
        if (std::filesystem::is_directory(entry)) {
            for (const auto &file : std::filesystem::directory_iterator(entry))
                if (file.is_regular_file())
                    seeds.push_back(readFile(file.path()));
        } else if (std::filesystem::is_regular_file(entry))
            seeds.push_back(readFile(entry));
    }
    if (seeds.empty()) {
        std::fprintf(stderr, "no seeds; nothing to replay\n");
        return 1;
    }

    // Every seed must survive unmutated: the corpus is valid input by construction.
    for (size_t i = 0; i < seeds.size(); ++i)
        LLVMFuzzerTestOneInput(seeds[i].data(), seeds[i].size());

    const auto start = std::chrono::steady_clock::now();
    const size_t prefix = asperaFuzzPreservedPrefix();
    Random random{0x5eed5eed5eed5eedull};
    for (size_t i = 0; i < iterations; ++i) {
        auto bytes = seeds[i % seeds.size()];
        const size_t rounds = 1 + random.below(4);
        for (size_t r = 0; r < rounds; ++r)
            mutate(bytes, random, prefix);
        if (random.below(2))
            asperaFuzzRepair(bytes);
        // A hang is as much a defect as a crash: a 1 KB .vox used to spin
        // forever here. Report the case index so it can be replayed.
        const auto before = std::chrono::steady_clock::now();
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before)
                .count();
        if (ms > 2000) {
            std::fprintf(stderr, "case %zu took %.0f ms (%zu bytes) -- treat as a hang\n", i, ms,
                         bytes.size());
            return 1;
        }
    }
    const auto total =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%zu seeds, %zu mutated cases, %.1fs, no crashes or hangs\n", seeds.size(),
                iterations, total);
    return 0;
}
