# Shader performance evaluation 3

Date: 2026-09-17. Scope: all 40 files in `shaders/`. The review is static: nothing was compiled,
captured or timed, and every cost figure is an estimate from reading the shaders and the host code
that drives them (`src/render/vk_renderer.cpp`, `vk/window_target.cpp`, `scene_uniforms.cpp`,
`shadow.cpp`, `particle_data.*`, `fragment_data.*`). Earlier evaluations were not consulted.

Target: Radeon 8060S (RDNA 3.5, 40 CU) on unified LPDDR5X at roughly 256 GB/s shared with the CPU,
32 MB MALL. The HDR target is `R16G16B16A16_SFLOAT`, so every full-screen read or write of it is
8 B per pixel. On this machine bandwidth is usually the thing that runs out first, and several of
the larger findings below are about bytes, not instructions.

"Exact" below means the output is bit-identical or differs only by float rounding (a few ULP).
"Approximate" means the output can change measurably, and each such item says how.

---

## 1. Where to start

Ranked across the whole frame, not per file. Impact assumes the phase of the cycle in which the
shader actually runs.

| # | Change | Files | Equivalence | Impact |
|---|--------|-------|-------------|--------|
| 1 | Draw the sky last among opaques, at depth 1.0 with the depth test on | `sky.vert`, host | Exact | High |
| 2 | Share lattice hashes in the dune normal (1a), or use analytic derivatives (1b) | `ground.frag`, `noise.glsl` | 1a exact / 1b approx | High |
| 3 | Replace the 9-tap shadow PCF with 4 fetches (gather or weighted bilinear) | `shadow.glsl` (5 callers) | Exact to ~1/256 | High |
| 4 | Skip `KeyShadow` where `lambert * keyAbove == 0` | building, board, ground, fragment | Exact | High (buildings) |
| 5 | Light the particle dust per vertex, not per pixel | `particle.vert/.frag` | Approx | High |
| 6 | Shrink particle sprites to their visible radius, hexagon strip, cull `alpha <= 0.002` | `particle.*`, `particle_count/scatter.comp` | Exact | High |
| 7 | Fold the exposure histogram into bloom level 0 | `bloom_down.comp`, `exposure_histogram.comp`, host | Exact (even sizes) | High at 4K |
| 8 | Debris: one non-instanced draw instead of N instances of 3 vertices | `fragment.vert`, `shadow_fragment.vert`, host | Exact | Med–High |
| 9 | Bloom chain in `B10G11R11_UFLOAT` | `bloom_*.comp`, host | Approx | Med–High |
| 10 | `bloom_up` 3×3 tent in 4 bilinear fetches | `bloom_up.comp` | Exact to ~1/256 | Med |
| 11 | Skip the debris sim before the blast; skip settled fragments | `fragment_sim.comp`, host | Exact | Med |
| 12 | Early-out in `CelestialBody` when `cosAngle <= 0.6` | `atmosphere.glsl` | Exact | Med (sky) |

Items 1, 3, 4, 6, 7, 8, 11 and 12 change nothing on screen and are the safest place to begin.

---

## 2. Cross-cutting findings

### 2.1 `KeyShadow`: 9 filtered taps → 4 fetches (`shadow.glsl:34-40`)

This is the most widely shared cost in the renderer. Ground, building, board, debris and particle
fragment shaders all call it, and it is 9 `image_sample_c` plus address math per receiving pixel.

The 3×3 grid of bilinear-compare taps at whole-texel spacing is, in texel space, a separable filter
over a 4×4 block with per-axis weights `[1-f, 1, 1, f] / 3`, where `f = fract(uv*size - 0.5)`.
Two reviewers independently arrived at 4-fetch forms that reproduce it:

**(a) Four compare-gathers, weights in ALU.** No weight quantisation, about 20 extra ALU.

```glsl
vec2 f = fract(uv / scene.shadow.x - 0.5);
vec4 a = textureGatherOffset(shadowMap, uv, depth, ivec2(-1,-1));
vec4 b = textureGatherOffset(shadowMap, uv, depth, ivec2( 1,-1));
vec4 c = textureGatherOffset(shadowMap, uv, depth, ivec2(-1, 1));
vec4 d = textureGatherOffset(shadowMap, uv, depth, ivec2( 1, 1));
// gather component order: w=(x0,y0) z=(x1,y0) x=(x0,y1) y=(x1,y1)
vec4 wx = vec4(1.0 - f.x, 1.0, 1.0, f.x);
float sum = (1.0 - f.y) * dot(vec4(a.w, a.z, b.w, b.z), wx)
          +               dot(vec4(a.x, a.y, b.x, b.y), wx)
          +               dot(vec4(c.w, c.z, d.w, d.z), wx)
          +         f.y * dot(vec4(c.x, c.y, d.x, d.y), wx);
return sum * (1.0 / 9.0);
```

**(b) Four bilinear-compare taps at fractional offsets.** Each pair of weights is produced by one
hardware-filtered tap placed between two texels and scaled.

