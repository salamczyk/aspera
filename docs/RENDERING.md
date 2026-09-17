# Rendering and lighting

The editor and player share a deferred HDR renderer in `src/render.cpp`, with
voxel indirect lighting in `src/render_gi.cpp`. Headless targets retain the same
material, environment, and light data without linking the graphics runtime.
See [API](API.md) for authoring units, validation, Lua calls, and save formats.

## Frame pipeline

Geometry is cached per entity and 32³ chunk, with independent camera/shadow
culling. Four bounded workers prepare CPU meshes and the render thread publishes
results whose scene generation and content keys still match. Chunk mesh keys
include neighboring boundary occupancy, so an interior edit rebuilds its own
chunk and a face-boundary occupancy edit also rebuilds the affected neighbor.
Partial damage without cell changes and appearance-only material changes retain
geometry. Transforms remain independent of geometry. Collision and GI sources
also use per-chunk caches; structural analysis and changed-object replication
remain object-wide. See [landscapes](LANDSCAPES.md) for details and limits.

The renderer submits these passes in dependency order:

1. Three stabilized sun cascades and selected point/spot shadow faces.
2. Opaque G-buffer: RGBA8 base color/metallic, RGBA16F world normal/roughness,
   RGBA16F emission, RG16F motion, and D32F depth.
3. Dirty GI occupancy, shadowed radiance injection, directional radiance levels,
   cone tracing, temporal resolve, and bilateral upsampling.
4. Half-resolution contact AO, then direct and indirect lighting into RGBA16F HDR.
5. Optional bloom, exposure, ACES-style tone mapping, and display encoding.
6. Stress overlay and ImGui, outside scene exposure and bloom.

Packed palette colors are decoded from sRGB. Authored floating-point sun, sky,
light, and emissive RGB values are linear multipliers. Lighting stays linear
until the final display conversion. Materials use GGX specular and a
Fresnel-weighted diffuse term, with a prefiltered procedural sky cubemap and
split-sum BRDF lookup. Sky reflections are an environment approximation; local
objects are not reflected as mirrors.

## Direct lighting

The sun uses stabilized orthographic cascades with 8, 24, and 64 metre splits,
transition blending, PCF filtering, and slope/normal bias. Up to 64 point/spot
lights are assigned to conservative 16×16 screen tiles on the CPU. Local shadow
selection sorts authored priority, then stable light ID. Point lights consume
six atlas faces; spot lights consume one. The atlas reserves space for two
six-face lights. Low retains local illumination but omits its shadows.

Sun and sky intensities are authored in lux, local flux in lumens, emission in
nits, exposure in EV, and distances in metres. Point flux is converted using
4π steradians; spot flux uses the outer cone solid angle. Light attenuation
combines inverse square distance with a finite range fade. The procedural sky
is artistic daylight tied to the sun, rather than an atmospheric simulation.

## Dynamic indirect lighting

Two camera-centered occupancy volumes cover nearby and distant geometry. At the
default 0.1 m world voxel size, Balanced covers cubes of 12.8 m and 51.2 m.
Origins snap to eight-cell bricks. Occupancy stores material IDs; static source
boxes are cached and transformed with each object. Compute voxelization uses a
separating-axis overlap test between each volume cell and transformed boxes.
Both previous and current bounds dirty bricks when bodies move or disappear.
Every queued dirty brick is processed in the current frame; the diagnostics
show the number updated, with no persistent deferred update queue.

The first radiance level integrates visible faces from 2×2×2 occupied cells,
injecting shadowed sun/local diffuse light and emission. Six directional lobes
are stored in separate, progressively smaller 3D textures. Higher levels use
directional front-to-back reduction. Four hemisphere cones trace at most 24
steps, blending directional lobes and the volume boundary. Unoccluded cone
transmittance supplies sky fill outside occupied coverage. This avoids adding
a second unoccluded ambient term on top of GI.

Motion vectors reproject the previous indirect result. Depth/normal checks and
neighborhood clamping reject incompatible history; edits, moving geometry,
lighting changes, volume shifts, teleports, and scene replacement reset it.
The reset is conservative and currently global for geometry changes. Bilateral
upsampling uses full-resolution geometry to limit bleeding across edges.
Contact AO only modulates unresolved sky ambient, leaving direct light,
emission, and already occluded voxel irradiance alone.

