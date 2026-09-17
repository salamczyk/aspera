# Lighting fixtures

All fixtures are Lua projects using the public engine APIs. Use `--fixed-step --no-ui` for repeatable captures. A saved `scene.aspera` overrides scene initialization; the checked-in fixtures have no saved override. The default camera is `(5,4,-7)`, yaw `-0.62`, pitch `-0.42` radians.

| Project | Purpose |
|---|---|
| `lighting-materials` | Four equal-color samples: smooth/rough dielectric, then smooth/rough metal, from left to right. A point light and procedural daylight expose their different BRDF responses. |
| `lighting-local` | A warm point light, blue spot light, thin occluders, and near-dark ambient fill. At 2 seconds the point moves; at 4 seconds it is removed. |
| `lighting-bounce` | Compact 6 m diffuse room, neutral floor/receiver, nonmetallic red and blue walls, and a warm emissive panel. Sky intensity and bloom are zero. At 2 seconds the thin wall between panel and receiver is removed; at 4 seconds emission is extinguished. |
| `lighting-lab` | Larger open-roof daylight room with material samples, two local lights, thin wall, emissive panel, and receiver. At 2 seconds the wall centered at `(0,1.35,5.55)` is destroyed; at 4 seconds material-4 emission and the red point light are switched off. Its side cards intentionally use the metallic/emissive sample materials; use `lighting-bounce` to isolate diffuse color transfer. |
| `lighting-benchmark` | 2,040,800 initial structural voxels (40,000 floor + ten 200,000-voxel slabs + 800-voxel target), plus 500 dynamic one-voxel bodies. A spotlight casts local shadows. At 3 seconds a radial edit destroys the target. |
| `collision` | Existing physical impact fixture: a launched block, rotating/separating supports, falling impactor, and intact resting stack. Exercises transformed geometry and incremental GI occupancy. |

## Repeatable visual acceptance

```sh
python3 tools/check_lighting.py --output out/lighting-checks
./build/aspera_player --project games/lighting-materials --fixed-step --no-ui --frames 30 --screenshot /tmp/materials.ppm
./build/aspera_player --project games/lighting-local --fixed-step --no-ui --frames 30 --screenshot /tmp/local.ppm
./build/aspera_editor --project games/lighting-lab --render-smoke --screenshot /tmp/lifecycle.ppm
```

The Python check uses native 800×600 indirect-only captures with bloom disabled. It verifies that wall removal changes indirect lighting, the emitter illuminates nearby surfaces, the neutral floor receives red and blue bounce after emission is off, and the tested changes settle within four rendered frames. Raw PPMs and numerical results remain in the output directory. Tolerances allow subpixel/backend differences; inspect the captures as well as the pass/fail result. This is a bounded fixture test, not a universal guarantee for all edits.

`--render-smoke` is an offline 96-frame lifecycle run. It changes resolution and quality, verifies lighting/material/light-ID undo and redo, teleports the camera, minimizes/restores the window, and restores the scene. Default window creation requests high pixel density; actual pixel scale depends on the attached display. It does not save over the project.

## Sustained performance

```sh
python3 tools/run_render_benchmarks.py --output out/render-benchmarks/my-machine
```

The default suite runs Demolition, Construction, Collision, and the large lighting benchmark **three times each for 60 measured seconds after five seconds of warm-up**, at native 1920×1080 Balanced with GI enabled. Initial structural/collision cache construction precedes the warm-up. Camera paths use elapsed measurement time; gameplay advances at fixed simulation steps. The normal scene scripts and physical destruction remain active during measurement.

Each run writes frame CSV, per-view CSV, percentile JSON, and a log. The suite records hardware, a source fingerprint, and a Markdown report. `--seconds 10 --runs 1` is useful for development but cannot pass qualification. Low can be measured explicitly with `--quality low`; it cannot satisfy the Balanced release gate. Avoid other GPU workloads during measurements.

Qualification requires frame p95 ≤16.7 ms in every required run on **both M1 and GTX 1660**. Investigate GI intervals above the approximate 4 ms budget and retain outliers. Results on other GPUs are development evidence only. Native Direct3D11 and Vulkan runtime verification remains a separate requirement from shader compilation.
