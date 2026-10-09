# Engine API and data contracts

## C++

Include `aspera/engine.hpp` and link `aspera_engine`. A minimal authority:

```cpp
aspera::Engine engine;
engine.loadProject("games/demolition");
aspera::Network session;
session.host(24680);
// At fixed 60 Hz:
session.poll(engine);
engine.step();
session.broadcast(engine);
```

`World` owns materials, stable entity IDs, voxel objects, joints, numeric gameplay properties, `EnvironmentSettings`, and a map of `Light` records. Each object has a transform, sparse grid, revision, motion, anchors, fracture seams, and voxel damage. Material 0 is empty. Coordinates are object-local cells, Y is up, distances are metres, and the default cell size is 0.1 m.

`Engine::submit(Command)` assigns a command ID immediately. Submission is not completion: inspect `events` for `committed` or `rejected` with that ID. At most four worker jobs prepare edits, serialized per object and committed at a simulation boundary. `expectedRevision=0` accepts the revision current at preparation; a nonzero mismatch rejects the edit. Results that become stale during preparation are rejected. Movement during preparation is preserved.

Commands support set/remove voxel, paint, sphere damage, impulse, anchors, cube prefabs, and body release/launch. A targeted prefab attaches to a static object's grid; an untargeted prefab creates a dynamic cube. Building onto moving fragments is rejected.

`simulating=false` lets `step()` drain edits without gameplay, structural analysis, or rigid-body motion. `resetPhysics()` waits for pending preparations, discards their results, clears commands, and invalidates physics/structural caches. Use it for replacing a world. Direct object-field authoring must update revisions and reset physics as appropriate.

Use `setEnvironment`, `setMaterial`, `addLight`, `updateLight`, and `removeLight` for lighting mutations. Invalid changes throw before publishing state. Physical material changes invalidate affected objects and derived physics; queued commands survive, and stale in-flight work is explicitly rejected. Appearance-only edits update the GPU palette without remeshing. Mesh, collision, structural, and GI caches distinguish actual voxel changes from accumulated damage through the grid's incremental content key. This key is not the serialization checksum.

`raycastWorld()` transforms rays into object space and returns the nearest entity, cell, local face normal, distance, and world hit point. `importAsset()` accepts `.vox` and `.avox`, remaps materials including their appearance fields, and returns spawned IDs.

## Landscape generation

`aspera/landscape.hpp` exposes `LandscapeSettings`, `LandscapeData`,
`generateLandscape(settings)`, and `generateLandscapeChunk(settings, coordinate)`
in the dependency-free voxel library. The full generator removes disconnected
islands and returns bottom anchors; the raw chunk API does not perform the global
connectivity pass.

`Engine::generateLandscape(settings)` validates material, world, collision, and
structural budgets and returns the published entity ID. `requestLandscape(settings)`
requires a stopped engine with no pending edits, returns a command ID, and prepares
one landscape on a bounded worker. `step()` emits `committed` with the new entity
or `rejected`; a changed world, pending edits, Play, or `resetPhysics()` invalidates
the request. C++ `validateLandscape(data)` preflights supplied data without mutation;
`publishLandscape(data)` validates then publishes it.

The Lua scene-initialization API is `engine.generate_landscape(table)`, returning
an entity ID. It is unavailable during runtime updates. Existing entity-based
commands apply across terrain chunks. See [landscape settings and examples](LANDSCAPES.md).
No terrain recipe or derived cache is serialized, so the existing save and network
versions are unchanged.

## Lighting units and limits

| Field | Contract |
|---|---|
| Material `rgba` | Packed `0xRRGGBBAA`; RGB is authored in sRGB and decoded once. Geometry is opaque. |
| `roughness`, `metallic` | Finite values in [0, 1]. The GGX shader uses a small roughness floor. |
| `emissive`, sun/sky/light colors | Nonnegative finite linear RGB multipliers. |
| `emissionIntensity` | Nonnegative nits (cd/m²), multiplied by emissive RGB. Contributes to GI independently of bloom. |
| Sun/sky intensity | Nonnegative lux; sunlight travels along normalized `sunDirection`. |
| `exposure` | Fixed EV100, [-20, 30]; no automatic exposure. |
| `bloomStrength` | [0, 10], locally disabled by Low or `--no-bloom`. |
| Light `intensity`, `range` | Nonnegative lumens; range in metres, greater than zero and at most 10,000. |
| Spot angles | Degrees, `0 <= innerAngle <= outerAngle <= 89.9`. |
| Direction vectors | Finite and nonzero. |
| Light IDs | Stable 64-bit IDs, allocated from 1; `nextLight` persists and replicates. At most 64 lights. |
| Shadow priority | Integer in [-100000, 100000]. Highest priority wins; lower ID breaks ties. |

