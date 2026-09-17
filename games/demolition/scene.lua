-- Appearance is authored independently of structural/physical material properties.
engine.material(1, {roughness = 0.92, metallic = 0})
engine.material(2, {roughness = 0.80, metallic = 0})
engine.material(3, {roughness = 0.32, metallic = 1})
engine.material(4, {roughness = 0.35, metallic = 0,
  emissive = {x = 1, y = 0.35, z = 0.06}, emission_intensity = 1500})

-- All measurements are metres.  Each voxel is 0.1m, so the civic works below
-- is a sparse 4 x 3.8 x 3.2m connected structure rather than a dense block.
local floor = engine.spawn_box('Quarry apron', -5.0, -0.1, -4.0, 100, 1, 80, 1, true)
local works = engine.spawn_box('Old river works', -2.0, 0.0, -1.6, 40, 1, 32, 2, true)

-- Broad decks and roof give the shell enough area to transfer load into its
-- perimeter supports.  Individual timber and steel voxels share one grid, so
-- damage can trigger the real structural splitting code.
for _, y in ipairs({1, 13, 25}) do
  for x = 1, 38 do
    for z = 1, 30 do engine.voxel(works, x, y, z, 2) end
  end
end
for x = 0, 39 do
  for z = 0, 31 do engine.voxel(works, x, 38, z, 2) end
end

for y = 1, 37 do
  for z = 0, 31 do
    -- Tall side windows make the building readable from the approach.
    if not ((y >= 18 and y <= 30) and (z % 9 >= 2 and z % 9 <= 6)) then
      engine.voxel(works, 0, y, z, 2)
      engine.voxel(works, 39, y, z, 2)
    end
  end
  for x = 1, 38 do
    -- The front opening is a real empty doorway, not a painted panel.
    local door = (x >= 16 and x <= 23 and y <= 21)
    local window = (y >= 18 and y <= 29 and (x % 11 >= 3 and x % 11 <= 7))
    if not door and not window then engine.voxel(works, x, y, 0, 2) end
    if not window then engine.voxel(works, x, y, 31, 2) end
  end
end

-- Steel portal frames make the important load paths obvious when the stress
-- overlay is enabled.  They are still cells of the same connected object.
for _, x in ipairs({0, 12, 27, 39}) do
  for y = 1, 38 do
    engine.voxel(works, x, y, 0, 3)
    engine.voxel(works, x, y, 31, 3)
  end
end
for _, z in ipairs({0, 10, 21, 31}) do
  for y = 1, 38 do
    engine.voxel(works, 0, y, z, 3)
    engine.voxel(works, 39, y, z, 3)
  end
end
for x = 1, 38 do
  if x % 2 == 0 then
    local y = math.floor(x * 0.8) + 3
    if y < 37 then engine.voxel(works, x, y, 0, 3) end
  end
end

-- This independent target is the thing that must remain standing.  It uses
-- material 4 exclusively, allowing server.lua to track it without object-only
-- query APIs.
local target = engine.spawn_box('Amber survey core', -0.25, 0.1, -0.25, 5, 9, 5, 4, true)

engine.property('demolition.floor_id', floor)
engine.property('demolition.works_id', works)
engine.property('demolition.target_id', target)
engine.property('demolition.target_initial', engine.count_voxels(4))
engine.property('demolition.works_initial', engine.count_voxels(2) + engine.count_voxels(3))
engine.property('game.state', 0)       -- 0 active, 1 victory, 2 failed
engine.property('game.progress', 0)
engine.property('game.time', 0)
