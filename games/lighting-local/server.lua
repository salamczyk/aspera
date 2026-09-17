function update(dt)
 local t=engine.property('local.time')+dt
 engine.property('local.time',t)
 if t>=2 and engine.property('local.phase')==0 then
  engine.update_light(engine.property('local.point'),{position={x=-2,y=3.5,z=3}})
  engine.property('local.phase',1)
 elseif t>=4 and engine.property('local.phase')==1 then
  engine.remove_light(engine.property('local.point'))
  engine.property('local.phase',2)
 end
end