```glsl
vec2 st = uv / scene.shadow.x - 0.5, base = floor(st), f = st - base;
vec2 w0 = 2.0 - f, w1 = 1.0 + f;
vec2 p0 = (base - 0.5 + 1.0 / w0) * scene.shadow.x;
vec2 p1 = (base + 1.5 + f / w1)   * scene.shadow.x;
float sum = w0.x*w0.y * texture(shadowMap, vec3(p0,               depth))
          + w1.x*w0.y * texture(shadowMap, vec3(p1.x, p0.y,       depth))
          + w0.x*w1.y * texture(shadowMap, vec3(p0.x, p1.y,       depth))
          + w1.x*w1.y * texture(shadowMap, vec3(p1,               depth));
return sum * (1.0 / 9.0);
```

I checked both weight derivations by hand and both reproduce `[1-f, 1, 1, f]`. Form (a) is the
closer match: today's hardware bilinear weights are 8-bit sub-texel fixed point, and (b) moves the
tap positions off the grid, so its edge values can differ by about 1/256 per tap. Either way the
fetch count drops from 9 to 4, about 55 %, in five shaders. The white border and the out-of-map
early-out at line 29 are unaffected.

The debris reviewer also suggested 4 taps at ±1 texel. That form is **not** equivalent: it
averages a 4×4 block as a box rather than weighting it by `[1-f,1,1,f]`, which softens shadow edges
visibly. Use (a) or (b), not that.

Smaller exact trims in the same function:
- `shadow.glsl:24`: `clip.w` is exactly 1 because `lightViewProj` is orthographic (`shadow.cpp:70-73`).
  `ndc = clip.xyz` saves a reciprocal and three multiplies per pixel. Comment it, because the
  change is exact only while the host keeps the projection orthographic.
- `shadow.glsl:23`: the transform is affine and, per face, the normal offset is constant. The
  building, board and ground vertex shaders can emit `lightViewProj * vec4(world + n*bias, 1)` as
  a varying and drop a per-pixel `mat4×vec4` (about 16 FMA). Approximate to rounding. It needs a
  `KeyShadow` overload that takes the light-space position.

### 2.2 Skip the shadow lookup where direct light is zero

`building.frag:95-96`, `board.frag:44-45`, `ground.frag:84`. `KeyShadow` is multiplied by
`lambert * keyAbove`. On a box, `lambert` is constant per face, so about half of the visible wall
area sees a zero there whenever the sun is up. At night `keyAbove` is zero everywhere, but that is
only true for buildings and board: the ground reviewer found the twilight key never drops below
-2°, so `keyAbove` in `ground.frag` never reaches 0.

```glsl
float direct = lambert * keyAbove;
vec3 shaded = vec3(0.0);
if (direct > 0.0)
    shaded = albedo * scene.keyColor.rgb * direct * KeyShadow(vWorldPos, normal);
```

Inside `KeyShadow`, switch `texture(...)` to `textureLod(..., 0.0)`, because the call now sits in
non-uniform control flow and implicit derivatives there are undefined. The map has one mip, so
LOD 0 is what is sampled today. Exact: `KeyShadow` returns a finite value in [0, 1], so the dropped
product was `x * 0`. The branch is coherent per face on buildings. On the ground it helps only in
twilight cycles, because dune perturbation breaks up the unlit region.

### 2.3 Uniform-only math evaluated per pixel

Every surface shader recomputes values that depend only on the scene UBO:
- `normalize(vec3(keyDir.x, 0, keyDir.z))` in `SkyGradient` (`atmosphere.glsl:31-32`)
- `smoothstep(-0.08, 0.06, keyDir.y)` (`ground.frag:82`, `building.frag`)
- `skyAmbient = max(ambient, mix(horizon, zenith, 0.7))`
- `pow(night, 1.5)`
- `max(boardColor.w², 1)` used as a divisor
- `cos(bodyRadius)` and `cos(bodyRadius*1.06)`
- the `2.2 / max(horizonColor.w, 1e-3)` factors

These cost roughly 15–30 VALU and a couple of transcendentals per pixel, depending on the shader.
Some drivers scalarise part of this, but not reliably.

**Spare slots are limited.** Several reviewers independently proposed `scene.timing.zw` and
`scene.bodyColor.w` (both written as 0 today, `scene_uniforms.cpp:39,46-47`). They cannot all
have them. Add one `vec4 derived[2]` to the scene UBO, mirrored in `scene_uniforms.h`, rather than
competing for the two spare floats. Folding `keyAbove` into `keyColor` is also tempting. It is
safe only if every reader of `keyColor` currently multiplies by `keyAbove`, so check before doing
it. Exact where an expression moves to the CPU unchanged; ≤ 1–2 ULP where products are reassociated.

### 2.4 `fire.glsl:34-41`: one rsq instead of sqrt + 2 reciprocals

This runs in six passes (ground ×2, building, board, debris, particle, missile), including under
particle overdraw.

```glsl
float dist2   = dot(toFire, toFire);
float invD    = inversesqrt(max(dist2, 1e-6));
float falloff = min(1.0, radius * radius * invD * invD);   // == r² / max(d², r²)
float wrap    = 0.5 + 0.5 * dot(normal, toFire) * invD;
```

