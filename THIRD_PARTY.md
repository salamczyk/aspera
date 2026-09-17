# Dependency notices

The build fetches pinned source revisions or release tags. Upstream copyright and license notices are copied into `build/licenses` and into exported games.

| Dependency | Pinned version | Upstream |
| --- | --- | --- |
| SDL3 | release-3.2.10 | https://github.com/libsdl-org/SDL |
| bgfx build wrapper | 7cea9ab17c6dc8745cdc5df992145c514a129abb | https://github.com/bkaradzic/bgfx.cmake |
| bgfx | wrapper's pinned submodule 9b636df330c81e11c84595651a291b6c59fb7396 | https://github.com/bkaradzic/bgfx |
| Jolt | v5.3.0 / 0373ec0dd762e4bc2f6acdb08371ee84fa23c6db | https://github.com/jrouwe/JoltPhysics |
| ENet | v1.3.18 | https://github.com/lsalzman/enet |
| Lua | v5.4.8 | https://github.com/lua/lua |
| Dear ImGui | v1.91.9b | https://github.com/ocornut/imgui |

bgfx's wrapper also pins bx and bimg. Their bundled notices accompany the engine/toolchain notices. Lua's license is included in its distributed `lua.h` header.

VOX parsing follows the [base file format](https://github.com/ephtracy/voxel-model/blob/master/MagicaVoxel-file-format-vox.txt) and [scene extension format](https://github.com/ephtracy/voxel-model/blob/master/MagicaVoxel-file-format-vox-extension.txt). The engine and game samples are original implementation work, without Teardown assets or source code.
