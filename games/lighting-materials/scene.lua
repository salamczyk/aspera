engine.environment({sun_intensity=35000,sky_intensity=14000,exposure=14,bloom_strength=0})
-- Equal base colours separate material response from palette differences.
engine.material(1,{rgba=0xb8b8b8ff,roughness=.08,metallic=0,emission_intensity=0})
engine.material(2,{rgba=0xb8b8b8ff,roughness=.9,metallic=0,emission_intensity=0})
engine.material(3,{rgba=0xb8b8b8ff,roughness=.08,metallic=1,emission_intensity=0})
engine.material(4,{rgba=0xb8b8b8ff,roughness=.9,metallic=1,emission_intensity=0})
engine.spawn_box('Rough floor',-5,-.1,-2,100,1,70,2,true)
for i=1,4 do
  engine.spawn_box('Response sample '..i,-4+(i-1)*2,0,0,12,20,12,i,true)
end
engine.add_light({type='point',position={x=0,y=3,z=-2},color={x=1,y=.8,z=.6},
  intensity=25000,range=12,shadows=true})