Exact to rounding, except within 1 mm of the fireball centre. Low impact per call. The uniform
`radius <= 0` early-out at line 32 is already correct.

### 2.5 Varying hygiene

Per-face and per-instance values are interpolated with perspective correction in several places:
- the city's `vBodyColor`, `vWindowColor`, `vWindowSeed` and `vNormal`
- the board's `vLit` and `vIsSegment`
- the debris's `vNormal` and `vColor`

Each interpolated component costs about two VALU on RDNA3 and occupies attribute-ring space. Mark
them `flat` and pack them into vec4s. The building VS/FS goes from 6 locations to 4. Where a
normal becomes a flat per-face constant that is already unit length, the fragment `normalize()`
can go too (`fragment.frag:26`, `building.frag`). Within float rounding, since interpolating three
equal values is not bit-exact today either.

---

## 3. Sky, ground, atmosphere (`sky.*`, `ground.*`, `atmosphere.glsl`, `scene.glsl`)

### 3.1 [High] The sky is shaded under every ground and building pixel. Exact.
`vk_renderer.cpp:552-554` creates the sky pipeline with depth test and write both off. Lines
1471-1472 draw it first, and the range, terrain and city then overwrite it (confirmed). That
is 50–60 % of a frame's pixels shaded and written at 8 B each for nothing. At 3440×1440 it is
about 2.5–3 M invocations and 20–24 MB of colour traffic per frame.

```glsl
// sky.vert:11
gl_Position = vec4(vNdc, 1.0, 1.0);
```
Create the pipeline with depth test on and write off. With the existing `LESS_OR_EQUAL` against a
1.0 clear, the sky passes exactly where nothing opaque was drawn. Move its draw to after all
opaque passes (ground, city, board, missile, debris) and before the particles. `sky.frag` has no
discard or depth write, so HiZ and early-Z reject the covered pixels. All blended passes still come
after it, so the image does not change. The comment at line 550 will need rewriting.

### 3.2 [High] `ground.frag:60-73`: the dune normal is ~90 % of the ground shader
Three forward-difference height samples, each `Fbm2(·,3) + Fbm2(·,2)`, add up to 15
`GradientNoise2` calls and 60 `Hash22`, roughly 1,300–1,500 VALU per sand pixel. The distance fade
does not limit it much: `exp(-d/1300) > 0.002` holds to about 8.1 km, which is the whole basin.

- **1a. Exact: share lattice corners.** `p+(e,0)` and `p+(0,e)` move less than one cell per octave
  (0.018–0.24 cells), in one axis each. The union of corners is 8, not 12. Evaluate the three
  noises together with `vec3 GradientNoise2x3(p, px, pz)`, selecting each shifted sample's corners
  by `floor(px.x) > i.x`. Hash inputs are bit-identical, and the saving is about 240 VALU/px
  (15–18 %) for ~16 extra VGPRs. `Corner4` is today's `GradientNoise2` body from `noise.glsl:24`
  onward, with the four gradients passed in; an `Fbm2x3` loops it with the same `*= 2.03` and
  `amp *= 0.5`.
  ```glsl
  vec3 GradientNoise2x3(vec2 p, vec2 px, vec2 pz) {
      vec2 i = floor(p);
      vec2 g00 = Hash22(i)             * 2.0 - 1.0, g10 = Hash22(i + vec2(1,0)) * 2.0 - 1.0;
      vec2 g01 = Hash22(i + vec2(0,1)) * 2.0 - 1.0, g11 = Hash22(i + vec2(1,1)) * 2.0 - 1.0;
      vec2 g20 = Hash22(i + vec2(2,0)) * 2.0 - 1.0, g21 = Hash22(i + vec2(2,1)) * 2.0 - 1.0;
      vec2 g02 = Hash22(i + vec2(0,2)) * 2.0 - 1.0, g12 = Hash22(i + vec2(1,2)) * 2.0 - 1.0;
      bool sx = floor(px.x) > i.x, sz = floor(pz.y) > i.y;
      float n  = Corner4(p,  i,         g00, g10, g01, g11);
      float nx = Corner4(px, floor(px), sx ? g10 : g00, sx ? g20 : g10, sx ? g11 : g01, sx ? g21 : g11);
      float nz = Corner4(pz, floor(pz), sz ? g01 : g00, sz ? g11 : g10, sz ? g02 : g01, sz ? g12 : g11);
      return vec3(n, nx, nz);
  }
  ```
