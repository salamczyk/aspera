-- Quarry Demolition is a compact first-person destruction course.  Materials
-- 2 and 3 are reserved for the works objective; terrain and player building
-- use other material IDs so construction cannot change the denominator.
engine.material(1, {roughness = 0.94, metallic = 0})
engine.material(2, {roughness = 0.80, metallic = 0})
engine.material(3, {roughness = 0.30, metallic = 1})
engine.material(4, {roughness = 0.35, metallic = 0,
  emissive = {x = 1, y = 0.35, z = 0.06}, emission_intensity = 1500})
engine.material(6, {roughness = 0.96, metallic = 0})
engine.material(7, {roughness = 0.88, metallic = 0})

-- The fixed seed gives the approach and the quarry walls a repeatable shape.
-- The apron occupies the central 16m, while the larger terrain bounds leave a
-- low rocky rim visible and reachable around its edge.  The shallow height
-- variation leaves the apron and all four authored spawns capsule-clear while
-- still exercising generated terrain collision.
local terrain = engine.generate_landscape({
  seed = 9417,
  minimum = {x = -120, y = -16, z = -120},
  maximum = {x = 120, y = 16, z = 120},
  surface_height = -2,
  hill_amplitude = 2,
  feature_scale = 72,
  cave_threshold = 1,
  overhang_strength = 0,
  grass_depth = 1,
  soil_depth = 3,
  bottom_depth = 8,
  grass_material = 6,
  soil_material = 6,
  rock_material = 7,
})

-- A concrete apron makes the approach and the supported frame deterministic;
-- the generated terrain remains visible around its edge as the quarry wall.
local floor = engine.spawn_box('Quarry apron', -8.0, -0.1, -8.0, 160, 1, 160, 1, true)

-- Build the works as one anchored object.  Only four 0.8m pads retain anchors;
-- removing a column disconnects the roof into the moving rubble fragments used
-- by the character traversal slice.
local works = engine.spawn_box('Supported quarry works', -4.0, 0.0, -2.8, 80, 1, 56, 2, true)
local pads = {
  {x0 = 8,  x1 = 15, z0 = 8,  z1 = 15},
  {x0 = 64, x1 = 71, z0 = 8,  z1 = 15},
  {x0 = 8,  x1 = 15, z0 = 40, z1 = 47},
  {x0 = 64, x1 = 71, z0 = 40, z1 = 47},
}
local function in_pad(x, z)
  for _, pad in ipairs(pads) do
    if x >= pad.x0 and x <= pad.x1 and z >= pad.z0 and z <= pad.z1 then return true end
  end
  return false
end
for x = 0, 79 do
  for z = 0, 55 do
    if not in_pad(x, z) then engine.voxel(works, x, 0, z, 0) end
  end
end

-- Four tall timber legs make the support-removal route legible from the south
-- approach.  The inner steel faces and the roof beams provide distinct load
-- paths without filling the playable doorway.
for _, pad in ipairs(pads) do
  for x = pad.x0, pad.x1 do
    for z = pad.z0, pad.z1 do
      for y = 1, 39 do
        local steel = (x == pad.x0 or x == pad.x1 or z == pad.z0 or z == pad.z1)
        engine.voxel(works, x, y, z, steel and 3 or 2)
      end
    end
  end
end

-- A shallow roof is dense enough to read as a supported work platform but leaves
-- the interior open for walking, tool reach, and falling fragments.
for x = 0, 79 do
  for z = 0, 55 do engine.voxel(works, x, 40, z, 2) end
end
for x = 0, 79 do
  for _, z in ipairs({8, 27, 47}) do engine.voxel(works, x, 39, z, 3) end
end
for z = 0, 55 do
  for _, x in ipairs({8, 15, 64, 71}) do engine.voxel(works, x, 39, z, 3) end
end

-- Side braces and a rear lintel create the intended support-removal route.  The
-- front remains open, while the side openings keep the rubble traversable.
for y = 8, 39 do
  for _, z in ipairs({0, 55}) do
    for _, x in ipairs({0, 1, 78, 79}) do engine.voxel(works, x, y, z, 2) end
  end
end
for x = 0, 79 do
  for y = 34, 39 do engine.voxel(works, x, y, 55, 3) end
end

-- The target is a separate anchored object, one metre east of the works.  Its
-- clear gap keeps collapse fragments from sharing the core's structure graph.
local core = engine.spawn_box('Amber survey core', 5.0, 0.0, -0.3, 12, 20, 12, 4, true)

engine.property('quarry.terrain_id', terrain)
engine.property('quarry.floor_id', floor)
engine.property('quarry.works_id', works)
engine.property('quarry.core_id', core)
engine.property('demolition.target_initial', engine.count_voxels(4))
engine.property('demolition.works_initial', engine.count_voxels(2) + engine.count_voxels(3))
engine.property('demolition.core_integrity', 1)
engine.property('demolition.destroyed', 0)
engine.property('game.state', 0)       -- 0 active, 1 victory, 2 failed
engine.property('game.progress', 0)
engine.property('game.time', 0)
