-- Default 12.8 x 9.6 x 12.8 metre landscape. Generation recipes remain in Lua.
local terrain = engine.generate_landscape({seed = 7})

-- An open entrance makes the volumetric interior visible from the south edge.
-- The quarry cuts and training roof belong to the same logical terrain object.
for x = 16, 51 do
  for y = -28, 0 do
    for z = -64, -24 do engine.voxel(terrain, x, y, z, 0) end
  end
end
-- Extend the anchored foundation out to a two-column soil roof. Its roof crosses
-- local X=32. Remove the right column with the voxel tools to overload the span.
for x = 16, 51 do
  for z = -72, -65 do
    for y = -32, -29 do engine.voxel(terrain, x, y, z, 7) end
    for y = -8, -5 do engine.voxel(terrain, x, y, z, 6) end
    if x < 20 or x >= 48 then
      for y = -28, -9 do engine.voxel(terrain, x, y, z, 7) end
    end
  end
end
engine.property('landscape.terrain', terrain)
engine.property('landscape.seed', 7)
engine.property('game.state', 0)
engine.property('game.progress', 0)
engine.property('game.time', 0)

-- A cool interior work light and warm entrance light make the cave readable.
engine.add_light({type='point', position={x=3.4,y=-1.5,z=-3.3},
  color={x=0.6,y=0.78,z=1}, intensity=450, range=5, shadows=false})
engine.add_light({type='point', position={x=3.4,y=0.5,z=-8},
  color={x=1,y=0.84,z=0.65}, intensity=400, range=6, shadows=false})
