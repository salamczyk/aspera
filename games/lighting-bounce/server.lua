function update(dt)
  local t=engine.property('bounce.time')+dt
  engine.property('bounce.time',t)
  local phase=engine.property('bounce.phase')
  if phase==0 and t>=2 then
    engine.damage(engine.property('bounce.wall'),0,1.3,2.95,2,100000)
    engine.property('bounce.phase',1)
  elseif phase==1 and t>=4 then
    engine.material(4,{emission_intensity=0})
    engine.property('bounce.phase',2)
  end
end
