return {
  name = 'Quarry Demolition',
  mode = 'fps',
  objective = 'Remove 68% of the supported works while keeping 80% of the amber survey core intact.',
  player = {
    mode = 'first_person',
    build_material = 5,
    spawns = {
      {x = -6.5, y = 0.03, z = -6.0, yaw =  0.75, pitch = 0.0},
      {x = -2.2, y = 0.03, z = -6.0, yaw =  0.35, pitch = 0.0},
      {x =  2.2, y = 0.03, z = -6.0, yaw = -0.35, pitch = 0.0},
      {x =  6.5, y = 0.03, z = -6.0, yaw = -0.75, pitch = 0.0},
    },
    bounds = {
      min = {x = -9.0, y = -4.0, z = -9.0},
      max = {x =  9.0, y =  8.0, z =  9.0},
    },
  },
}