- **1b. Approximate, bigger: analytic derivatives.** Only `hx-h` and `hz-h` are used, so the height
  itself is dead. Gradient noise with an analytic derivative needs 5 evaluations and 20 hashes
  instead of 60. That is about 3× cheaper for the block, roughly 900–1,000 VALU/px saved. It
  replaces a 1.5 m secant with a tangent: identical on the coarse octaves, and up to tens of
  percent different in local slope on the fine ripple octaves. Same pattern, slightly different
  ripple contrast.
  ```glsl
  vec3 GradientNoise2D(vec2 p) {            // (value, d/dx, d/dy) * 1.414
      vec2 i = floor(p), f = p - i;
      vec2 u  = f*f*f*(f*(f*6.0-15.0)+10.0);
      vec2 du = 30.0*f*f*(f*(f-2.0)+1.0);
      vec2 ga = Hash22(i)*2.0-1.0,           gb = Hash22(i+vec2(1,0))*2.0-1.0,
           gc = Hash22(i+vec2(0,1))*2.0-1.0, gd = Hash22(i+vec2(1,1))*2.0-1.0;
      float va = dot(ga,f), vb = dot(gb,f-vec2(1,0)), vc = dot(gc,f-vec2(0,1)), vd = dot(gd,f-1.0);
      float k = va - vb - vc + vd;
      vec2  d = ga + u.x*(gb-ga) + u.y*(gc-ga) + u.x*u.y*(ga-gb-gc+gd) + du*(u.yx*k + vec2(vb,vc) - va);
      return 1.414 * vec3(va + u.x*(vb-va) + u.y*(vc-va) + u.x*u.y*k, d);
  }
  ```
- **1c. Approximate, below quantisation: raise the cutoff** at line 59 from 0.002 to 0.02. The
  largest normal tilt dropped is about 0.0026 rad, under 0.3 % Lambert and under one 8-bit step
  after tonemapping. The block then stops at ~5.1 km instead of ~8.1 km.

### 3.3 [Med] `atmosphere.glsl:72`: `CelestialBody` early-out. Exact.
Every sky pixel pays two `cos`, a smoothstep with a divide, and `pow(x, 220)`.
```glsl
float cosAngle = dot(dir, keyDir);
if (cosAngle <= 0.6) return vec3(0.0);
```
`0.6^220 ≈ 2^-162` underflows to 0 in fp32, and `limb` is 0 whenever `cosAngle < cos(1.06·r)`,
which is ~0.999 for the body radii used. Both terms are therefore already exactly 0 there. The
branch is coherent (one small disc) and saves roughly 20–25 % of `sky.frag`. Hoist the two `cos`
to the CPU (see 2.3) for the pixels that do reach the body.

### 3.4 [Low–Med] `sky.vert`: move the `ViewRay` unprojection to the vertex stage
For a perspective projection, `far.w` of `invViewProj * vec4(ndc, 1, 1)` is the same positive
constant across the screen. So `far.xyz - cameraPos * far.w` is linear in NDC and interpolates
correctly, and `normalize` removes the scale. This saves a per-pixel `mat4×vec4`, a divide and a
subtract (~20 VALU) for one vec3 varying. Approximate to rounding. The same trick applies to the
tonemap's blast refraction ray (§7.5).

### 3.5 Smaller ground and sky items
- **`ground.frag:106-112`** runs the board-light term (`length`, 2 divides) even when
  `boardLight.w == 0`, which is most of the cycle. Wrap it in `if (scene.boardLight.w > 0.0)`.
  Exact, uniform branch, ~15 VALU.
- **`SkyGradient` for ground pixels** (`ground.frag:130`). `-viewDir` points down, so `elevation = 0`,
  `pow(0, 0.42) = 0`, `mix(h,z,0) = h` and `exp(0) = 1`, all exactly. A `dir.y <= 0` branch
  returning `horizon + horizon * t4 * 1.6` is exact and skips log2 plus two exp2.
- **`sky.frag:33-35`:** a below-horizon early-out after the `fwidth`. Only worth it if 3.1 is not
  adopted.
- **`atmosphere.glsl:33`:** `pow(t, 4.0)` becomes `t2*t2`. The driver probably does this already,
  and writing it explicitly costs nothing.

Already fine: `sky.vert` (attribute-less triangle), `ground.vert`, the `Stars` uniform early-out
and 98 % reject, `HazeAmount`, the range-before-terrain draw order, the rockiness and detail gates,
and both `normalize` calls in `ground.frag` (both are needed).

---

## 4. Buildings and board (`building.*`, `building_common.glsl`, `shadow_building.vert`, `board.*`)

The city is 500 instances of a 24-vertex cube, drawn twice per frame (shadow at
`vk_renderer.cpp:1435`, HDR at `:1489`). Vertex work is negligible. The fragment shader is what
costs: 9 shadow taps, ~150–200 ALU, 14 interpolated floats.

1. **[High] Shadow skip on unlit faces**: §2.2. With §2.1, this removes 75–100 % of this shader's
   shadow sampling.
2. **[Med] Varyings**: §2.5. Pack them into 4 locations, 3 of them `flat`.
3. **[Med] Move per-instance math out of the fragment shader** (`building.frag:36-47, 88, 143`).
   This covers the three `fract(seed*k)` calls, the two divides for `grid`, `vWindowColor*0.35`
   and `normalize(vNormal)`. Emit `gridX = facadeU / bay` from the VS; it is linear across the
   face, so perspective-correct interpolation reproduces the divide. Also emit `1/floorH` as a flat
   varying and the pre-scaled glass colour. Saves ~15–20 VALU including 2 divides and an rsq.
   Rounding can move `floor(grid)` across a cell edge only where the mullion already covers the
   edge (`lo = 0.20`, `hi ≤ 0.82`), so no visible pixel changes.
