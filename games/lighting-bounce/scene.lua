-- A compact diffuse room isolates colored bounce and emissive illumination.
engine.environment({sun_direction={x=-0.25,y=-1,z=0.1},sun_intensity=20000,
  sky_intensity=0,exposure=12,bloom_strength=0})
engine.material(1,{rgba=0xbcbcbcff,metallic=0,roughness=0.95})
engine.material(2,{rgba=0xd32a20ff,metallic=0,roughness=0.9})
engine.material(3,{rgba=0x2250d0ff,metallic=0,roughness=0.9})
engine.material(4,{rgba=0xffddaaff,metallic=0,roughness=0.8,
  emissive={x=1,y=0.45,z=0.1},emission_intensity=3000})
engine.spawn_box('Neutral floor',-3,-0.1,-1,60,1,60,1,true)
engine.spawn_box('Red diffuse wall',-3,0,-1,1,30,60,2,true)
engine.spawn_box('Blue diffuse wall',2.9,0,-1,1,30,60,3,true)
engine.spawn_box('Neutral back wall',-3,0,4.9,60,30,1,1,true)
engine.spawn_box('White receiver',-.6,0,1.4,12,14,12,1,true)
engine.spawn_box('Emissive panel',-.7,1.1,3.2,14,12,1,4,true)
local wall=engine.spawn_box('Removable thin occluder',-.8,0,2.9,16,26,1,1,true)
engine.property('bounce.wall',wall)
engine.property('bounce.time',0)
engine.property('bounce.phase',0)