Balanced shadows at most two local lights; other lights still illuminate. Point shadows have six faces; spot shadows have one. Render targets clamp radiance to 60,000 before half-float storage. Renderer quality and diagnostic selection are local and are not saved in the world.

## Lua projects

`project.lua` returns a table with `name`. `scene.lua` initializes the scene. An existing `scene.aspera` then replaces that initial world. `server.lua` defines `update(dt)` and optionally `on_event(type, entity, command, message, details)`. Runtime gameplay and lighting mutations run on the authority. Clients display replicated state; their native HUD is the current presentation layer.

An optional `player` table opts a project into transient first-person character
sessions. Projects without it keep the free-flight camera and do not allocate
player state. The table uses this shape:

```lua
player = {
  mode = 'first_person',
  build_material = 5,
  spawns = {
    {x = -6.5, y = 0.03, z = -6, yaw = 0.75, pitch = 0},
    {x =  6.5, y = 0.03, z = -6, yaw = -0.75, pitch = 0},
  },
  bounds = {
    min = {x = -9, y = -4, z = -9},
    max = {x =  9, y =  8, z =  9},
  },
}
```

`mode` is currently `first_person`; `spawns` are feet positions and view angles,
and `bounds` are world-space playable limits. The loader validates finite values,
ordered bounds, one to sixteen spawn poses, and a build material present in the
palette. The controller uses a 1.8 m capsule with 0.3 m radius, 1.65 m eye
height, 4 m/s walk speed, 5 m/s jump velocity, 9.81 m/s² gravity, a 45° slope
limit, and a 0.3 m step height. These are runtime defaults rather than saved
world fields. Recovery clears velocity and chooses a capsule-clear authored spawn
or a nearby tested offset; a blocked spawn remains flagged for retry.

The C++ session API exposes `PlayerId`, `PlayerInput`, and `PlayerState` through
`Engine::playerSettings()`, `Engine::players()`, `addPlayer`, `removePlayer`,
`submitPlayerInput`, `setPlayerStates`, `resetPlayers`, `predictPlayer`,
`predictedPlayer`, `predictionHistorySize`, and `clearPrediction`. Player state
is session-owned: it is excluded from voxel objects, structural graphs, undo,
saves, and world checksums. `playerCollisionRevision(world)` identifies the
published collision geometry used by reconciliation.

| Function | Contract |
|---|---|
| `engine.spawn_box(name,x,y,z,nx,ny,nz,material,anchored)` | Scene initialization only. Dimensions are cell counts; position is metres. Returns entity ID. |
| `engine.voxel(id,x,y,z,material)` | Direct during scene initialization, otherwise queued. Material 0 removes. Returns command ID, or 0 during initialization. |
| `engine.prefab(x,y,z,size,material)` | Queue a dynamic cube; returns command ID. |
| `engine.damage(id,x,y,z,radius,strength)` | Queue radial material damage in world coordinates. |
| `engine.impulse(id,x,y,z)` | Queue a physical impulse. |
| `engine.anchor(id,x,y,z,enabled)` | Queue a foundation-anchor change. |
| `engine.release_body(id,vx,vy,vz)` | Queue release/launch; velocity defaults to zero. |
| `engine.raycast(x,y,z,dx,dy,dz,distance)` | Returns entity and world hit XYZ, or nil. |
| `engine.entities()` / `engine.object(id)` | Enumerate IDs / inspect name, ID, voxel count, dynamic flag, stress. |
| `engine.position(id)` | Returns world XYZ, or nil. |
| `engine.count_voxels([material])` | Count occupied cells, optionally filtered by material. |
| `engine.property(key[,value])` | Read/write a finite numeric value that saves and replicates. |
| `engine.input(action)` | Read local input; not a remote input protocol. |
| `engine.environment(table)` | Partially update environment fields: `sun_direction`, `sun_color`, `sky_color`, `sun_intensity`, `sky_intensity`, `exposure`, `bloom_strength`. |
| `engine.material(id,table)` | Partially update `rgba`, `roughness`, `metallic`, `emissive`, `emission_intensity`, or existing physical fields. |
| `engine.add_light(table)` | Return a stable ID. Fields: `type` (`"point"` or `"spot"`), `position`, `direction`, `color`, `intensity`, `range`, `inner_angle`, `outer_angle`, `shadows`, `shadow_priority`. |
| `engine.update_light(id,table)` / `engine.remove_light(id)` | Partially update or delete a light. |
| `engine.log(text)` | Append an editor event. |

