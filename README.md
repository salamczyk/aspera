# Per Aspera

A standalone C++20 voxel-engine **development build** with Lua game projects, a native editor/player, Jolt rigid bodies, structural destruction, ENet co-op, and a command-line asset cooker. This repository implements the first working foundation of the accompanying engine design; it is not yet a production-ready SDK.

## Build

Requirements: CMake 3.24+, a C++20 compiler, Git, and a desktop graphics driver. The first configuration downloads pinned SDL3, bgfx, Jolt, ENet, Lua, and Dear ImGui sources. Shader-tool compilation makes the first build substantially slower than incremental builds.

```sh
cmake --preset dev
cmake --build --preset dev --parallel 6
ctest --preset dev
```

On Windows, use a Visual Studio developer terminal and `cmake -S . -B build`, followed by `cmake --build build --config Release`. Binaries are then under `build/Release`. The CI workflow builds on Windows, Linux, and macOS. Local execution has to be verified separately on each platform.

For a graphics-free server, configure with `-DASPERA_DESKTOP=OFF`. To build only the dependency-free voxel importer/cooker and its tests, use `cmake --preset core` and `cmake --build --preset core`.

## Run

Run these commands from the repository root:

```sh
./build/aspera_editor --project games/demolition
./build/aspera_player --project games/demolition
./build/aspera_player --project games/construction
./build/aspera_editor --project games/collision
```

The editor supports import, pointer-based live voxel edits, materials, anchors, joints, drop/launch controls, undo/redo, save/load, and play/stop. Hard object impacts chip material and fracture supports; the Collision Lab project demonstrates it. See [controls](docs/CONTROLS.md).

The editor and player now use HDR PBR lighting, a procedural daylight sky, cascaded
sun shadows, point/spot lights, contact AO, bloom, and dynamic voxel indirect
lighting. Balanced enables GI by default; Low retains GI at reduced resolution.
Open **LIGHTING** in the editor to author the environment and local lights; edit
roughness, metalness, and emission in the material Inspector.

```sh
./build/aspera_player --project games/lighting-bounce --no-bloom
./build/aspera_editor --project games/lighting-lab
./build/aspera_player --project games/demolition --quality low
```

```sh
# Host plus three remote players
./build/aspera_player --project games/demolition --host
./build/aspera_player --project games/demolition --connect 127.0.0.1

# Headless authority; direct connection uses UDP port 24680
./build/aspera_server --project games/construction --port 24680

# Finite offline smoke test and save
./build/aspera_server --project games/demolition --no-network --ticks 120 --save /tmp/world.sav
```

Internet clients need a reachable host address/port. There is no relay, lobby service, or host migration. All peers must have the same game content. Co-op currently replicates world edits and moving fragments; the full player-character prediction/reconciliation system remains unfinished.

## Landscapes

Generate seeded hills, caves, and overhangs from **View → Landscape** in the editor
or `engine.generate_landscape({...})` in a Lua scene. Terrain supports the existing
editing, damage, and material-based collapse tools. The default finite map is
12.8 × 9.6 × 12.8 metres, processed in 32³ chunks.

```sh
./build/aspera_editor --project games/landscape
```

See [landscape generation and chunk processing](docs/LANDSCAPES.md) for settings,
limits, the quarry collapse demo, and benchmark commands.

## Assets and export

```sh
./build/aspera_cooker model.vox model.avox
./build/aspera_cooker --new-project out/my-game
./build/aspera_editor --project out/my-game
./build/aspera_cooker --package games/demolition out/demolition
./out/demolition/Play.sh
```

Packages include the player, server, compiled shaders, and game content; they run on the operating system on which they were built. Build and export on each target OS. The output directory must be empty. The editor can import `.vox` and cooked `.avox` files. Imported assets initially behave as dynamic objects; add foundation anchors for static structures.

## Architecture

- `aspera_voxel`: sparse 32³ chunks, seeded volumetric landscapes, greedy chunk meshing, merged collision boxes, DDA queries, scene-aware VOX import, cooked assets.
- `aspera_engine`: queued revision-checked edits, bounded worker preparation, support graphs, Jolt bodies/constraints, Lua, versioned snapshots, replication.
- `aspera_app`: SDL3 window/input, bgfx rendering, ImGui authoring, cameras, HUD.
- Game directories: scene definitions and gameplay scripts using the same public interfaces.

See [API and formats](docs/API.md) and [implementation status](docs/STATUS.md) for contracts and remaining work. Structural loads are a gameplay approximation. The initial solver distributes forces and bending moments through support paths; it is not an engineering analysis tool.

## Validation and performance

Tests cover voxel boundaries/imports, save decoding, edit revisions, fragmentation, and ENet loopback replication. Socket tests require permission to bind a local UDP port. No service or account is required.

```sh
./build/aspera_server --project games/demolition --no-network --benchmark --ticks 120
```

The benchmark constructs two million structural voxels and 500 additional one-voxel dynamic objects and reports CPU tick percentiles. It does **not** establish a 1080p/60 FPS graphics result. The design's hardware and network-performance gates remain release criteria until measured on the stated machines.

For the graphics suite, run `python3 tools/run_render_benchmarks.py --output out/render-benchmarks/my-machine`.
It records three 60-second 1080p Balanced runs per scene, including the large
fixture, after warm-up, with CPU/GPU percentiles and raw pass timings. Run
`python3 tools/check_lighting.py` for deterministic bounce/emission cleanup checks.
See [rendering](docs/RENDERING.md), [fixtures](docs/LIGHTING_FIXTURES.md), and
[local validation](docs/VALIDATION.md). The M1/GTX 1660 gate remains unqualified
until both target GPUs pass; results on the available M3 are development evidence.
