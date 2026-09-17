# Landscapes and chunks

The landscape generator creates a finite, editable voxel object. Its default
128 × 96 × 128-cell bounds cover 12.8 × 9.6 × 12.8 metres at the engine's 0.1 m
voxel size. All chunks stay resident. There is no streaming or terrain LOD.

```sh
./build/aspera_editor --project games/landscape
./build/aspera_player --project games/landscape
./build/aspera_server --project games/landscape --no-network --ticks 120
```

In the editor, open **Landscape** on the toolbar or **View → Landscape**, choose a seed and settings, and press
**Generate new landscape** while stopped. Generation and derived-budget checks
run in the background. A successful result becomes the selected object and adds
one undo entry. Starting Play, loading/undoing a world, or committing another
edit cancels publication of the pending result. Errors leave the existing world
intact. The panel displays elapsed preparation time and the result.

Generation settings in the editor last for that session. Save stores the actual
terrain and its edits, not a recipe. Keep reusable recipes in `scene.lua`:

```lua
local terrain = engine.generate_landscape({
  seed = 7,
  minimum = {x = -64, y = -32, z = -64},
  maximum = {x = 64, y = 64, z = 64},
  surface_height = 12,
  hill_amplitude = 18,
  feature_scale = 48,
  cave_threshold = 0.63,
  overhang_strength = 12,
  grass_depth = 1, soil_depth = 3, bottom_depth = 4,
  grass_material = 5, soil_material = 6, rock_material = 7,
})
engine.property('terrain_id', terrain)
```

Bounds are half-open object-local cell coordinates; Y is up. Heights, feature
scale, displacement, and layer depths are measured in cells. The unsigned
32-bit seed samples the same field at the same absolute cell coordinates,
independently of chunk generation order. `feature_scale` controls feature size;
`cave_threshold=1` disables caves, while lower values remove more interior rock.
`overhang_strength=0` disables volumetric surface displacement.

The bottom `bottom_depth` layers remain solid rock. Every bottommost cell is
anchored; chunk edges are not anchored. Disconnected islands are removed before
publication. Grass and soil appear at outdoor surfaces, with rock on deep cave
walls and floors. The default engine palette adds grass, soil, and rock at IDs
5, 6, and 7. Saved worlds retain their own palettes: choose existing material IDs
when generating in an older saved world.

Generation accepts at most two million bounded cells, dimensions up to 512 cells,
and coordinates within the existing edit domain (X/Z −1280 through 1280 and
Y −640 through 640). World snapshot limits, the 10,000 structural-node limit, and
the 32,768 collision-box limit are checked before publication. Detailed noise can
hit a derived limit before reaching the cell limit; increase feature size or
reduce caves/displacement in that case.

## Collapse and the quarry demo

Use the normal Remove, Add, Paint, Anchor, prefab, and damage tools on terrain.
The support solver sees the complete voxel object, including bonds across chunk
boundaries. Weak or overloaded connections fracture; pieces with no anchor path
become dynamic rigid bodies. A detached piece can span several chunks. The
solver remains the engine's gameplay stress approximation.

The demo opens a quarry entrance at the south edge and adds a two-column soil
roof extending from the terrain foundation. Its roof crosses X=32 and belongs
to the same object. Remove the right column near world X=5, Y=−2, Z=−6.8 to
remove a support. Fly below the surface to inspect the cave interior and use the
stress overlay to inspect the load paths.

## Processing and diagnostics

- `chunkOf` and `localOf` handle negative cell coordinates.
- `VoxelGrid::chunkContentKey` tracks a chunk's cell content.
  `chunkMeshKey` also incorporates the occupied boundary faces of six neighbors.
  Interior edits and boundary paint do not invalidate neighboring meshes.
- `meshChunk` samples neighbor occupancy and emits only the owner chunk's visible
  faces. `collisionBoxesChunk` merges material-compatible cells within one chunk.
  Whole-grid geometry APIs remain available.
- Four renderer workers prepare CPU meshes, with a bounded queue. The render
  thread publishes matching results; obsolete scene and content keys are dropped.
  Geometry, shadows, and GI maintain independent chunk caches.
- Static voxel objects use one collision body per occupied chunk. Dynamic
  fragments use one compound body assembled from chunk collision boxes. Contact
  events and joints continue to refer to logical entity IDs.
- Edit/generation workers retain prepared collision boxes and reuse unchanged
  chunks. Jolt body creation and replacement run on the simulation thread.
- Structural membership uses cached 8 × 8 × 8-cell regions with dense labels.
  Changed chunk inputs identify local region rebuilds; anchor changes update
  support flags without reconstructing membership. Boundary contacts and bond
  geometry are reused independently of renderer dirty flags.
- Connectivity runs over structural nodes. The existing global support/load
  solve and fatigue thresholds still run every simulation tick; collapse is not
  deferred across frames. Node and bond ordering matches the reference solver.
- Fragments partition prepared graphs and copy intact chunks. Mixed chunks are
  rebuilt, and matching collision boxes are reused. All fragment objects, graphs,
  and collision caches are staged before ownership publication.
- Generation seeds its validated graph into the live cache. Material-property
  changes refresh node masses and reset fatigue without rebuilding membership.
- The Performance panel separates structural preparation, connectivity, solving,
  fragment preparation, collision preparation, and physics publication. Region
  rebuilds, cells scanned, cache hits, and estimated structural cache bytes
  supplement the existing mesh and collision diagnostics. Structural work counters
  are per tick; collision rebuild counters are cumulative.

Edit preparation still copies object state. Saves and network patches still encode
changed objects in full; save version 3 and network protocol 3 are unchanged.
The load solve remains global over the compact graph, and rotating fragments retain
a dense cell traversal to preserve velocity accumulation order. These paths, larger
workloads, and replication remain candidates for further measurement. Undo and redo
share a 256 MiB byte budget, with oldest entries evicted first and a 96-entry limit
per stack.

## Validation and benchmark

```sh
cmake --build --preset dev --parallel 4
ctest --preset dev
./build/landscape_engine_tests --benchmark
./build/aspera_editor --project games/lighting-lab --render-smoke
```

The benchmark runs five independent default-map and quarry scenarios, reporting
initialization separately, tick p50/p95/maximum, paused-edit completion latency,
the first tick after digging, and the worst collapse tick's stage breakdown.
For a quick development sample, add --benchmark-runs 1. Each run requires the
quarry roof to detach on tick 16 with all 1,152 voxels. Timing gates are evaluated
on the measured host rather than imposed on hardware-independent correctness tests.
See [validation](VALIDATION.md) for recorded results and workload limits.
