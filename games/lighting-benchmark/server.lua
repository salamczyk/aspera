function update(dt)
  local t = engine.property("benchmark_time") + dt
  engine.property("benchmark_time", t)
  if engine.property("benchmark_destroyed") == 0 and t >= 3 then
    -- A deterministic, targeted topology edit during the benchmark.
    engine.damage(engine.property("benchmark_target"), 0, 1.1, 2.1, 1.6, 100000)
    engine.property("benchmark_destroyed", 1)
  end
end
