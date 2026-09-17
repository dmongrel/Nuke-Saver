# nuke-saver: Implementation Plan

Status: draft, second revision
Companion to [`Nuke-Saver-Spec.md`](Nuke-Saver-Spec.md), which owns behavior. This document
owns sequencing, module layout and risk.

Revisions:

- 2026-09-17: initial plan.
- 2026-09-17: rewritten against spec revision 2. The volumetric raymarch is gone, which
  removes the project's largest performance risk and its hardest-to-tune system. In its
  place the GPU fragment simulation becomes the centre of the work and moves early. New
  milestones for growth, countdown and missile.
- 2026-09-17: updated against spec revision 3. The countdown moves from a screen-space overlay
  to world geometry with four faces and its own light, which moves it out of the post chain and
  into M3. A disperse phase is added to M4. The continuous camera orbit makes the framing solver
  a real piece of work rather than a lookup, and it lands in M3 with the shot library.

---

## 1. Shape of the work

The spec describes a nine-phase sequence. Building it in phase order would mean the fragment
simulation — the system everything after phase 6 depends on, and the one most likely to
force a redesign — lands two thirds of the way through. It moves up instead.

Three rules govern the build:

- **The screen saver never stops working.** Every milestone ends with a `.scr` that installs,
  runs, and exits on input. A milestone that leaves the saver broken is not done.
- **The GDI fallback comes first, not last.** Spec section 12 makes graceful degradation a
  hard requirement, so it is written in M1 and exercised by every milestone after it.
- **Fragments before polish.** M4 proves 125,000 GPU-simulated fragments scatter, settle,
  gather and hold a mushroom shape at frame rate. Everything in M5 and M6 is cosmetic by
  comparison, and none of it is worth writing if M4 does not hold up.

## 2. What revision 2 changed

Worth stating plainly, because it redraws the risk table:

- **The volumetric raymarch is withdrawn.** No density field, no marching, no bilateral
  upsample, no half-res volumetric target. This was R1 in the previous plan and the single
  most expensive pass in the renderer. It is now simply gone.
- **The cloud is opaque geometry.** Fragments are boxes' worth of triangles going through
  the normal forward path. Cheaper, sharper, and it ties the cloud visually to the city.
- **The city halved and simplified.** 500 plain boxes instead of 2,000–6,000 parameterised
  shapes. The shape library module disappears; one unit cube replaces it.
- **Three new authored sequences** — growth, countdown, missile — none of them technically
  hard, all of them needing timing work to feel right.
- **Colour is now specified** rather than left to the implementation, which means it can be
  tested (A18, A19) instead of argued about.

## 3. Milestones

### M0 — Toolchain

Nothing compiles until this lands. Vulkan headers and `glslc` are both absent from the
current MSYS2 install; the runtime loader is already in `System32`.

- `pacman -S mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc`
- Vendor `volk` and VMA into `third_party/`, checked in.
- Makefile: `-std=c++17`, multi-TU object rule, `shaders/%.glsl -> build/%.spv` rule,
  SPIR-V embedding, new libraries.
- Prove the chain end to end with a throwaway shader and a `vkCreateInstance` that is then
  torn down.

Exit: A1 passes and the `.scr` still behaves as a screen saver.

### M1 — Host and fallback

The Win32 side, finished, before any rendering exists.

- Per-monitor window enumeration and creation (spec 9.2), replacing the single
  virtual-desktop window.
- Shared exit path: input on any window kills the process; first `WM_MOUSEMOVE` swallowed;
  mouse dead zone.
- GDI fallback renderer behind the same interface the Vulkan renderer will implement.
- Renderer selection: try Vulkan, fall back on any failure, never show a dialog.
- Settings under `HKCU\Software\nuke-saver`, and the `/c` dialog.

Exit: A2, A3, A4, A8, A9, A12 pass. The saver is complete except that it draws nothing.

### M2 — Vulkan core

- Instance, device selection, queues, VMA allocator.
- Per-window swapchain, recreate on resize and `VK_ERROR_OUT_OF_DATE_KHR`.
- Frames in flight, per-frame command pools, synchronisation.
- HDR offscreen target, depth buffer, ACES tonemap to the swapchain.
- Linear colour discipline (spec 5.1) established here, in the only pass that exists, where
  it is trivial to verify. Retrofitting it later is how projects end up with a gamma bug
  nobody can find.
