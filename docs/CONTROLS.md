# Per Aspera controls

Both desktop programs accept `--project <path>`, `--host`, `--connect <host>`,
`--port <number>`, `--frames <number>`, and `--screenshot <path>`.

## Player

`W`, `A`, `S`, and `D` move the camera. Hold `Middle mouse` and move the mouse to
look around. Use `Tab` to change between first-person and top-down views.
`Left mouse` damages the voxel surface under the mouse pointer; `Right mouse`
places a timber prefab there. The pointer stays free in both views.

The Demolition project asks the player to collapse the old river works while
keeping the amber survey core intact. Construction Defense is top-down by
default: build a protective shell around the command core and survive incoming
scripted impacts for 90 seconds. HUD state is stored in the replicated world
properties: `game.state` is active/victory/failed as `0/1/2`, and
`game.progress` ranges from zero to one.

## Editor

Use the **Hierarchy** to choose an object and the **Inspector** to adjust its
position or material. The **Voxel Tools** panel chooses Add, Remove, Paint,
Anchor, or Prefab. Hold the right mouse button to look around; left mouse
applies the selected voxel tool directly under the mouse pointer, anywhere in
the viewport. Tool-panel clicks do not edit the world. Release right mouse
before editing.

`F5` starts a simulation preview. All voxel tools remain available while the
simulation runs, including on moving fragments. `F6` restores the saved editor
scene, discarding changes made during the preview.
Enable **Stress overlay** to draw local structural region bounds. Green means
0% bond capacity used, yellow 50%, and red 100% or more; the inspector reports
the selected object's peak utilization. Outlines show internal regions through
surfaces and are capped at 4,096 regions per frame. These are coarse region
diagnostics, not per-voxel stress measurements. Run Play on the host to compute
them; they are not included in client replication or save files. Paused values
are labeled as such. `--stress-preview` starts the editor in Play with this
overlay enabled.

The scene menu saves `scene.aspera`, loads it, and reloads Lua scripts. Undo and
redo restore serialized world snapshots. Pick **Fixed Joint** or **Hinge Joint**
then select two different hierarchy objects to author a joint.

Use the **Asset Import** field to add a cooked `.avox` collection or a source
`.vox` model to the current scene. Import is recorded as one undo step.

## Collision destruction

Hard collisions chip both objects according to material **Impact kJ/m3** toughness
and can sever supports into moving fragments. Gentle contacts below 1 m/s do not
apply impact damage. Impacts are authoritative on the host; use matching protocol-3
builds for co-op.

During Play, select an object and choose **Release / Drop** to remove its anchors
and let it fall at its current position. **Launch** assigns velocity along the
camera viewing direction using the **Launch m/s** slider (0–50 m/s, default 10).
Remove attached joints first. Stop restores the pre-play scene.

Try `./build/aspera_editor --project games/collision`, then Play. After half a
second a steel block launches into a timber support, and an elevated steel block
falls onto a timber target. The separate resting stack should stay intact.
The Performance panel reports queued, applied, and dropped impact records.

## Lighting

Open **LIGHTING** from the toolbar or View menu. Choose local **Balanced** or
**Low** quality, select a diagnostic view, and inspect GPU timings, texture/GI
memory, and bricks updated in the current frame. Negative timing values mean a
counter is unavailable; individual view intervals may overlap on tile GPUs.

The host or offline editor can adjust sun direction/color/lux, sky color/lux,
fixed exposure, and bloom strength. **Add point** and **Add spot** place a light
two metres in front of the camera. Select a light to edit position, color,
lumens, range, spot cone, shadow priority, or delete it. The material Inspector
edits base color, roughness, metallic, emission color/intensity, and physical
properties. These authoring edits participate in undo/redo, saves, replication,
and Play/Stop restoration. Clients receive lighting from the host.

Both applications accept `--quality balanced|low`, `--no-bloom`, `--no-ui`,
`--width`, `--height`, and `--debug-view final|material|direct|shadows|indirect|ao|voxels`.
Use `--camera x,y,z,yaw,pitch` for a fixed starting camera (angles in radians),
and `--fixed-step --frames N --screenshot /tmp/view.ppm` for a repeatable capture.
`--render-smoke` exercises offline rendering lifecycle and lighting undo/redo.
See [lighting fixtures](LIGHTING_FIXTURES.md) for visual and sustained benchmark commands.

## Landscape authoring

Open **Landscape** on the toolbar or **View → Landscape** while stopped to configure and generate a finite voxel
landscape. Generation shows progress, selects the new object on success, and adds
one undo entry. Use the existing voxel and stress tools on terrain. See
[landscapes](LANDSCAPES.md) for parameters and the quarry demo.
