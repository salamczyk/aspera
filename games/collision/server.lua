function update(dt)
    local previous = engine.property('game.time')
    engine.property('game.time', previous + dt)
    if previous < 0.5 and previous + dt >= 0.5 then
        engine.release_body(engine.property('launcher'), 10, 0, 0)
    end
end
function on_event(kind, entity, command, message, details)
    if kind == 'collision' then
        engine.property('lab.impacts', engine.property('lab.impacts') + 1)
        engine.property('lab.last_energy_j', details.energy)
    end
end
