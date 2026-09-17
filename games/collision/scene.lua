-- Appearance is authored independently of structural/physical material properties.
engine.material(1, {roughness = 0.92, metallic = 0})
engine.material(2, {roughness = 0.80, metallic = 0})
engine.material(3, {roughness = 0.32, metallic = 1})
engine.material(4, {roughness = 0.35, metallic = 0,
  emissive = {x = 1, y = 0.35, z = 0.06}, emission_intensity = 1500})

engine.spawn_box('Foundation', -8, -0.2, -6, 160, 2, 120, 1, true)
engine.spawn_box('Timber support tower', 0, 0, -1.4, 3, 30, 3, 2, true)
engine.spawn_box('Concrete impact wall', 0, 0, 2, 3, 30, 20, 1, true)
local steel = engine.spawn_box('Steel launcher', -4, 1, -1.5, 5, 5, 5, 3, true)
engine.property('launcher', steel)
engine.spawn_box('Timber drop target', 3, 0, -2, 20, 3, 20, 2, true)
engine.spawn_box('Elevated steel drop', 3.5, 4, -1.5, 6, 6, 6, 3, false)
engine.spawn_box('Stack base', -4, 0, 3, 8, 8, 8, 1, true)
engine.spawn_box('Resting stack lower', -4, 0.8, 3, 8, 8, 8, 2, false)
engine.spawn_box('Resting stack upper', -4, 1.6, 3, 8, 8, 8, 2, false)
engine.property('game.time', 0)
engine.property('lab.impacts', 0)
