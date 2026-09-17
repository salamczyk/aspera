-- The scene is centered around the default camera's first look direction (+Z).
engine.environment({
  sun_direction = { x = -0.45, y = -0.78, z = 0.34 },
  sun_color = { x = 1.0, y = 0.93, z = 0.82 },
  sky_color = { x = 0.55, y = 0.70, z = 1.0 },
  sun_intensity = 80000,
  sky_intensity = 18000,
  exposure = 14,
  bloom_strength = 0.04,
})

-- Material IDs remain stable in a stock project: 1 concrete, 2 timber, 3 steel, 4 target.
engine.material(1, { roughness = 0.92, metallic = 0.00 })
engine.material(2, { roughness = 0.62, metallic = 0.00 })
engine.material(3, { roughness = 0.16, metallic = 0.92 })
engine.material(4, {
  roughness = 0.35,
  metallic = 0.15,
  emissive = { x = 1.0, y = 0.22, z = 0.06 },
  emission_intensity = 1200,
})

-- Open-roof bounce room: floor z=0..12m, rear wall z=12m, side walls x=-6m/+6m.
engine.spawn_box("lab floor", -6, 0, 0, 120, 1, 120, 1, true)
engine.spawn_box("lab rear", -6, 0, 12, 120, 70, 2, 2, true)
engine.spawn_box("lab left", -6, 0, 0, 2, 70, 120, 4, true)
engine.spawn_box("lab right", 6, 0, 0, 2, 70, 120, 3, true)

-- Four 1m samples at z=3m: concrete, timber, steel, and emissive target.
engine.spawn_box("rough concrete", -3.8, 0.1, 3, 10, 10, 10, 1, true)
engine.spawn_box("rough timber", -1.2, 0.1, 3, 10, 10, 10, 2, true)
engine.spawn_box("metal", 1.4, 0.1, 3, 10, 10, 10, 3, true)
engine.spawn_box("emissive panel", 4.0, 1.5, 6, 20, 8, 1, 4, true)
engine.spawn_box("emissive receiver", 4.0, 0.1, 4.2, 14, 1, 14, 1, true)

-- Thin colored bounce cards, separated from their receiver by one metre.
engine.spawn_box("red bounce card", -5.7, 0.2, 7, 1, 26, 32, 4, true)
engine.spawn_box("blue bounce card", 5.6, 0.2, 7, 1, 26, 32, 3, true)
engine.spawn_box("bounce receiver", 0, 0.1, 8, 30, 1, 18, 1, true)

local removable = engine.spawn_box("timed wall", -1, 0.1, 5.5, 20, 25, 1, 2, true)
engine.property("lighting_lab_wall", removable)
engine.property("lighting_lab_phase", 0)
engine.property("lighting_lab_time", 0)

engine.property("lighting_lab_point", engine.add_light({
  type = "point", position = { x = -2.5, y = 4.5, z = 2.5 },
  color = { x = 1.0, y = 0.10, z = 0.06 }, intensity = 1500, range = 9,
  shadows = true, shadow_priority = 3,
}))
engine.property("lighting_lab_spot", engine.add_light({
  type = "spot", position = { x = 3.5, y = 5.5, z = 1.0 },
  direction = { x = -0.25, y = -0.75, z = 0.55 },
  color = { x = 0.08, y = 0.25, z = 1.0 }, intensity = 1800, range = 12,
  inner_angle = 18, outer_angle = 32, shadows = true, shadow_priority = 4,
}))
