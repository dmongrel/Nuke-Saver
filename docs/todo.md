# Possible work

Nothing here is required. The saver is complete and installed; these are candidates that would
have to earn their way in. Each says what it would buy and what would have to be true for it to be
worth doing.

Anything marked **measure first** is a change whose benefit depends on what the driver's backend
compiler already does, which is not visible from the GLSL or the SPIR-V. Doing it blind trades
readability for a number nobody has.

## Shader micro-optimisations

Surviving candidates from `shader-eval.md`, after checking each against the source. The rest of
that report is refuted in its own assessment header — do not work from it directly.

### 1. `pow(towardsKey, 4.0)` in `SkyGradient` (atmosphere.glsl)

Replace with two multiplies. An integer exponent is the one case where `pow` has a cheaper exact
equivalent, and this runs over every sky and ground pixel — the sky pass alone is the full frame.
`sky.frag` compiles to three `Pow` instructions today, `ground.frag` to two.

Not the neighbouring `pow(elevation, 0.42)`: a fractional exponent *is* `exp2(log2(x) * k)`, so
writing it out by hand buys nothing.

- **Risk:** none. Same value to float rounding.
- **Size:** two lines.

### 2. Precompute the horizontal key direction (atmosphere.glsl, scene_uniforms)

`SkyGradient` does `normalize(vec3(keyDir.x, 0.0, keyDir.z))` per pixel on a value that is uniform
for the whole frame. Computing it CPU-side into the scene UBO removes a normalize per sky pixel
and leaves one definition of "the key direction, flattened" instead of one per call site.

- **Risk:** low. Adds a field to `SceneUniforms`, so the `static_assert` on its size moves.
- **Worth it if:** the sky pass shows up at all in a GPU capture. It is a full-screen pass, so it
  might.

### 3. Unroll the 3×3 tap loop in `KeyShadow` (shadow.glsl) — **measure first**

The loop survives `glslc -O`: `ground.frag.spv` holds exactly one depth-compare sample
instruction, so the nine taps are a real loop in the SPIR-V, not nine instructions. Whether AMD's
backend unrolls a constant-trip-count loop containing a texture fetch is not visible from here —
it almost certainly does, which is why this is not already done.

- **Risk:** none to output; costs nine lines of near-identical text where three read better.
- **Worth it if:** a shader ISA dump (RGA, or `AMD_DEBUG=vs,ps`) shows the loop intact.

### 4. Unroll the five-way system selection in `ParticleAt` (particle_common.glsl) — **measure first**

Same situation: `particle_count.comp.spv` and `particle.vert.spv` each keep one `OpLoopMerge`, the
five-iteration walk over system capacities. The `break` is probably why `glslc` leaves it alone.

Expected value is low. The loop body is a compare and a uniform read, and what follows it is
dozens of transcendentals; the loop is a rounding error in the function that contains it. Note
this is **not** the five-function refactor `shader-eval.md` proposes — that duplicates the shared
preamble across five bodies to do by hand what the compiler does for free.

## Larger, not obviously a win

### 5. `ParticleAt` is evaluated six times per particle

The particle draw is six vertices an instance with no vertex buffer, so every vertex invocation
re-derives the whole closed-form particle state. For 80,000 particles that is 480,000 full
evaluations a frame, five of every six of them recomputing what the sixth already has.

The obvious fix is to have the scatter pass write `pos`, `size`, `tint` and `alpha` into a buffer
and have the vertex shader read it. That trades ALU for bandwidth — roughly 2.5 MB written and
15 MB read per frame — which may or may not win on a machine whose GPU shares system memory.

It also cuts against spec 8.3's stateless premise, which says the sorted index list is the only
particle memory in the project. So this is a design change rather than an optimisation, and it
should only be attempted with a before-and-after profile, not on the argument above.

## Not shader work

### 6. Reverse-Z for the scene projection

Still unclaimed after the horizon rewrite, and still the largest single improvement available for
anything at distance: the camera runs a forward projection with a 0.5 m near plane and a 60 km far
plane, which resolves about 50 m at 20 km. Reverse-Z takes that to about a millimetre for the cost
of a flipped compare and a changed clear value.

Nothing currently *needs* it — the range that was fighting is now one surface — so it is an
improvement without a bug attached. Requires a per-pipeline compare op, since the shadow pass is
orthographic and stays forward.

### 7. Paperwork left deliberately unfinished

`README.md` was never rewritten, and the full A1–A19 acceptance pass, the 8 MB size check and the
second A11 soak were never run. Recorded as a known gap rather than a pending task.