- Device-loss handling and one re-init attempt.
- Validation layers behind a debug flag, off in release.

Exit: every monitor clears to a colour through the HDR path. A11 soak passes on an empty
scene.

### M3 — World and growth

- Noise library: value/gradient fBm, ridged, curl. Written once, shared by terrain, fragment
  swirl and particles.
- Terrain generation, chunked LOD, sand shading.
- City generator: extent, roads, lot subdivision to exactly 500 lots, box dimensions.
- Instanced box rendering from one unit cube, with shader-generated windows.
- The colour system: per-instance HSV variation (spec 5.2), base palette, and all four times
  of day (spec 5.4).
- Sun, sky, shadow cascades.
- Growth animation (spec 6.4).
- Countdown board: mast and four-faced geometry, procedural 7-segment bars from a digit value,
  its emissive material and its light on the rooftops (spec 7.6). The digits are static at this
  milestone; phase timing is M5's job.
- Camera orbit, the shot library, and the framing solver (spec 11.1) — radius sized from the
  phase 8 cloud extent, then checked against board legibility, missile visibility and city fit
  across the whole arc.

Exit: phases 0–2 are shippable on their own — empty land, a city that grows under an orbiting
camera, a board standing over it. A5, A6, A13, A18, A19, A20, A21, A22, A25 pass.

### M4 — Fragments

The milestone that decides whether the project works. Nothing here is cosmetic.

- GPU fragment framework: subdivide a box into 200–300 triangles on a jittered grid, pack
  per-fragment state, simulate in compute, draw in one instanced call.
- Fragmentation trigger driven by a blast radius expanding from a point — no shell rendering
  yet, just the radius.
- Scatter forces: radial impulse, gravity, drag, tumble (spec 7.4).
- Ground collision and settling.
- Gather: target assignment on the analytic mushroom shape, spring-damper convergence, curl
  swirl, cap roll (spec 7.5).
- Disperse: staggered bottom-up release from the mushroom targets back into the scatter force
  model, cap fan-out, settling (spec 7.7).
- Performance work against the real 125,000 count on the reference machine, at native
  resolution, before anything else is layered on top.

Exit: a building-shaped city shatters, scatters, settles, gathers into a mushroom and then rains
back down, at 60 FPS. A15, A16, A17, A23, A24 pass. Ugly is acceptable at this milestone; slow is
not.

### M5 — The detonation sequence

Now the parts that sit around M4's simulation.

- Phase state machine covering all eleven phases and the cycle reset.
- Countdown timing: exact 5 s, one step per second, all four board faces updating in the same
  frame, dark before phase 3 and after the flash (spec 7.6). The board itself was built in M3.
- Missile: procedural mesh, approach path, exhaust and trail, impact.
- Flash: emissive magnitude, tonemapper white clamp.
- Auto-exposure: histogram compute pass, asymmetric adaptation.
- Bloom chain.
- Fireball: emissive noise-displaced sphere, cooling curve, and its light on the scene.
- Blast shell: screen-space refraction pass and the ground ring (spec 7.2).
- Terrain scorch.

Exit: a full cycle runs end to end and loops cleanly. A14 passes.

### M6 — Particles and finish

- The five particle systems (spec 8.3).
- Embers, settled dust, smoke.
- Quality scaler and the auto-quality controller (spec 11.2).
- Bake preview stills from a real run; wire the preview cross-fade.
- Rewrite `README.md`.
- Full acceptance pass, A1–A19. Size check against 8 MB.

## 4. Module layout

One translation unit per module. Headers stay narrow; nothing outside a module sees its
Vulkan handles.

