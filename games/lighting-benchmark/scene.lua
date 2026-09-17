engine.environment({
  sun_direction = { x = -0.45, y = -0.78, z = 0.34 },
  sun_intensity = 80000, sky_intensity = 20000, exposure = 14, bloom_strength = 0.04,
})
engine.material(1, { roughness = 0.88, metallic = 0.0 })
engine.material(3, { roughness = 0.18, metallic = 0.9 })
engine.material(4, { emissive = { x = 1, y = .25, z = .05 }, emission_intensity = 900 })

-- A visible 20m x 20m floor, plus ten separated 10m x 2m x 10m slabs.
-- Total structural count including the target: 40,000 + 10 * 200,000 + 800 = 2,040,800 voxels.
engine.spawn_box("benchmark floor", -10, 0, 0, 200, 1, 200, 1, true)
for row = 0, 1 do
  for column = 0, 4 do
    local x = -27 + column * 11
    local z = -4 + row * 12
    engine.spawn_box("benchmark slab", x, 0.1, z, 100, 20, 100, (column % 2 == 0) and 1 or 3, true)
  end
end

-- 500 individual dynamic fragments distributed above the visible floor.
for i = 0, 499 do
  local x = -9 + (i % 25) * .75
  local z = 1 + (math.floor(i / 25) % 20) * .75
  local y = 3 + math.floor(i / 125) * .35
  engine.spawn_box("benchmark fragment", x, y, z, 1, 1, 1, (i % 7 == 0) and 4 or 2, false)
end

engine.property("benchmark_target", engine.spawn_box("benchmark target", -1, .1, 2, 20, 20, 2, 2, true))
engine.property("benchmark_destroyed", 0)
engine.property("benchmark_time", 0)
engine.property("benchmark_light", engine.add_light({
  type = "spot", position = { x = 2, y = 7, z = -2 }, direction = { x = -0.2, y = -0.8, z = .5 },
  color = { x = .2, y = .45, z = 1 }, intensity = 2400, range = 20,
  inner_angle = 20, outer_angle = 35, shadows = true, shadow_priority = 5,
}))
