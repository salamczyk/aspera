-- The objective is evaluated from material counts so it remains correct after
-- structural splitting turns the works into several moving fragments.
local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

function update(dt)
  local state = engine.property('game.state')
  if state ~= 0 then return end

  engine.property('game.time', engine.property('game.time') + dt)
  local initial_works = engine.property('demolition.works_initial')
  local initial_core = engine.property('demolition.target_initial')
  local remaining_works = engine.count_voxels(2) + engine.count_voxels(3)
  local remaining_core = engine.count_voxels(4)
  local progress = initial_works > 0 and 1 - remaining_works / initial_works or 0
  local integrity = initial_core > 0 and remaining_core / initial_core or 0

  progress = clamp(progress, 0, 1)
  integrity = clamp(integrity, 0, 1)
  engine.property('demolition.destroyed', progress)
  engine.property('demolition.core_integrity', integrity)
  engine.property('game.progress', progress)

  if integrity < 0.80 then
    engine.property('game.state', 2)
    engine.log('Survey core integrity fell below 80%. Quarry permit revoked.')
  elseif progress >= 0.68 then
    engine.property('game.state', 1)
    engine.log('Supported works removed. Quarry site secured.')
  end
end
