function update(dt)
  local t = engine.property("lighting_lab_time") + dt
  engine.property("lighting_lab_time", t)
  local phase = engine.property("lighting_lab_phase")

  -- At 2 seconds, remove a thin timber wall to expose the bounce receiver.
  if phase == 0 and t >= 2 then
    local wall = engine.property("lighting_lab_wall")
    engine.damage(wall, 0, 1.35, 5.55, 2, 100000)
    engine.property("lighting_lab_phase", 1)
  end
  -- At 4 seconds, extinguish material emission and remove the red local light.
  if phase == 1 and t >= 4 then
    engine.material(4, {emission_intensity = 0})
    engine.remove_light(engine.property("lighting_lab_point"))
    engine.property("lighting_lab_phase", 2)
  end
end