4. **[Med] Skip the pane pattern where it is dissolved** (`building.frag:56, 70, 78-83`). At about
   3 km most of the city has `resolved == 0`, and `mix()` discards the pattern. Keep `fwidth`
   above the branch, then compute `pane` and `WindowHash` only when
   `facade > 0.0 && resolved > 0.0`. Exact, since `mix(a, b, 0) == a` for finite `b`. Saves ~30
   VALU on the distant majority.
5. **[Low–Med] Front-to-back instance order.** The 500 opaque boxes are drawn in generator order,
   so early-Z only helps by luck. Precompute a few azimuth-bucketed orders (the camera orbits), or
   sort 500 keys on the CPU per frame. Exact, and typically 1.5–3× less shading overdraw inside the
   skyline.
6. **[Low] Drop the underside face** (`indexCount = 30` for the city, in both passes). Buildings
   sit on y = 0, so the face never faces the camera. In the shadow pass this is exact only while the
   key light is above the horizon. With the key at -0.08…0 the underside was the light-facing
   silhouette and the stored depth changes. Keep 36 for the board.
7. **[Low] Micro:** `pow(1-t, 3.0)` → `u*u*u` (`building_common.glsl:46`, per vertex). Use one
   `inversesqrt(dot)` for both the distance and the view direction (`building.frag:32-34`,
   `board.frag:27-29`). Board light: use `max(dot(n, toBoard), 0) / max(dist, 1e-3)` instead of
   normalising first.

Leave alone: `shadow_building.vert` (the dead inputs are not fetched on AMD, there is no fragment
stage, and slope bias is in the pipeline). Also leave the hidden-instance `z = -1` rejection, the
instance formats (≤ 32 KB, cache-resident) and the board VS. Do not change `length(d) <= w` to
`dot(d,d) <= w*w` in `building_common.glsl:39` / `board.vert:34` alone: it must stay identical to
`fragment_sim.comp`, so either all three change or none.

---

## 5. Debris fragments (`fragment.*`, `fragment_common.glsl`, `fragment_init/sim.comp`, `shadow_fragment.vert`)

N ≈ 150k fragments (~218k ceiling at quality 0). `FragRest` and `FragState` are 64 B each. Two
draws of `vkCmdDraw(3, N)` and a sim dispatch every frame. ALU is not the constraint here; wave
packing and bytes are.

1. **[Med–High] One non-instanced draw instead of N 3-vertex instances**
   (`fragment.vert:18,34`, `shadow_fragment.vert:14,26`, host `:1444, :1554`). AMD's front end
   packs vertex waves poorly for instances this small, possibly 3 live lanes per wave for about
   1 M vertex invocations per frame.
   ```glsl
   uint i = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) - i * 3u;
   ```
   Then `vkCmdDraw(cmd, 3 * N, 1, 0, 0)`. Exact: same vertices, same order. Confirm in RGP, but
   there is no downside.
2. **[Med] Stop running the sim when nothing can change** (`fragment_sim.comp:30-51`, host
   `:2196`). Before the blast (`ShellRadius == 0`), every invocation reads 144 B and **writes**
   `state[i].pos` (line 49), a value that init already wrote. Settled fragments are read and
   rewritten in full every frame. Skip the dispatch while `ShellRadius == 0`. In the shader, return
   early for settled fragments outside gather, and for intact fragments test the uniform
   `fp.blast.w` before the dependent `boxes[]` fetch. Drop the redundant write. Exact. It saves
   roughly 22–31 MB per frame for the whole pre-blast part of every cycle.
3. **[Med] Split hot and cold fields.** The draws need 32 of 64 B of state and 48 of 64 B of rest;
   the sim needs only 16 B of rest. Split into `pose[]` (pos, quat), `dyn[]` (vel, spin),
   `corner[]` and `anchor[]`. Total traffic falls ~30 %, and the hot draw set (80 B × N ≈
   12–17 MB) then fits in the 32 MB MALL between the shadow and main passes. The shadow VS can fetch
   only its own corner. `spin.w` (always `FragHash(i,101u)`) can be recomputed, and `vel.w` is
   written but never read, despite the comment at `fragment_common.glsl:36`. Exact.
4. **[Low–Med] Gather swirl** (`fragment_sim.comp:151-153, 175`). It computes `atan` then
   `sin`/`cos` to rebuild a direction it already has as `pos.xz / |pos.xz|`. Use one
   `inversesqrt`, saving ~30–40 ALU per fragment per gather frame. Approximate to rounding, and it
   makes `pos.xz = 0` defined.
5. **[Low] Load `rest` after the intact early-out** in both vertex shaders
   (`fragment.vert:20-26`, `shadow_fragment.vert:16-21`). Exact.
6. **[Low] `MushroomTarget` computed twice** per disperse frame (`:94` and `:141`) with different
   `grow`. Add a Y-only helper for the release test. Exact.
