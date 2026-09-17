# Implementation status

This file separates implemented behavior from the full release design. Passing the local tests is not evidence that the entire six-milestone release is complete.

## Implemented

- Native C++20 build with pinned dependency versions and a three-OS CI workflow.
- Sparse 32³ voxel chunks, negative coordinates, dirty-boundary tracking, greedy meshes, merged collision boxes, local/world ray queries.
- MagicaVoxel scene import and a bounded cooked asset format.
- Revision-checked asynchronous voxel transactions; add/remove/paint/damage and supported cube construction.
- Structural region graphs with material/contact-area strengths, gravity/contact loads, support-directed force and bending-moment distribution, fatigue thresholds, persistent fracture seams, and physical fragment extraction.
- Jolt rigid bodies with continuous collision detection, compound shapes, fixed/hinge joints, material-based impact erosion, approximate contact-load feedback, and mass-preserving splitting.
- Paired collision damage transactions, structured Lua collision events, bounded impact queues, and editor release/launch controls.
- ENet direct-connect authority, content checks, late snapshots, changed-object replication, checksums, resynchronization, rate limits, and interpolated body transforms.
- Explicit validated world save/load, Lua numeric gameplay persistence, script reload/error reporting.
- Native editor/player/cooker/server, three Lua sample games, dedicated lighting fixtures, and platform-specific game packaging.
- Deferred HDR PBR rendering, procedural daylight/sky reflections, three cascaded sun shadows, tiled point/spot lights, and two priority-selected shadowed local lights.
- Two camera-centered voxel GI volumes with transformed occupancy, directional radiance, diffuse cone tracing, motion-aware filtering, contact AO, bloom, and tone mapping.
- Local Balanced/Low presets, lighting/material authoring, diagnostic views/timings, version-3 saves/assets/network lighting state, and v1/v2 asset/save migration.
- Deterministic lighting fixtures, native lifecycle smoke checks, and a repeatable graphics benchmark retaining CPU/GPU percentile and raw-frame data.

- Finite seeded voxel landscapes with caves, overhangs, material layers, foundation anchors, Lua/editor authoring, and a quarry demo.
- Independent chunk mesh/collision/GI caches, bounded background mesh/edit preparation, static chunk bodies, dynamic compound fragments, and a byte-bounded undo history.
- Cached 8-cell structural regions and boundary bonds, graph-based connectivity, prepared fragment reuse, atomic collision-cache staging, and a frozen reference solver for differential validation.

## Still required for the full release

- General iterative elastic beam solving and calibration beyond the current support-path load approximation; torsional/free-body contact equilibrium and fatigue-state persistence need further work.
- Broader destruction workloads, rotating-fragment extraction, edit copies, and network/save performance. The finite quarry benchmark now has region-based structural preparation and five-run latency measurements; larger maps, streaming, and LOD remain deferred.
- Physical player characters, replicated player poses, and complete client movement prediction/reconciliation. Current player cameras are free-flight inspection/gameplay cameras.
- A separate Lua presentation VM, richer HUD/audio APIs, general component registration, and more complete prefab/project authoring.
- Stronger stress/collision/connectivity overlays, configurable joints, bounds/debris policies, and editor polish.
- Longer four-player motion/destruction soak tests and measured bandwidth/memory budgets. The current edit/late-join impairment test passes at 100 ms RTT, jitter/reordering, and 2% loss.
- Measured performance on the designated M1/GTX 1660 hardware, native Windows/Linux runtime checks, and signed distributable releases.

The lighting implementation is present, with local Metal validation and portable
SPIR-V shader compilation. Its target-hardware release gate is not yet qualified.
The twelve sustained M3 runs passed the local frame-time gate; GI intervals
exceeded the approximate 4 ms budget (5.44–6.79 ms p95).
See [rendering](RENDERING.md) for approximation limits and
[validation](VALIDATION.md) for the measured evidence.

These are unfinished engineering items, not silently claimed features. The foundation is intended to make subsequent milestones testable and reviewable.
