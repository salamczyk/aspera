-- Values live in World.properties so hot-reloading this script never resets a
-- live round.  The player damages the building through the normal client
-- command path; this script only evaluates the result.
local function update_progress()
  local initial = engine.property('demolition.works_initial')
  if initial <= 0 then return 0 end
  local remaining = engine.count_voxels(2) + engine.count_voxels(3)
  return math.max(0, math.min(1, 1 - remaining / initial))
end

function update(dt)
  if engine.property('game.state') ~= 0 then return end
  engine.property('game.time', engine.property('game.time') + dt)
  local core_initial = engine.property('demolition.target_initial')
  local core_remaining = engine.count_voxels(4)
  local progress = update_progress()
  engine.property('game.progress', progress)
  engine.property('demolition.core_integrity', core_initial > 0 and core_remaining / core_initial or 0)

  if core_initial > 0 and core_remaining < core_initial * 0.80 then
    engine.property('game.state', 2)
    engine.log('Survey core damaged: demolition permit revoked.')
  elseif progress >= 0.68 then
    engine.property('game.state', 1)
    engine.log('River works safely collapsed. Site secured.')
  end
end