7. **[Low, changes the pattern] Six hashes per gather frame** could become one or two with bit
   splitting. The cloud pattern changes, though the statistics stay the same. Skip it if
   `selftest_fragments.cpp` mirrors these hashes.

Rejected: precomputing world-space corners in the sim (net traffic rises, and it saves ALU that
isn't the bottleneck), and storing the rest normal (bytes for ALU, the wrong trade here).
`local_size_x = 64`, the all-vec4 std430 structs, quaternion rotation and the `z = -1` intact
rejection are all already right.

---

## 6. Particles (`particle.*`, `particle_common.glsl`, `particle_count/prefix/scatter.comp`)

73,000 slots at quality 0. Two draws of `vkCmdDraw(6, 73000)` (876k vertex invocations) regardless
of how many particles are alive. Dust sprites are tens of pixels across at the orbit distance, so
fragment work is on the order of 10⁷–10⁸ blended invocations per frame. Compute and vertex are tens
of microseconds; fragments dominate by one to two orders of magnitude.

1. **[High] Light the dust in the vertex stage** (`particle.frag:43-102`). Every dust pixel runs
   the full lighting rig on a sprite with peak alpha 0.105–0.14 that is 25–190 m across:
   `KeyShadow` (9 taps), `HighAirLight`, `FireIrradiance`, the board light, `SkyGradient` and
   `HazeAmount`. Move it into a `DustLight()` function evaluated per corner in `particle.vert`, and
   pass one colour. The fragment shader keeps only the radial falloff. This needs
   `VK_SHADER_STAGE_VERTEX_BIT` on the shadow-map binding (`vk_renderer.cpp:953`). It removes ~90 %
   of the fragment ALU and all its texture fetches. Approximate: the only visible difference would
   be a shadow edge crossing a single puff. If that is too lossy, keep per-pixel lighting and apply
   §2.1.
2. **[High] Rasterise only what survives** (all exact):
   - Cull sprites with `p.alpha <= 0.002`, since `alpha ≤ vAlpha`. Do it in the VS and in the
     count/scatter tests, which currently use `<= 0.0`.
   - Scale each sprite's corners by `rMax = sqrt(1 - sqrt(0.002 / p.alpha))`, the radius where the
     falloff crosses the discard threshold.
   - Replace the quad with a circumscribed hexagon drawn as a 6-vertex triangle strip. That is 13 %
     less area and the same vertex count; each instance restarts the strip. It needs
     `TRIANGLE_STRIP` for the two particle pipelines (`vk_renderer.cpp:317`).
   ```glsl
   const float R = 1.1547005; // 2/sqrt(3)
   const vec2 kHex[6] = vec2[6](vec2(-R,0), vec2(-R*.5,-1), vec2(-R*.5,1),
                                vec2(R*.5,-1), vec2(R*.5,1), vec2(R,0));
   ```
   Together these cut roughly 25–40 % of fragment invocations and blends over a sprite's life, and
   far more during fades (at α = 0.005 only 37 % of the disc is visible).
3. **[Med] Indirect draws.** Have the prefix pass write two `VkDrawIndirectCommand`s (alpha count,
   additive count) so dead slots are never instanced. That removes ~400–800k wasted vertex
   invocations and the 584 KB `0xFFFFFFFF` fill. Even without it, fill `2*total*4` bytes instead of
   `VK_WHOLE_SIZE` (`vk_renderer.cpp:2070`), which is 4.5× too much at quality 3. Exact.
4. **[Med] Specialise the emitter pipeline.** One `.frag` serves both pipelines, so the additive
   pipeline's VGPR allocation, and with it occupancy, is set by dust lighting it never runs. Use a
   `constant_id` bool. Exact.
5. **[Low–Med] `ParticleAt` runs 6× per instance** with divergence, because after depth sorting a
   wave mixes systems. Have scatter write a 32 B per-frame record (`pos, size, tint, alpha`) that
   the VS just reads (~2.3 MB/frame). Exact at fp32.
6. **[Low] Shared-memory ranking in count/scatter** with `local_size_x = 256` and one global
   atomic per occupied bucket, cutting global atomics 3–8×. Exact, since order within a bucket was
   already arbitrary.
7. **[Low] Subgroup scan in `particle_prefix.comp`** instead of Hillis–Steele (16 barriers). It
   needs `glslc --target-env=vulkan1.1` or later; the Makefile (line 96) uses the 1.0 default.
   Exact.

Already right: stateless closed-form particles, the 256-bucket counting sort, workgroup sizes,
`vkCmdFillBuffer` clears, the push-constant parameter block, depth test on with write off, and
z = 2 rejection of dead instances.

---

## 7. Post-processing (`bloom_*.comp`, `exposure*`, `tonemap.frag`, `fullscreen.vert`, `probe.comp`)

Figures at 3840×2160. A full HDR read is 66 MB, about 0.26 ms at peak bandwidth.

1. **[High] Fold the histogram into `bloom_down` level 0.** `exposure_histogram.comp` reads the
   HDR image again right after bloom level 0 has read it. `texelFetch(2*gid)` only samples a
   quarter of the pixels, but on a tiled 64-bpp surface it still touches nearly every cache line,
   and 66 MB does not fit in the MALL. Inside `bloom_down` when `firstLevel` is set (a dynamically
   uniform push constant), bin `texelFetch(uSource, texel*2)` into a shared 256-bin histogram and
   merge the non-zero bins. That texel sits under tap `e`, so the fetch hits in L0/L1. The group
   becomes 16×16 so each lane owns one bin, the early `return` becomes an `inside` flag, and the
   exposure SSBO is added to the level-0 bloom set. Delete the histogram dispatch. Saves about
   35–66 MB per frame (0.15–0.26 ms) plus one dispatch and barrier. Exact for even swapchain sizes;
   on odd sizes the edge row or column sampled can shift by one.
2. **[Med–High] `B10G11R11_UFLOAT_PACK32` for the bloom chain** (`window_target.cpp:390, 417`).
   Alpha is written as a constant 1.0 that nothing reads. The chain moves about 88 MB per frame
   at 4K; this halves it. It needs `shaderStorageImageExtendedFormats` (no device features are
   enabled today, `context.cpp:196`) and a format-properties check for `STORAGE_IMAGE_BIT`.
   Approximate: the mantissa drops to 6/6/5 bits, accumulated over five up steps. The result is
   blended at 0.08 and dithered, so it is probably invisible, but check a capture for blue banding.
3. **[Med] `bloom_up` 3×3 tent in 4 bilinear fetches** (`bloom_up.comp:33-44`). Per axis the nine
   taps reduce to texel weights `(1-a, 2-a, 1+a, a)/4`, which pair into two bilinear taps:
   ```glsl
   vec2 size = vec2(textureSize(uSource, 0)), inv = 1.0 / size;
   vec2 p = uv * size - 0.5, k = floor(p), a = p - k;
   vec2 w0 = 3.0 - 2.0 * a, w1 = 1.0 + 2.0 * a;
   vec2 t0 = (k - 0.5 + (2.0 - a) / w0) * inv;
   vec2 t1 = (k + 1.5 + a / w1) * inv;
   vec3 sum = texture(uSource, t0).rgb * (w0.x * w0.y)
            + texture(uSource, vec2(t1.x, t0.y)).rgb * (w1.x * w0.y)
            + texture(uSource, vec2(t0.x, t1.y)).rgb * (w0.x * w1.y)
            + texture(uSource, t1).rgb * (w1.x * w1.y);
   sum *= 1.0 / 16.0;
   ```
   I checked the weights by hand. The layout comes from the real sample position, so it holds on
   odd floor-halved levels too. Clamp-to-edge acts per texel address in both forms, so edges match.
   The only error is 8-bit sub-texel weight quantisation. Small levels gain close to 2×; the
   1→0 step stays bound by its read-modify-write.
4. **[Low] Histogram same-bin fast path.** On a flash white-out every lane hits one LDS bin. Use
   `subgroupAllEqual(bin)` and have one lane add `gl_SubgroupSize`. Exact, ~20 µs only on those
   frames.
5. **[Low] Tonemap refraction.** The shell's projected screen centre (`tonemap.frag:112-114`) is
   the same for every pixel, so compute it on the CPU. Optionally emit the `invViewProj` far-point
   from `fullscreen.vert` as in §3.4. Exact to rounding. It runs only during the blast.
6. **[Low] `exposure_adapt.comp`:** replace the shared-memory tree with `subgroupAdd` partials.
   The float summation order changes, which is negligible.
7. **[Low] Deep bloom mips:** 11 dispatches, each followed by a full barrier; level 5 at 4K is
   8×5 groups. An SPD-style fused tail is possible but awkward with the 13-tap ±2 border. Do it only
   if the frame turns out to be latency-bound.

Rejected: LDS tiling for `bloom_down` (it cuts fetch instructions, not DRAM bytes). Folding the
last upsample into the tonemap (roughly break-even, and it changes the output). `texelFetch` in the
tonemap (`vUV` is already a pixel centre). Changing the sRGB encode (the dither must come after it,
and the cost hides under the bandwidth).

Already right: the 13-tap `bloom_down` kernel (it cannot be done in fewer bilinear taps), the
shared-memory histogram with non-zero merge, the ACES rational fit, and the fullscreen triangle.
**`probe.comp` is never dispatched** (not referenced by the renderer or `vk_probe.cpp`), so it
costs nothing at runtime.

---

## 8. Fireball, flash, missile, noise (`fireball.*`, `flash.frag`, `missile.*`, `fire.glsl`, `noise.glsl`)

Nothing here ray-marches. The fireball is a displaced 40×72 sphere drawn additively for ~10 s of an
80–115 s cycle; the flash is 0.3 s per cycle.

1. **[Med] `fireball.frag:31`: 3-octave 8-hash `Fbm3` per pixel** (~560 VALU). This is the only
   per-pixel 3D noise in the renderer: about 0.08 ms at 25 % of 2560×1600, and up to ~0.6 ms when
   the fireball fills a 4K screen. Options:
   - **A 32³ `R16_UNORM` hash texture** (64 KB, stays in L2/MALL), sampled with quintic-warped
     coordinates so the trilinear filter reproduces the quintic interpolation:
     ```glsl
     float ValueNoise3Tex(vec3 p) {
         vec3 i = floor(p), f = p - i;
         vec3 u = f*f*f*(f*(f*6.0 - 15.0) + 10.0);
         return textureLod(uNoise3D, (i + u + 0.5) * (1.0/32.0), 0.0).r * 2.0 - 1.0;
     }
     ```
     About 15 ALU plus 1 fetch per octave instead of ~185. Approximate: the pattern tiles every 32
     cells, and filter weights quantise to 256 steps per cell, which may show faint banding on a
     large fireball.
   - **Cheaper and ALU-only:** drop to 2 octaves (the third contributes ±0.0375 of `heat`, a 33 %
     saving), and/or use a cubic fade (`noise.glsl:52-54` notes this noise is never
     differentiated). Both slightly change the look.
2. **[Low–Med] The back hemisphere is rasterised only to be discarded** (`fireball.frag:24-25`, no
   culling at `vk_renderer.cpp:598`). Cull whole back-facing triangles in the VS with
   `gl_CullDistance`, which keeps the handedness independence the comment wants:
   ```glsl
   vec3 toCam = scene.cameraPos.xyz - vWorldPos;
   gl_CullDistance[0] = dot(dir, toCam) * inversesqrt(dot(toCam, toCam)) + 0.05;
   ```
   This needs `shaderCullDistance` enabled at device creation (it is not today). It halves the
   fireball's rasterised fragments. It is effectively exact, because only pixels whose alpha is
   already ~0 could change.
3. **[Low] `flash.frag`:** a full-screen RGBA16F read-modify-write (~130 MB at 4K) for 0.3 s per
   cycle. Adding the flash analytically in the three HDR readers (histogram, bloom level 0,
   tonemap) is exact but spreads the logic across files; do it only if flash frames spike. The
   `if (flash <= 0.0) discard;` at lines 17-18 is dead, because the host gates the draw on the same
   value; removing it is exact.
4. **[Low] `fireball.vert`:** a non-indexed draw runs each grid vertex's 4-octave `Fbm3` about 5.8×
   (17,280 invocations for 2,993 unique vertices). A static index buffer with `gl_VertexIndex →
   (ring, seg)` fixes it. Exact, but only a few microseconds.
5. **[Low] `noise.glsl:55-76`:** hoist `fract(i*0.1031)` out of the 8 corner hashes (exact, ~2 mul
   per corner). FP16 is not worth it, since the hash must stay fp32 for large coordinates.
6. **[Low] `missile.frag:39-40, 67`:** uniform-only sky ambient (see §2.3). The plume flicker could
   be per vertex, but the missile covers too few pixels to matter.

Already right: `fireball.frag` discards before any noise; depth test without write keeps early-Z;
the `Hash13`/`Hash22` sine-free hashes are fp32-safe over the coordinate ranges used; the `Fbm3`
octave counts are constant, so the loops unroll; and the flash is a single fullscreen triangle.

---

## 9. Suggested order of work

1. **Exact wins first:** §3.1 (sky last), §2.2 (shadow skip), §3.3 (`CelestialBody`), §6.2
   (sprite shrink, hexagon, cull), §5.1 (debris non-instanced), §5.2 (sim skip), §7.1 (histogram
   fold), §3.2 1a (shared hashes), §3.5 board-light gate, §2.4 (`fire.glsl`).
2. **§2.1 shadow gather.** A one-function change with five beneficiaries. Compare a capture before
   and after, looking at shadow edges.
3. **Approximate changes that need a visual sign-off:** §3.2 1b (analytic dune normal), §6.1
   (per-vertex dust lighting), §7.2 (R11G11B10 bloom), §8.1 (fireball noise).
4. **Structural host changes:** §6.3 (indirect particle draws), §5.3 (debris SoA), §4.5 (city
   sort), §2.3 (derived-uniform block).

Profile with RGP before and after each group. These estimates come from reading the code, and the
bandwidth figures in particular depend on DCC and cache behaviour that only a capture can show.

---

## 10. Not performance, but noticed

- `atmosphere.glsl:104-125` (`HighAirLight`, `HighAirKeyAbove`) sits after the `#endif` at line 102,
  outside the include guard. A second include through another header would fail to compile.
- `particle_data.h:37`: `kParticleLife[0] = 6.0f` claims to mirror the shader, but the shader's
  contrail life is `7.0` and its flame life `0.26` (`particle_common.glsl:227`). The comment at
  `particle_common.glsl:60` ("system 4 slots") is stale; `vk_renderer.cpp:122` marks that field
  as spare.
- `fragment_common.glsl:36` describes `vel.w` as the release time, but nothing reads it.
- Odd bloom level sizes (floor-halving gives 135→67→33) put `bloom_down`'s taps off texel
  corners, which may shimmer slightly on the smallest levels. Rounding the chain sizes up fixes it.
- `exposure_histogram.comp`'s "a quarter of the pixels, four times cheaper" (lines 34-35) holds
  for ALU but not for DRAM traffic (see §7.1).
