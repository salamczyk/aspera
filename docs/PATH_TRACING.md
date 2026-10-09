# Native path tracing

Path tracing is an opt-in renderer selected at startup. The default remains the
existing bgfx raster renderer with Balanced and Low quality. Metal owns the
native GPU path on Mac; Vulkan ray queries own it on Windows/Linux. Backend
switching requires a restart. Renderer choices are local and do not change
scene saves or replication.

```sh
./build/aspera_player --project games/path-tracing --renderer path-traced --width 1920 --height 1080
./build/aspera_editor --project games/quarry-demolition --renderer path-traced
./build/aspera_player --project games/path-tracing --renderer path-traced --width 800 --height 600 --no-ui --no-bloom --pt-capture-samples 256 --screenshot /tmp/path-tracing-reference.ppm
python3 tools/run_render_benchmarks.py --renderer path-traced --output out/render-benchmarks/path-traced
```

`--pt-samples` selects 1–64 samples per pixel per frame (default 1).
`--pt-bounces` selects 1–12 surface interactions (default 6).
`--pt-no-denoise` disables spatial denoising. Stationary history still accumulates.
`--pt-capture-samples N` freezes simulation, waits for all scene meshes to publish,
uses a deterministic sampling sequence, and accumulates exactly N samples with
denoising disabled. It requires a screenshot, path tracing, and an offline
session. Use `--no-ui` for image comparisons; live performance text can vary.
Reference capture exits on completion; `--frames` does not truncate it.
Existing `--camera x,y,z,yaw,pitch` selects a repeatable viewpoint.

Unsupported GPUs or missing optional backends fall back to raster with a reason
in the Lighting/performance panels and application log. Explicit path-tracing
benchmarks and reference captures fail instead of falling back. Native resource
allocation or GPU failures are reported as errors.

The path tracer traces primary visibility and multiple diffuse/GGX specular
bounces. It explicitly samples the procedural sky, a 0.27-degree sun disk,
punctual point/spot lights, and emissive triangles, with multiple importance
sampling for the continuous sources. It does not add raster shadow-map lighting,
voxel GI, screen-space ambient occlusion, or cubemap reflections. Exposure, bloom,
tone mapping, and UI follow the lighting solve. Materials are opaque; there is
no transmission, participating media, or spectral transport.

Native images are traced at output resolution. A temporal resolve rejects
incompatible depth, normal, and surface identity, uses separate previous-frame
history snapshots, and feeds five edge-aware 3×3 filtering passes. Moving history
is bounded; a stationary image progressively accumulates. Scene, geometry,
lighting, and resolution changes invalidate incompatible history. Stable chunk
IDs and previous instance transforms support moving-object reprojection.

The Lighting panel uses path-tracing-specific diagnostic labels for the existing
view selector: Final, Material, Normal, Diffuse, Specular, Depth, and History.
Performance reports include trace, denoise/post, acceleration-structure times,
and native memory. Metal pass intervals may overlap on tile GPUs; frame GPU
time uses their interval envelope.
GPU memory includes tracing images, scene buffers, and acceleration structures.

## Build and qualification

Mac builds compile the Objective-C++ backend against Metal and QuartzCore. If
Xcode provides `metal` and `metallib`, CMake packages compiled native libraries.
Command Line Tools-only builds package the source shaders and compile them via
Metal at startup. This is independent of the bgfx shader build.

Windows/Linux builds enable the Vulkan path when a Vulkan SDK and
`glslangValidator` are available. `ASPERA_VULKAN_PT=OFF` explicitly disables it.
Vulkan 1.2, acceleration structures, buffer device addresses, and ray queries
are required. Runtime device feature checks remain necessary even when shaders
compile. `ASPERA_VULKAN_PT=ON` on Mac is useful for syntax/build validation, not
for claiming native Windows/Linux runtime qualification.

The transport lab contains metal samples, an off-camera red card, a small
emitter, colored diffuse walls, a sun-shadow pillar, and a sealed room. Capture
from inside the sealed room with `--camera -4,1,7,0,0`: it should receive no
illumination. Compare one- and six-bounce captures to inspect color transfer.
Use `--camera -1.4,1.2,-3,0,0` to see the off-camera red card reflected
in the smooth metal sample; the card is behind that camera.
Use existing lighting-local/collision fixtures for light removal and moving
geometry, and the editor smoke run for scene replacement, undo/redo, resizing,
and minimizing/restoring. Inspect captures in addition to automated checks.

The optional mode targets native 1920×1080 at 30 fps on an 8-core M3 and RTX 3060
12 GB. Qualification requires frame p95 ≤33.3 ms, native resolution, active
path tracing, denoising, bloom, a full 60-second measurement, and sufficient
unique GPU/trace/denoise timing samples. Three passing runs per scene are required
on both reference GPUs; Vulkan must also be exercised on Windows and Linux.
No runtime support or performance claim follows from shader compilation alone.
Actual local evidence is recorded in docs/VALIDATION.md.

Run `python3 tools/check_path_tracing.py` for repeatable sample-grouping and
closed-room leakage checks, including a partial final sample dispatch.
