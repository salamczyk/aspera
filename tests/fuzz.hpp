#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Fuzz targets are written against libFuzzer's entry point so they can be driven
// by `-fsanitize=fuzzer` wherever it is available. Apple clang does not ship
// libFuzzer, so tests/fuzz_main.cpp supplies a portable driver that replays a
// corpus and applies deterministic mutations to it. Both drivers call the same
// LLVMFuzzerTestOneInput, so a crash found by either reproduces under the other.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Valid inputs each target seeds its corpus with. Keeping these in the harness
// means no binary fixtures in the repository and no way for the seeds to drift
// away from the encoders that produce them.
std::vector<std::vector<uint8_t>> asperaFuzzSeeds();

// Leading bytes the mutator leaves alone. Without this, measurement showed a
// quarter of .vox cases dying on the four-byte "VOX "/"MAIN" literal compares
// instead of reaching any validation logic.
size_t asperaFuzzPreservedPrefix();

// Optional structure-aware fixup applied to half the mutated cases. Formats that
// frame their payload with a length (.vox nests chunks inside MAIN) otherwise
// reject almost everything at the outer bounds check, so the nested parsers are
// never exercised. The other half runs unrepaired so the framing checks stay
// covered too.
void asperaFuzzRepair(std::vector<uint8_t> &bytes);