Vector tables use named components, for example `{x=1,y=0,z=0}`:

```lua
engine.environment({sun_intensity=80000, sky_intensity=20000, exposure=15})
engine.material(4, {emissive={x=1,y=.25,z=.05}, emission_intensity=1500})
local lamp = engine.add_light({type='point', position={x=0,y=2,z=0},
  color={x=1,y=.8,z=.6}, intensity=800, range=8, shadows=true})
engine.update_light(lamp, {intensity=400})
engine.remove_light(lamp)
```

Persistent gameplay state belongs in `engine.property`; Lua globals and locals are recreated on reload. Script errors appear in the event log and disable failing updates until reload. Instruction hooks bound loading and per-tick execution. OS/file/package/debug libraries are unavailable; projects remain trusted code.

## Files and networking

World and cooked-asset writers emit **version 3**. Readers accept versions 1, 2, and 3. Older material records receive roughness 0.85, metallic 0, zero emission; older worlds receive the default daylight environment and no local lights. Version 1 also receives material-name impact-toughness defaults. VOX palette import retains its prior behavior and supplies those appearance defaults. Older executables cannot read newly written version-3 files.

Snapshots have explicit little-endian fields, magic/version headers, bounded counts, and validation before publication. They preserve material definitions, cells, damage, seams, transforms, velocities, anchors, joints, numeric script state, environment, lights, and allocators. Saves write a sibling temporary file and atomically replace the destination; failures are reported.

The handshake requires **protocol 4** and a matching game-content fingerprint. Reliable ordered blocks transmit initial worlds and changed objects. Material/environment/light state, light removals, allocators, properties, joints, and object deletions accompany transactions, including lighting-only updates. Clients verify a checksum before publication and request resynchronization on mismatch. Moving-body snapshots are sent at 20 Hz and interpolated. Commands are sequenced and rate/size bounded.

Player input is sampled at 60 Hz, sent in bounded redundant batches, and
acknowledged by the authoritative player snapshot. One-shot jump, damage, build,
and recovery flags are consumed once; movement frames may be repeated briefly
during silence. Player snapshots include the acknowledged input sequence,
recovery generation, support entity, and collision revision. A local prediction
history is bounded to two seconds; a missing history entry or unavailable
geometry causes a clean reset. Remote poses are interpolated, while newly
fractured or fast-moving debris can still cause a small correction because
prediction uses sampled kinematic obstacles.

The co-op protocol supports four player IDs: a listen host plus three remote
players, or four remote players on a dedicated authority. ENet channel 0 carries reliable
world transactions, channel 1 moving-body snapshots, and channel 2 player
membership, input, and player snapshots. The host assigns IDs 1 through 4 after
the content handshake; an offline/listen-host player occupies one slot and a
dedicated server creates only remote slots. Roster and poses are included in
late-join initialization. Disconnect removes the player's queued input. A
reliable round reset clears prediction and refreshes the roster; only the host
can request it. The protocol still has no account identity, authentication,
encrypted transport, public matchmaking, or host migration.

## Collision destruction

`Material::impactToughness` is positive finite joules per cubic metre. Defaults are timber/target 100000, concrete 800000, and steel 5000000. Partial voxel damage also stores normalized impact damage.

`CommandType::ReleaseBody` requires active simulation, a current optional revision, no attached joints, and velocity magnitude at most 50 m/s. Zero velocity drops the object at its current pose. Release clears anchors and angular velocity.

C++ collision events carry optional `CollisionDetails`: `other`, `point`, `normal` (first entity toward second), `closingSpeed`, and estimated joules `energy`. Lua's fifth event argument exposes `other`, `x/y/z`, `nx/ny/nz`, `closing_speed`, and `energy`. Callbacks run on the authority; clients receive committed outcomes.

Contacts are coalesced by body pair; accepted impacts suppress repeat contacts for three ticks. The strongest compound contact represents the pair. Closing speed must exceed 1 m/s. A shared energy budget is split equally between participants and distributed radially within at most 0.5 metres and five voxels. No additional physical impulse is applied by the damage code.

At most 256 impacts queue and 64 process per tick. Paired patches defer around manual edit preparation and publish only after both validate. Partial damage retains geometry caches. Actual topology changes invalidate affected chunk collision and mesh caches. Structural analysis, fragmentation, object copies, and changed-object network patches remain object-granular, so expensive edits can still produce outlier frames.
