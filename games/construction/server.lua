-- Script reload retains properties.  In particular, do not set attack_at or
-- wave here: an editor Lua reload must not restart a live defense round.
local function attack(core, wave)
  -- Resolve the first actual voxel from the incoming lane. Prefab blocks placed
  -- in that lane are damaged before the command core, so defense construction
  -- affects the real attack outcome instead of merely decorating the scene.
  local lane = (wave % 5 - 2) * 0.16
  local path = wave % 4
  local ox, oz, dx, dz
  if path == 0 then ox, oz, dx, dz = 7.0, lane, -1, 0
  elseif path == 1 then ox, oz, dx, dz = -7.0, lane, 1, 0
  elseif path == 2 then ox, oz, dx, dz = lane, 6.0, 0, -1
  else ox, oz, dx, dz = lane, -6.0, 0, 1 end
  local entity, x, y, z = engine.raycast(ox, 0.55, oz, dx, 0, dz, 20)
  if entity then engine.damage(entity, x, y, z, 0.62 + math.min(wave, 10) * 0.018, 0.42 + math.min(wave, 12) * 0.035) end
  engine.log('Incoming impact wave ' .. wave)
end

function update(dt)
  if engine.property('construction.initialized') == 0 then
    -- This branch only supports loading an older saved world. New scenes set
    -- these once during scene.lua and never overwrite them on reload.
    engine.property('construction.initialized', 1)
    engine.property('construction.attack_at', engine.property('game.time') + 7)
  end
  if engine.property('game.state') ~= 0 then return end

  local elapsed = engine.property('game.time') + dt
  engine.property('game.time', elapsed)
  local initial = engine.property('construction.core_initial')
  local remaining = engine.count_voxels(4)
  engine.property('construction.core_integrity', initial > 0 and remaining / initial or 0)
  engine.property('game.progress', math.min(1, elapsed / 90))

  if initial <= 0 or remaining < initial * 0.30 then
    engine.property('game.state', 2)
    engine.log('Command core lost. Restart the project to try again.')
    return
  end
  if elapsed >= 90 then
    engine.property('game.state', 1)
    engine.log('Convoy arrived. Construction defense complete.')
    return
  end

  local next_attack = engine.property('construction.attack_at')
  if elapsed >= next_attack then
    local wave = engine.property('construction.wave') + 1
    engine.property('construction.wave', wave)
    engine.property('construction.attack_at', elapsed + math.max(2.4, 6.0 - wave * 0.16))
    attack(engine.property('construction.core_id'), wave)
  end
end
