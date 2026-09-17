engine.environment({sun_intensity=0,sky_intensity=10,exposure=6,bloom_strength=0})
engine.material(1,{rgba=0xbcbcbcff,roughness=.85,metallic=0})
engine.spawn_box('Floor',-4,-.1,-1,80,1,80,1,true)
engine.spawn_box('Back wall',-4,0,6.9,80,40,1,1,true)
engine.spawn_box('Point occluder',-.8,0,1.5,3,20,3,1,true)
engine.spawn_box('Spot occluder',1,0,3.5,18,16,2,1,true)
engine.property('local.point',engine.add_light({type='point',position={x=-2,y=2,z=1},
 color={x=1,y=.14,z=.025},intensity=650,range=9,shadows=true,shadow_priority=3}))
engine.add_light({type='spot',position={x=2.5,y=3.8,z=1},direction={x=-.3,y=-1,z=.8},
 color={x=.04,y=.25,z=1},intensity=1400,range=10,inner_angle=20,outer_angle=35,
 shadows=true,shadow_priority=4})
engine.property('local.time',0)
engine.property('local.phase',0)
