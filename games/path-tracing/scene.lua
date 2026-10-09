-- Opaque transport fixtures: off-camera reflections, small emitters, glossy
-- and rough metal, colored bounce, soft sun shadows, and a sealed dark room.
engine.environment({sun_direction={x=-.55,y=-.65,z=.45},sun_intensity=80000,
  sky_intensity=12000,exposure=14.5,bloom_strength=.04})
engine.material(1,{rgba=0xbcbcbcff,roughness=.9,metallic=0})
engine.material(2,{rgba=0xd53024ff,roughness=.9,metallic=0})
engine.material(3,{rgba=0x235bd0ff,roughness=.9,metallic=0})
engine.material(4,{rgba=0xd8d8d8ff,roughness=.08,metallic=1})
engine.material(5,{rgba=0xd8d8d8ff,roughness=.65,metallic=1})
engine.material(6,{rgba=0xffeeccff,roughness=.8,metallic=0,
  emissive={x=1,y=.7,z=.3},emission_intensity=3000})
engine.spawn_box('Neutral floor',-6,-.1,-3,120,1,110,1,true)
engine.spawn_box('Red bounce wall',-4,0,1,1,30,40,2,true)
engine.spawn_box('Blue bounce wall',3.9,0,1,1,30,40,3,true)
engine.spawn_box('Rear receiver',-4,0,5,80,30,1,1,true)
engine.spawn_box('Smooth metal',-2,0,1,12,15,12,4,true)
engine.spawn_box('Rough metal',.5,0,1,12,15,12,5,true)
-- This object is behind the default camera, but can appear in reflections.
engine.spawn_box('Off-camera red card',-3,0,-8,60,35,1,2,true)
engine.spawn_box('Small emissive panel',-.4,1.2,4,8,8,1,6,true)
engine.spawn_box('Shadow contact pillar',2,0,3,3,25,3,1,true)
-- Six solid walls enclose a room with no authored emitters or lights inside.
engine.spawn_box('Sealed room floor',-5,0,6,20,1,20,1,true)
engine.spawn_box('Sealed room ceiling',-5,2,6,20,1,20,1,true)
engine.spawn_box('Sealed room left',-5,0,6,1,20,20,1,true)
engine.spawn_box('Sealed room right',-3.1,0,6,1,20,20,1,true)
engine.spawn_box('Sealed room front',-5,0,6,20,20,1,1,true)
engine.spawn_box('Sealed room rear',-5,0,7.9,20,20,1,1,true)
