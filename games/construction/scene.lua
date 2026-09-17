-- Appearance is authored independently of structural/physical material properties.
engine.material(1, {roughness = 0.92, metallic = 0})
engine.material(2, {roughness = 0.80, metallic = 0})
engine.material(3, {roughness = 0.32, metallic = 1})
engine.material(4, {roughness = 0.35, metallic = 0,
  emissive = {x = 1, y = 0.35, z = 0.06}, emission_intensity = 1500})

-- A wide anchored work pad gives dynamically spawned prefab blocks a stable
-- landing surface.  The target is separate and material 4, so attacks and the
-- objective use the exact same destruction machinery as player damage.
local floor = engine.spawn_box('North yard concrete pad', -5.0, -0.1, -4.0, 100, 1, 80, 1, true)
local core = engine.spawn_box('Construction command core', -0.30, 0.0, -0.30, 6, 10, 6, 4, true)
local gate = engine.spawn_box('East gate frame', 1.8, 0.0, -1.0, 28, 1, 20, 2, true)

-- Open steel frame and a low timber berm create an immediately legible top-
-- down play space while leaving plenty of room for player-built voxels.
for y = 1, 28 do
  engine.voxel(gate, 0, y, 0, 3)
  engine.voxel(gate, 27, y, 0, 3)
  engine.voxel(gate, 0, y, 19, 3)
  engine.voxel(gate, 27, y, 19, 3)
end
for x = 0, 27 do
  for z = 0, 19 do engine.voxel(gate, x, 28, z, 3) end
end
for x = 0, 27 do
  if x < 11 or x > 16 then
    engine.voxel(gate, x, 1, 9, 2)
    engine.voxel(gate, x, 2, 9, 2)
  end
end

engine.property('construction.floor_id', floor)
engine.property('construction.core_id', core)
engine.property('construction.gate_id', gate)
engine.property('construction.core_initial', engine.count_voxels(4))
engine.property('construction.attack_at', 7)
engine.property('construction.wave', 0)
engine.property('construction.initialized', 1)
engine.property('game.state', 0)       -- 0 active, 1 convoy arrived, 2 core lost
engine.property('game.progress', 0)
engine.property('game.time', 0)