```
main.cpp              entry, arg parsing, mode dispatch
app/
  windows.cpp         per-monitor window creation, message loop, exit policy
  settings.cpp        registry read/write, defaults
  config_dialog.cpp   /c
  preview.cpp         /p, GDI cross-fade of baked stills
render/
  renderer.h          the interface M1 defines and both backends implement
  gdi_fallback.cpp    black fill
  vk_device.cpp       instance, device, queues, allocator, device loss
  vk_swapchain.cpp    per-window swapchain and its images
  vk_frame.cpp        frames in flight, command recording, submission
  passes/
    depth_prepass.cpp
    forward.cpp       terrain, buildings, missile, fragments
    fireball.cpp
    particles.cpp
    refraction.cpp    blast shell
    bloom.cpp
    exposure.cpp
    tonemap.cpp
  quality.cpp         quality levels and the auto-scaler
sim/
  phases.cpp          the eleven-phase state machine and its clock
  camera.cpp          orbit, shot library, framing solver
  fragments.cpp       fragment state, scatter, settle, gather, mushroom targets, disperse
  missile.cpp         approach path and impact
world/
  noise.cpp           fBm, ridged, curl
  terrain.cpp         heightfield, chunking, LOD
  city.cpp            roads, lots, 500 boxes, growth schedule
  board.cpp           countdown mast, four faces, procedural 7-segment geometry
  palette.cpp         base colours, per-instance HSV variation, times of day
shaders/              GLSL, compiled to SPIR-V by the Makefile
third_party/          volk, VMA
```

`world/shapes.cpp` from revision 1 is gone: one unit cube needs no library.

## 5. Risks

| # | Risk | Why it matters | Response |
|---:|---|---|---|
| R1 | 125,000 fragments will not hold 60 FPS | They are simulated, sorted against nothing, and drawn every frame from phase 6 to phase 9 — over half the cycle | M4 exists to answer this before anything depends on it. Fragments-per-building is the first quality lever (spec 11.2), giving a 5× range without touching building count. |
| R2 | The gather reads as a gimmick, not a mushroom | Fragments converging on an analytic shape can easily look like a mesh being assembled rather than a cloud forming | Spring-damper with per-fragment variation and continuous curl swirl, never a direct lerp to target. Convergence spread over most of a 30–45 s phase. Budget tuning time in M4. |
| R3 | Growth, countdown and missile are timing, not code | Each is straightforward to implement and easy to get subtly wrong — a city that grows too fast, a countdown that drifts, a missile that arrives off screen | Exact-duration requirements and visibility rules are in the spec (6.4, 7.1, 7.6) with tests (A13, A14). Treat them as tuning tasks with acceptance criteria, not as features that are done when they compile. |
| R8 | The framing solver is over-constrained | One orbiting camera, no cuts, must fit a growing city, keep an eight-glyph readout legible, catch a missile, and frame a cloud several times the city's width — across a 30–90° arc | Solve radius from the widest constraint (phase 8) and treat the rest as checks, not as inputs. The board's four faces and its adjustable placement are the slack in the system; the camera path is not. If the solver still cannot converge, the board gets larger or moves, before anything touches the orbit. |
| R4 | Auto-exposure oscillation | A feedback loop driven by a source spanning four orders of magnitude will hunt if damping is wrong | Asymmetric rates specified, not tuned late. Test the phase 5→6 transition specifically. |
| R5 | MinGW plus `-static` plus Vulkan | Static runtime alongside a dynamically loaded loader can surface link-order and TLS problems | M0 proves the whole chain before a line of renderer code exists. |
| R6 | iGPU memory pressure | The reference GPU shares system RAM, and fragment state at 125,000 bodies is the largest single buffer | Track allocations against a budget from M2. Fragment state is fixed-size and known up front, which makes this measurable rather than emergent. |
| R7 | Per-monitor swapchains on mixed displays | Different refresh rates mean present calls blocking at different rates against one shared simulation clock | Simulation advances on the wall clock, never on present. Windows render independently off one shared state. |

Revision 1's R1 — volumetric cost — is retired with the pass that caused it.

## 6. Verification

- Every milestone re-runs A3 and A4, not just its own tests. An exit path that regresses is
  the worst failure this project has, because the user cannot get their desktop back.
- The soak (A11) runs at M2, M4 and M6. GPU leaks do not show up in a two-minute run.
- Determinism (A6) needs a debug-only forced seed. Add it in M3 and keep it.
- The sequence tests (A13–A17) need a debug-only phase scrubber — jump to a phase, freeze,
  step frames. Add it in M4; frame-stepping the blast front by eye is otherwise impossible.
- Per-pass frame timing captured from M2 onward behind the debug flag, so R1 and R6 are
  measured continuously rather than investigated after they bite.

## 7. What is not planned yet

Waiting on further specs: audio, weather and wind beyond a constant drift, any second
detonation type. None changes the module layout above.