This is one approximate diffuse bounce. Coarse far cells, finite cone counts,
thin geometry, overlapping solids, and volume boundaries can produce bias or
light leakage. Receiver offsets trade self-occlusion against fine contact
accuracy. There is no hardware ray tracing, baked lightmap, transparent
material transport, multiple bounce solve, or volumetric weather. The fixture
checks establish bounded behavior, not a universal four-frame guarantee for
arbitrary scenes.

## Quality and diagnostics

| Setting | Balanced (default) | Low |
|---|---|---|
| Sun shadows | 3 × 2048², 64 m coverage | 3 × 1024², 64 m coverage |
| Local shadows | 2 lights, 512² per face | Disabled |
| GI occupancy | 2 × 128³ | 2 × 64³ |
| GI cell sizes | World voxel size and 4× | World voxel size and 4× |
| GI tracing | Half resolution, 4 cones × 24 steps | Quarter resolution, 4 cones × 24 steps |
| Contact AO | Half resolution | Half resolution |
| Bloom | Enabled when scene strength is positive | Disabled |

Quality and diagnostic selections are local presentation state. Environment,
materials, and lights are scene data and replicate from the authority.
The Lighting panel edits these values with undo/redo and play/stop restoration.
Connected clients cannot author lighting. Diagnostic views expose material,
direct lighting, shadows, indirect lighting, AO, and an actual near-volume
occupancy slice through the camera's height.

The performance panel reports CPU submission and per-view CPU/GPU times,
updated GI bricks, draw calls, texture allocation, and estimated GI memory.
Texture allocation includes render targets. The GI estimate overlaps texture
allocation and also includes GI buffers; **do not add the two memory totals**.
Resources are recreated on size/quality changes and released when objects or
the renderer disappear. Required compute/image access and render-target formats
are checked at startup. Unsupported capabilities fail initialization explicitly.

## Measurement and qualification

Run the commands in [lighting fixtures](LIGHTING_FIXTURES.md) for visual checks,
the editor lifecycle test, and the sustained benchmark suite. Each native run
retains frame and per-view CSV data and JSON p50/p95/p99 summaries. The default
suite measures all three sample games and the two-million-voxel/500-body fixture
three times for 60 seconds at native 1920×1080 Balanced with GI enabled.
Initial structure/collision cache preparation happens before five seconds of
rendering warm-up. Scripted destruction remains active during measurement.

The frame gate requires p95 ≤16.7 ms, a full measured duration, unchanged native
resolution, bloom enabled, and unique GPU/GI timestamps for at least half the
measured frames (and at least 120 samples each). An interrupted or short run cannot
qualify. The full release gate requires all runs on **both Apple M1 and GTX
1660**; other hardware supplies development evidence only. GI has an approximate
4 ms budget, with misses recorded and investigated without changing the preset.

`frame_ms` is elapsed frame cadence; submission and simulation measure CPU work.
`gpu_ms` is the backend command-buffer interval. `gi_ms` spans the first GI
occupancy/injection dispatch through bilateral upsampling. GPU samples are
deduplicated by their asynchronously reported GPU frame IDs. On tile GPUs,
command-buffer/view intervals can overlap and exceed steady-state frame cadence;
they must not be summed to reconstruct frame time. Unsupported timing is
reported as unavailable, not zero.

The pinned bgfx Metal backend originally omitted compute-view timestamps.
`cmake/MetalComputeProfiling.cmake` applies an idempotent patch adding compute
encoder boundary counters and per-view encoder separation while profiling.
It fails configuration if the pinned source anchors change. This makes the GI
measurement include injection and filtering rather than only its raster pass.
Profiling is enabled consistently for the native development measurements.

All shaders are built and packaged by CMake/cooker. The native macOS pipeline
and exported game have been exercised, and portable SPIR-V compilation passes.
Native Direct3D11/Vulkan checks, target GPU measurements, and an actual 2× pixel
density display remain external validation requirements. CI contains three-OS
shader builds and an opt-in graphics smoke job; defining a job is not evidence
that it has passed. See [validation](VALIDATION.md) for actual results.
