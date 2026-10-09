# Native path-tracing shader interface

The native backends use the same scene ABI on Metal and Vulkan.  The C++
declarations in `src/render_pt.hpp` are the source of truth.  Native shaders
are deliberately separate from the bgfx `.sc` pipeline; the presentation and
ImGui pipelines bind their own resources.

## Dispatch entry points

All compute entry points use an 8 x 8 x 1 workgroup.  Dispatch dimensions are
`ceil(width / 8)` by `ceil(height / 8)`.

| Source | Entry point | Purpose |
| --- | --- | --- |
| `pt_trace.metal`, `pt_trace.comp` | `pt_trace` | Jittered camera paths, TLAS traversal, NEE, multi-bounce GGX/diffuse transport, and guide output. |
| `pt_temporal.metal`, `pt_temporal.comp` | `pt_temporal` | Reprojection, guide rejection, history accumulation, moments and variance. |
| `pt_filter.metal`, `pt_filter.comp` | `pt_filter` | One edge-aware à-trous pass. Dispatch five times with pass 0 through 4. |
| `pt_post.metal`, `pt_post.comp` | `pt_post` | Exposure, ACES tone mapping, and a small bright-neighborhood bloom resolve. |

The trace pass averages `counts.x` Monte Carlo samples per pixel (up to the
shader's safety limit of 64). `sizeFrame.z` is the global sample offset used
to decorrelate progressive samples; each sample seed depends only on its pixel
and `sizeFrame.z + sampleIndex`. `counts.y` is the bounce limit. `options[0]` enables denoising, `options[1]` requests stationary
accumulation, `options[2]` is the previous accumulated sample count, and
`options[3]` is the number of instances in the TLAS.  `sizeFrame.w` resets
history.  Stationary history is unbounded; moving history is capped to eight
samples/frames by the temporal pass.

## Scene buffers

Metal uses `[[buffer(n)]]`; Vulkan uses descriptor set 0 and the same binding
number.  Storage is std430-compatible, with matrices represented as four
column-major `vec4` columns.  `PtParams` has 16-byte alignment throughout.

| Binding | Type | Contents |
| ---: | --- | --- |
| 0 | `PtParams` | Camera matrices, eye, sun/sky, post, dimensions, counters, and options. |
| 1 | `PtVertex[]` | `position`, `normal`, and linear `color`; `color.w` is the material index. |
| 2 | `uint[]` | Triangle indices local to each geometry range. |
| 3 | `PtInstance[]` | `model`, inverse and previous matrices, then `offsets = { indexOffset, vertexOffset, stableId, geometryIndex }`. |
| 4 | `PtMaterial[]` | `baseRoughness = { base.r, base.g, base.b, roughness }`; `emissionMetallic = { emission.r, emission.g, emission.b, metallic }`. |
| 5 | `PtLight[]` | `positionRange`, `colorIntensity`, `directionType`, `cone`; type 0 is point and type 1 is spot. `cone.x/y` are inner/outer cosine and `cone.z > 0.5` enables shadow-ray tests. `colorIntensity.w` is CPU-normalized by the authored spherical solid angle. |
| 6 | `PtEmitter[]` | `aArea.xyz`, `bCdf.xyz`, and `cPad.xyz` are world-space triangle vertices; `aArea.w` is area; `bCdf.w` is the cumulative selection CDF; `radiance.xyz` is radiance in nits and `radiance.w` is selection probability. |
| 7 | TLAS | Metal instance acceleration structure; Vulkan `accelerationStructureEXT`. Instance IDs are 0-based indices into `PtInstance[]`. |

`PtInstance.offsets` is interpreted exactly as four unsigned integers.  A hit
with primitive `p` fetches
`indices[indexOffset + 3*p + {0,1,2}] + vertexOffset` from the instance's
geometry.  The backend must publish the complete geometry and TLAS together;
the shaders never infer geometry from a neighboring chunk.

`sunDirectionIntensity.xyz` points from the sun toward the scene, so a ray to
the sun uses `-sunDirectionIntensity.xyz`.  Its `w` is irradiance scale.
`sunColorRadius.xyz` is sun color and `.w` is angular radius in radians; the
default is 0.27 degrees. The shader converts authored sun irradiance to disk
radiance using `pi * sin(radius)^2`. `skyColorIntensity` contains sky tint and
scale, including the authored below-horizon fill.
`post.x` is exposure multiplier and `.y` is bloom strength. `post.z` selects
the native diagnostic view: `0` final, `1` material albedo (unexposed sRGB),
`2` world normal, `3` diffuse history tone/exposure, `4` specular history
tone/exposure, `5` primary depth divided by 64, and `6` history length divided
by 8. Diagnostic views that are already display-encoded bypass bloom and the
final tone map.

## Storage images

Every image is a `rgba32f` storage image.  Metal uses texture indices 8
through 27, and Vulkan uses descriptor set 0 bindings 8 through 27.  The
indices are shared with `enum PtImage`.

| PtImage | Binding | Format and contents |
| ---: | ---: | --- |
| `PtRawDiffuse` | 8 | Current sample diffuse radiance, RGB; alpha 1. |
| `PtRawSpecular` | 9 | Current sample specular radiance, RGB; alpha 1. |
| `PtGuide` | 10 | World normal RGB and primary hit distance in alpha. |
| `PtAlbedo` | 11 | Linear base color RGB and roughness in alpha. |
| `PtMotion` | 12 | Previous-frame UV in XY, stable instance ID in Z, material ID in W. |
| `PtHistoryDiffuse` | 13 | Reprojected/accumulated diffuse radiance. |
| `PtHistorySpecular` | 14 | Reprojected/accumulated specular radiance. |
| `PtHistoryGuide` | 15 | Guide associated with the history sample. |
| `PtHistoryMotion` | 16 | Motion identity associated with the history sample. |
| `PtMoments` | 17 | Current luminance first and second moments in R/G. |
| `PtPreviousMoments` | 18 | Previous luminance moments in R/G. |
| `PtFilterA` | 19 | À-trous ping-pong image A. |
| `PtFilterB` | 20 | À-trous ping-pong image B. |
| `PtOutput` | 21 | Post-tonemapped RGBA output sampled by presentation. |
| `PtScratchA` | 22 | Final linear denoised radiance before post. |
| `PtScratchB` | 23 | Reserved scratch image; available to backend diagnostics. |
| `PtPreviousGuide` | 24 | Immutable previous-frame guide snapshot used for reprojection. |
| `PtPreviousMotion` | 25 | Immutable previous-frame motion/identity snapshot used for reprojection. |
| `PtPreviousDiffuse` | 26 | Immutable previous-frame accumulated diffuse snapshot. |
| `PtPreviousSpecular` | 27 | Immutable previous-frame accumulated specular snapshot. |

After each temporal dispatch, the backend copies history diffuse/specular/guide/
motion (bindings 13–16) to previous diffuse/specular/guide/motion (bindings
26–27, 24–25), respectively, and binding 17 to binding 18. Those snapshots
are then the immutable inputs for the next frame's temporal dispatch. The
temporal pass reads every historical signal at the reprojected UV and writes
bindings 13 through 17. This avoids a data race when neighboring pixels
reproject to one another. Moving history is capped to seven old samples plus
the current frame using the reprojected moment count; stationary accumulation
loads the previous pixel at the same coordinates, bypasses guide/motion
rejection, uses the host's unbounded sample count, and skips neighborhood
clamping. The filter pass reads the
history images for pass 0 and alternates bindings 19 and 20 for passes 1–4;
pass 4 writes binding 22. The post pass reads binding 22 and writes binding
21. The denoiser flag still keeps the history and moment images coherent;
when disabled, the filter is a pass-through resolve.

The filter's pass number is a single 32-bit constant.  Metal binds it as
`[[buffer(8)]]` for the filter pipeline; Vulkan supplies it in the push
constant block.  This does not overlap the scene buffer bindings because
filter dispatches do not bind the scene buffers.

## Presentation

`pt_present.metal`/`pt_present.vert` and `pt_present.frag` draw a fullscreen
triangle (`pt_blit.vert`/`pt_blit.frag` are equivalent Vulkan names used by
older backend builds).  The post output is sampled as a regular `rgba32f` texture at
texture/descriptor binding 0 with a clamp-to-edge linear sampler at sampler
binding 0.  The fragment writes the swapchain's native format.

## ImGui

The ImGui pipeline is independent of the path-tracing descriptor set.  Vertex
attributes are location 0 `vec2 position`, location 1 `vec2 uv`, and location
2 packed RGBA8 color.  Vulkan uses a push constant `mat4 projection` and a
combined texture sampler at descriptor binding 0.  Metal uses a constant
projection matrix at vertex buffer 2 and a 2D texture plus sampler at texture
and sampler index 0.  The Metal vertex shader unpacks the ImDrawList packed
color with the same RGBA byte order used by Dear ImGui.

## Transport details

The trace shaders implement opaque GGX metallic/diffuse mixtures.  The
specular lobe is sampled from the GGX NDF and the diffuse lobe cosine-weighted;
the returned PDF is the matching mixture PDF.  Sun, point/spot, cosine-sky,
and emissive-triangle samples use power-heuristic MIS against the BSDF PDF.
When a BSDF path lands on a published emissive triangle, the shader recovers
that triangle's area/selection PDF from the emitter table before adding its
emission, preventing direct-light double counting.
The sun is a uniform disk with a fixed angular radius from the parameter (the
default is 0.27 degrees); the sampled sky explicitly excludes that disk.
Environment misses apply the same MIS, including a camera miss where the
weight is one. Direct-light MIS is disabled at the terminal bounce because
there is no competing BSDF continuation sample. Russian roulette begins after
the third surface interaction, and the finite limit is clamped to twelve by
the host. A path with no TLAS instances skips
traversal and evaluates the sky/sun environment directly.

The trace pass stores diffuse and specular contributions separately.  It also
stores the primary normal/distance, albedo/roughness and previous projected
UV plus stable/material IDs needed by temporal rejection.  The post pass keeps
the existing exposure/bloom/tone-mapping stage after denoising.

## Validation

The Vulkan modules are validated with:

```sh
for f in assets/shaders/pt/*.comp; do
  glslangValidator --target-env vulkan1.2 -V -S comp -Iassets/shaders/pt \
    -o /tmp/$(basename "$f" .comp).spv "$f"
done
for f in assets/shaders/pt/*.vert; do
  glslangValidator --target-env vulkan1.2 -V -S vert \
    -o /tmp/$(basename "$f" .vert).vert.spv "$f"
done
for f in assets/shaders/pt/*.frag; do
  glslangValidator --target-env vulkan1.2 -V -S frag \
    -o /tmp/$(basename "$f" .frag).frag.spv "$f"
done
for f in /tmp/pt_trace.spv /tmp/pt_temporal.spv /tmp/pt_filter.spv /tmp/pt_post.spv; do
  spirv-val "$f"
done
```

On a full Xcode installation, compile the Metal sources into a single library
with `xcrun -sdk macosx metal -std=metal3.1 -Iassets/shaders/pt -c` for each
`.metal` source followed by `xcrun -sdk macosx metallib`.  The current
command-line-tools image does not ship the `metal` executable, so the Metal
backend's source-library fallback is the local validation path; the runtime
compiler reports source diagnostics when a device creates the pipeline.

The five à-trous passes use a 3×3 binomial kernel with increasing spacing.
This keeps native-resolution filtering bounded without dropping passes or rays.
