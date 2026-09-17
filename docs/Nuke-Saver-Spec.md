# nuke-saver: Build Specification

Status: draft, fifth revision — more specs to follow
Target: Windows 10 1903+ / Windows 11, x86-64
Deliverable: `nuke-saver.scr`, a native Win32 screen saver rendering with Vulkan

This document is the source of truth for what nuke-saver does and looks like. The staged
engineering work — milestones, module layout, dependency setup — lives in
[`Nuke-Saver-Implementation-Plan.md`](Nuke-Saver-Implementation-Plan.md); where the two
overlap, this document wins on behavior and the plan wins on sequencing.

Keywords: **MUST** is a hard requirement. **SHOULD** is expected unless there is a stated
reason not to. **MAY** is optional.

Revisions:

- 2026-09-17: initial spec. Owner direction: a screen saver showing a simulated nuclear
  detonation over a desert city, C++, Vulkan, real-time, not physically accurate.
- 2026-09-17: owner direction, second pass. The scenario is rewritten end to end: the land
  starts empty, a 500-building city grows out of it, a 7-segment countdown runs, a missile
  flies in, and the city shatters into triangles that scatter and then reassemble into the
  mushroom cloud. Colour authoring is now specified (section 5, HDR-ready linear values,
  ±10% per-instance variation). Time of day expands to four settings. The raymarched
  volumetric cloud of revision 1 is **withdrawn** — the cloud is made of the city's own
  triangles (section 7.5).
- 2026-09-17: owner direction, third pass. Q2 and Q4 resolved. The countdown is now a
  **world object** — a physical display standing over the city, lighting it, and destroyed
  by the blast along with everything else (section 7.6). The mushroom cloud **disperses**
  rather than holding: a new phase releases the fragments to rain back over the ruins
  (section 7.7). The cycle is renumbered to eleven phases.
- 2026-09-17: owner direction, third pass addendum. The camera **orbits the city continuously**
  for the whole cycle and is never static (section 11.1). This forced a change to the countdown
  board: a single fixed face would turn away from an orbiting camera mid-countdown, so the board
  now carries four faces (section 7.6).
- 2026-09-17: owner direction, fourth pass. Q1, Q5 and Q6 resolved. **Twilight** is the default
  time of day (section 10). The horizon is closed by a **far-field impostor** of random
  triangular mountains, and the sky carries **stars and one celestial body — sun or moon — that
  is also the scene's key light** (section 6.3). The countdown board shows the time and nothing
  else, and is art-directed as **diegetic title typography**: numerals that belong to the city
  the way film credits are built into a shot (section 7.6). Weather is settled as **none** —
  no cloud layer, no precipitation (section 15).
- 2026-09-17: owner direction, fifth pass. Q3 resolved: preview paints **black until M6**, when
  the stills can be baked from a working renderer, and that is an accepted interim state rather
  than an open defect (section 9.3). No open questions remain.

---

## 1. Purpose

nuke-saver is a screen saver. An empty desert basin builds itself a city, a countdown board
over the rooftops runs out, a missile arrives, and the city is blown apart into a storm of
triangles that gathers itself back into a mushroom cloud — then lets go, and rains what is
left over the ruins. Then the land is empty again and it starts over.

The simulation is **theatrical, not physical**. Nothing here models yield, neutron transport,
overpressure or fallout. Every effect is whatever cheap approximation sells the look at frame
rate.

The project is not a targeting aid, a weapons-effects estimator or a training tool, and MUST
NOT present its output as predictive of anything. It is a light show.

## 2. Source of truth

| Source | Describes | Status |
|---|---|---|
| Owner direction, 2026-09-17 (fifth pass) | Preview black until M6 | **Authoritative** |
| Owner direction, 2026-09-17 (fourth pass) | Twilight default, mountain horizon impostor, skybox with stars and sun/moon key light, diegetic countdown typography, no weather | Authoritative where the fifth pass is silent |
| Owner direction, 2026-09-17 (third pass) | World-space countdown board, dispersing mushroom cloud, orbiting camera | Authoritative where the fourth pass is silent |
| Owner direction, 2026-09-17 (second pass) | Growth, countdown, missile, triangle destruction and triangle mushroom, colour rules, four times of day | Authoritative where the third pass is silent |
| Owner direction, 2026-09-17 (first pass) | Screen saver, desert city, nuclear detonation, C++, Vulkan, real-time, non-physical | Authoritative where later passes are silent |
| `main.cpp` (commit `911badf`) | Win32 screensaver skeleton: `wWinMain`, `/s` `/p` `/c`, black window | Valid scaffolding. Section 9 extends it. |
| `Makefile`, `nuke-saver.rc`, `nuke-saver.manifest` (commit `911badf`) | MinGW/g++/windres build of a `.scr` | Valid, but section 13 changes the standard and adds dependencies. |
| `docs/ghost-saver-spec.md` (sibling project) | Screen-saver host behavior conventions | **Authoritative** for `/s` `/p` `/c` semantics and exit conditions, which nuke-saver copies. |
| Revision 1, section 6.3 (raymarched volumetric cloud) | A marched analytic density field | **Withdrawn.** Do not implement. Section 7.5 replaces it. |
| Revision 2, section 7.6 (screen-space countdown overlay) | A 2D HUD readout | **Withdrawn.** Do not implement. Section 7.6 replaces it. |
| `README.md` | "Scaffolding only" | Stale once M1 lands. Rewrite at that point. |

## 3. Scope of this revision

Specified here: the run cycle and its eleven phases, the world and its sky, colour authoring, the
rendering approach, the screensaver modes, settings, the performance budget, failure behavior
and the build.

Deferred, listed so nobody designs around their absence: audio; HDR display output; any
second detonation type.

Weather is no longer deferred — it is **resolved as none** (section 15). The only atmospheric
effect in the project is the distance haze that the horizon range and terrain already use. Wind
is a single constant drift direction per cycle, used by the cloud and the smoke, and nothing
more.

## 4. Run cycle

### 4.1 Phase table

One cycle runs 80–115 seconds, then repeats with a new seed. Durations marked *random* are
drawn per cycle, uniformly, from the stated range.

| # | Phase | Duration | What the viewer sees |
|---:|---|---|---|
| 0 | Empty land | 5–8 s *random* | Bare desert basin. No city, no marks on the ground. The camera is already moving. |
| 1 | Growth | 5–10 s *random* | 500 buildings rise out of the ground, staggered, until the city stands complete. The countdown board rises with them, last. See 6.5. |
| 2 | Settle | 2–3 s *random* | The finished city, still, the board dark. Nothing happens. This beat exists so the countdown lands on a stable frame. |
| 3 | Countdown | exactly 5 s | The board lights: `00:00:05` counting to `00:00:00`, throwing its own light across the rooftops. See 7.6. |
| 4 | Missile | 4–6 s *random* | A missile comes in over the mountains and runs down to the city centre, trailing a contrail. The board holds at `00:00:00`. |
| 5 | Flash | 0.3 s | On impact the screen goes fully white over ~120 ms and holds. |
| 6 | Blast | 4–6 s *random*, overlaps 7 | A refractive spherical shell expands fast from the impact point. Every building it touches — and the board — bursts into triangles. |
| 7 | Scatter | 5 s exactly | Triangles fly outward from the centre, tumbling, gravity dragging them down until they are skidding and settling on the desert floor. |
| 8 | Gather | 25–35 s *random* | The pull reverses. Every triangle is drawn back toward the city centre and lifted, converging into a mushroom cloud built entirely out of the city that was there. The cap turns about its own tube, rising on the inside and curling down at the rim. |
| 9 | Disperse | 15–25 s *random* | The cloud lets go. Fragments are released from the shape and fall, thinning the cloud from the bottom up, raining back down over the ruins and settling. The embers go out, turn grey and come down with it. |
| 10 | Fade | 8 s | To black over the settled debris field. State torn down, new seed drawn, empty land again. |

Phases 3 and 4 MUST NOT overlap: the countdown reaches zero, *then* the missile appears.
Phase 6 MUST overlap phase 7 — the blast front is still expanding while the nearest triangles
are already flying.

### 4.2 Timing

- The clock MUST come from `QueryPerformanceCounter`, never from frame counts or accumulated
  `Sleep`.
- Every animated quantity MUST be a function of elapsed seconds, so frame rate affects
  smoothness and nothing else.
- Frame delta MUST be clamped to 100 ms before it reaches the simulation, so a stall does not
  fling triangles across the map.
- Phase 3 MUST run exactly 5.000 s of wall clock. A countdown that drifts against its own
  displayed digits is a defect.
- A cycle MUST be reproducible from its seed: same city, same missile approach, same gross
  cloud shape. Per-frame jitter from delta timing is exempt.

## 5. Colour

This section governs every colour in the project. It is short, and it is not negotiable
per-effect.

### 5.1 HDR-ready authoring

- All colour MUST be authored and computed in **linear scene-referred** space. sRGB encoding
  happens once, in the tonemap pass, and nowhere else.
- Surface albedo is in 0–1. Light and emission are **not** clamped to 1: the flash, the
  fireball and the countdown segments MUST carry values well above 1.0 so that bloom and
  auto-exposure have something real to work with. An emissive authored at 1.0 will look flat
  and is a defect.
- Reference emissive magnitudes, relative to a mid-grey lit by the noon sun at 1.0:

| Source | Linear emissive |
|---|---:|
| Building windows, night | 2 – 5 |
| Countdown board, lit segments | 30 – 60 |
| Missile exhaust | 20 – 40 |
| Detonation flash, peak | 8,000 – 15,000 |
| Fireball, phase 6 start | 2,000 – 4,000 |
| Fireball, phase 6 end | 20 – 60 |
| Embers, phases 8–9 | 5 – 15 |

The countdown board is far brighter than a window because it is a light source in its own
right (7.6), not a lit surface.

- The HDR path is internal. Output to the swapchain is SDR; HDR *display* output is out of
  scope (section 15).

### 5.2 Per-instance variation

A screen saver that draws 500 boxes in one grey looks like 500 boxes in one grey. Every colour
in the scene MUST vary.

- Each instance MUST offset its base colour by a stable per-instance random of **±10%** unless
  a different figure is given below.
- The offset MUST be applied in HSV and then converted, not by scaling RGB — scaling RGB
  changes brightness and leaves hue flat, which is the thing this rule exists to prevent. Hue
  ±10% of the stated hue range, saturation ±10%, value ±10%, each drawn independently.
- The random MUST be derived from the instance's ID and the cycle seed, so it is stable for
  the whole cycle and reproducible across runs.
- Fragments inherit their parent building's varied colour (section 7.3), so the cloud in phase
  8 carries the city's colour spread rather than averaging to mud.

### 5.3 Base palette

Hues are given as ranges; the ±10% of 5.2 applies on top.

| Surface | Base | Variation |
|---|---|---|
| Desert floor | Brown, hue 25–35°, sat 0.35–0.50, val 0.35–0.55 | ±10% |
| Rock and mesa | Brown-grey, hue 20–30°, sat 0.15–0.30, val 0.30–0.45 | ±10% |
| Building body | Concrete grey, hue 20–40°, sat 0.02–0.12, val 0.35–0.70 | ±10% |
| Building windows | Blue-grey, hue 195–215°, sat 0.10–0.30, val 0.25–0.60 | ±15% |
| Countdown board frame | Dark grey, sat 0.00–0.08, val 0.10–0.20 | ±10% |
| Countdown segments, lit | Amber-red, hue 5–20°, sat 0.85–1.00 | ±5% |
| Countdown segments, unlit | Board frame colour, darkened 40% | inherited |
| Missile body | Neutral grey-white, sat 0.00–0.05, val 0.70–0.85 | ±10% |
| Fragment edges | Parent colour, darkened 15–30% | inherited |
| Settled dust | Desert floor colour, desaturated 40% | ±10% |

Lit-segment variation is tightened to ±5% because eight glyphs of the same display visibly
mismatching reads as a bug, not as variety.

### 5.4 Time of day

Four settings, selectable or random per cycle (section 10). **Twilight is the default.** It
gives the fireball its best contrast, puts the city into silhouette, and is the only setting
where the sky itself carries colour worth looking at for 25 seconds of empty land.

Each MUST change key-light angle, key-light colour, sky gradient, star visibility and base
exposure together — a time of day that only tints the sky is not done. The celestial body drawn
on the skybox and the direction of the key light come from one value (6.3).

| | Sun elevation | Sun colour | Sky | Notes |
|---|---:|---|---|---|
| Morning | 12–20° | Warm, ~4,500 K | Pale blue overhead, warm haze at the horizon | Long shadows running one way |
| Noon | 70–85° | Neutral, ~6,500 K | Strong blue, tight horizon haze | Short hard shadows, highest contrast on building faces |
| Twilight | 2–6°, below horizon at the low end | Deep orange-red, ~2,200 K | Orange to violet gradient, strong horizon band | Best contrast for the fireball. Buildings read as silhouettes. |
| Night | Sun below horizon | None; moon fill only, ~7,500 K, dim | Near-black with a faint gradient, stars MAY be drawn | City is lit by its own windows and the board. The flash is at its most violent here. |

Window emission MUST scale inversely with ambient light: barely visible at noon, a main source
of city light at night.

## 6. The world

### 6.1 Generation, not assets

The world MUST be generated procedurally at cycle start from a 64-bit seed. The `.scr` MUST
NOT load external model, texture or level files at runtime, and MUST remain a single
self-contained executable. This is a screen saver: it gets copied into `System32` by itself,
and anything it cannot carry inside its own resource section does not exist.

### 6.2 Terrain

- A desert basin extending to a visible horizon. Nominal extent 8 km × 8 km, with distant
  terrain carried by a skirt mesh or a horizon impostor.
- Height from summed fBm noise, 5–7 octaves, ridged at the low frequencies for a few mesas and
  a shallow rim of hills, flattened where the city will stand.
- Chunked LOD grid. The horizon MUST NOT visibly pop.
- Colour per section 5.3. Dunes are normal-map detail, not geometry.
- The floor MUST darken and scorch inside the blast radius during phase 6, and MUST stay
  scorched through phase 10.

### 6.3 Horizon and sky

The basin is 8 km across and the camera orbits it. Something has to close the horizon, and it
has to be cheap enough that it costs nothing to keep on screen for the whole cycle.

**Far-field mountains**

- A ring of low-polygon triangular peaks surrounds the basin beyond the playable terrain,
  standing far enough out to read as distant range rather than as basin rim.
- Each peak is a simple triangular form — a few faces, no displacement, no LOD. They are
  silhouette, not geography.
- Height, width, spacing and radial distance are randomised per cycle from the seed, with
  overlapping rows at different distances so the range has depth rather than reading as a
  fence.
- The ring MUST completely close the horizon from every point on the camera orbit, at every
  shot height in the library. No gap may show sky meeting flat ground.
- Peaks MUST be tall enough that the tallest building never breaks the skyline behind them
  from a low shot, which is what would give the impostor away.
- They are lit by the same sun or moon as everything else, and MUST fade into the horizon haze
  with distance so the range reads as atmospheric rather than as a painted backdrop.
- They are **static scenery**: not fragmented by the blast, not lit dynamically by the fireball
  beyond a flat distance-attenuated term, and never simulated.

**Sky**

- A skybox, drawn behind everything, carrying a gradient from the time-of-day table in 5.4.
- **Stars** at twilight and night, as points on the skybox, with brightness scaled by ambient
  so they emerge as the sky darkens. They MUST be fixed to the sky, not to the camera, so the
  orbit moves past them.
- **One celestial body** is drawn, and it is the scene's key light:

| Time of day | Body | Role |
|---|---|---|
| Morning | Sun, low | Key light, warm |
| Noon | Sun, high | Key light, neutral |
| Twilight | Sun, at or just below the horizon | Key light, deep orange-red |
| Night | Moon | Key light, dim and cool |

- The body's position on the skybox and the direction of the scene's key light MUST be derived
  from the same value. A sun drawn in one place while shadows fall from another is a defect,
  and the long shadows of morning and twilight make it obvious.
- The body MUST be emissive per 5.1 and MUST bloom.
- At night the moon MUST be bright enough to read as the light source, and MAY be drawn with
  simple surface variation. Phase is fixed; it does not need to be modelled.
- No weather. No cloud layer, no precipitation, no fog beyond the distance haze that the
  mountains and terrain already use (section 15).

### 6.4 The city

**500 buildings, hard limit for this revision.** Each is a rectangular or square box — nothing
else. No setbacks, no crowns, no roof furniture, no masts. The silhouette comes from the
distribution of box dimensions, not from detail.

Generated in this order:

1. **Extent.** A roughly circular footprint, 1.2–2 km across, at the basin centre.
2. **Roads.** A primary grid rotated by a random angle, with 1–3 arterials cutting across.
   Blocks are the cells left over.
3. **Lots.** Blocks subdivided by recursive splitting until 500 lots exist. Excess lots are
   discarded from the outside in, so the city stays dense at the centre.
4. **Boxes.** One box per lot. Footprint square or rectangular, aspect between 1:1 and 1:2.5.
   Height drawn from a distribution whose mean falls off with distance from the centre, so a
   downtown emerges without anybody authoring one.

Requirements:

- All 500 MUST be drawn from **one** unit-cube mesh, instanced. Per-instance data is transform,
  colour, growth time and fragment range — nothing more.
- Windows MUST be shader-generated from surface UV, not textured, with a stable per-window
  random on/off state (section 5.4).
- Two consecutive cycles sharing a skyline is a defect.

### 6.5 Growth (phase 1)

- Each building is assigned a start time spread across the phase duration, weighted so the
  centre starts first and the outskirts follow. The city grows outward.
- A building rises from zero height to full height over 0.4–0.9 s, scaling on Y only, with an
  ease-out so it decelerates into place. It MUST NOT overshoot or bounce.
- A building MUST NOT be visible before its start time.
- A small dust puff at the base on emergence is permitted and encouraged.
- The countdown board (7.6) rises **last**, after the final building, so the eye is left on it
  going into phase 2.
- Everything MUST be standing before phase 2 ends.

## 7. The detonation

### 7.1 Missile (phase 4)

- One procedurally generated mesh: a cylinder body, a cone nose, four fins. No asset.
- Enters at a random compass bearing, reaching the city centre exactly at the end of the phase.

**Where it comes from**

Two requirements, both about the entry point's **elevation as seen from the camera**, and neither
about a distance — the same entry point is over the mountains from a street-level shot and below
them from a high oblique one:

- It MUST be **above the mountain silhouette (6.3)** at its own bearing, by a clear margin, so the
  missile comes in over the range rather than appearing in front of it.
- It MUST be **inside the frame**, below the top edge and in front of the camera rather than off
  to one side of it. A missile the viewer never sees arrive fails this phase more completely than
  one that enters level with the peaks: an entry aimed over mountains the shot does not contain is
  a correct number and an empty screen.

Both are therefore settled **after** the camera and the range exist, and what is adjusted to meet
them is the **descent angle** — the approach is turned, not moved. The bearing and the run length
are drawn per cycle and left alone, so the arrival that the framing solver was given does not move
under it, and the entry point's distance from the camera does not depend on the angle being solved
for, which makes the solve one line of trigonometry rather than a search.

- The descent angle MUST be held to a band a missile could plausibly hold — roughly 10° to 45°.
  Below the floor the approach is the shallow slide that put the missile among the rooftops for
  the last second of its run; above the ceiling it is a drop, and the contrail behind it is a
  vertical stroke that says nothing about where the missile came from.
- The **bearing** is the one thing that may be given up, and only as much of it as the shot
  demands: a random bearing is sometimes the one the camera has its back to, where no descent
  angle puts the entry both over the range and on screen. The search MUST walk outward from the
  drawn bearing and stop at the first that works, and MUST prefer a bearing that is in shot but
  level with the peaks over one that clears them off the edge of the frame.
- Where the shot carries no sky above the range at all — the steep passes look down into the
  basin — the frame wins and the missile enters from above the top edge. This is a property of
  those shots, not a defect.

**How it flies**

- Speed MUST read as deliberate rather than as falling, and MUST decrease monotonically along the
  run without ever reaching zero. A constant pace over an approach long enough to clear the
  mountains spends the final stretch — the part the phase exists to show — in under a second.
- Camera framing MUST guarantee the missile is on screen for the arrival: from the moment it is
  within about 0.85 city radii of the impact point, which on a typical run is its last two
  seconds. Stated as a distance rather than as a time because the run length is not known when
  the framing is solved. A missile that arrives unseen wastes the phase.

**How it reads**

- It MUST be bright, and it MUST NOT be a glowing dot. The airframe carries the visibility, lit
  rather than emissive: it is high enough that the key light still reaches it after the ground has
  lost it, so at the default twilight it is a pale object against a darkening sky. A small emitter
  at that range is a bloom bead with nothing legible inside it, so the exhaust plume is short,
  sits at the bottom of 5.1's band, and dims further with distance.
- The high-altitude term applies to the **key light only**. It says that the sun is below the
  ground's horizon and above this object's, which is a statement about one light and no others;
  applied to the sky ambient as well it says the sky is brighter up there, which is false, and at
  twilight it is enough to lift the trail above the sunset behind it.
- It MUST leave a thin smoke contrail, persisting long enough to draw the whole approach back to
  where it came over the mountains. Thin is the requirement, not incidental: a trail as wide as
  the missile is long is a smear.
- The contrail MUST read as close in tone to the sky it hangs in, a little brighter and less
  saturated, never as a solid stroke ruled across it. A contrail is dozens of overlapping sprites
  deep, so **its colour is the colour of one sprite whatever its alpha is** — lowering the alpha
  softens its edges and does not dim it — and the composite sits far enough up the tonemap's
  shoulder that halving anything is invisible. Its density MUST therefore be set by the emission
  rate, its tone by the particle's own colour, and both MUST be checked by measuring the rendered
  pixels against the sky beside them rather than by eye.
- At impact the missile is destroyed. It MUST NOT be visible in phase 5 or later.

### 7.2 Flash and blast (phases 5–6)

- **Flash.** Screen goes fully white over ~120 ms and holds for the remainder of phase 5.
  Driven by emissive magnitude and auto-exposure (8.2), with a tonemapper clamp toward white to
  guarantee full saturation.
- **Blast wave.** An expanding spherical shell centred on the impact point, rendered with a
  **refractive shader**: the shell offsets the screen-space UV of everything behind it,
  strongest at the shell surface, falling off sharply on both sides. It MUST be visible as a
  distinct moving boundary, not a general blur.
- The shell expands fast at first and decelerates. It MUST cross the whole city inside phase 6.
- A ground ring of stripped dust MUST expand with it, slightly ahead of the shell at ground
  level.
- A fireball — emissive sphere with noise-displaced surface, cooling white → yellow → orange →
  deep red per 5.1 — sits at the centre and is the dominant light source for phases 6 and 7.

### 7.3 Fragmentation

The centre of the whole project. Buildings do not collapse; they cease to be buildings.

- When the blast shell reaches a building, that building is **replaced** by its fragments in the
  same frame. There is no partial state and no damaged-building model.
- Each building shatters into **200–300 triangles**, giving roughly 125,000 fragments across the
  city. The count per building MUST scale with the quality level (11.2).
- The countdown board fragments on the same rule, into 400–600 triangles because it is larger
  and closer to the centre. Its fragments keep the board's colours, so the cloud carries visible
  streaks of amber and dark grey through it.
- Fragments are generated by subdividing faces on a jittered grid, so they are small, irregular,
  and visibly triangular rather than uniform.
- Each fragment is an independent body: position, orientation, linear velocity, angular velocity,
  and the parent's varied colour (5.2).
- Fragments MUST be simulated on the GPU in compute and drawn in one instanced call. The CPU MUST
  NOT touch per-fragment data.
- Fragments are double-sided and unlit on their back face, so a tumbling cloud flickers with
  contrast instead of going flat.

### 7.4 Scatter (phase 7)

Exactly 5 seconds. The forces, in order of magnitude:

1. **Radial impulse**, applied once at fragmentation: outward from the explosion centre,
   magnitude falling off with distance, ±20% random per fragment so the front is ragged.
2. **Gravity**, constant, downward.
3. **Drag**, light, so fragments do not accelerate forever.
4. **Tumble**: angular velocity set at fragmentation from the impulse, then held.

- Fragments MUST collide with the ground plane and settle: on contact, kill the vertical
  component, scrub most of the horizontal, damp the spin. They skid and stop.
- By the end of the phase the majority MUST be on or near the ground, spread well outside the
  city footprint. The frame at t+5 s should read as a flat debris field with the fireball above
  it.

### 7.5 Gather and the mushroom (phase 8)

The pull reverses. This is what the scatter exists to set up.

- Every fragment, grounded or airborne, is released from rest and drawn toward the city centre.
  Grounded fragments MUST visibly lift off rather than teleport.
- Each fragment is assigned a **target position on an analytic mushroom shape** — a torus cap
  over a tapering stem, with cap radius and stem height growing over the phase. Assignment MUST
  be stable for the cycle: fragments that started near the centre go to the stem, fragments from
  the outskirts go to the cap rim.
- Fragments move toward their target under a spring with damping, plus a slow curl-noise swirl so
  the surface churns and the cloud never looks like a solid model. They MUST NOT snap into place;
  convergence should take most of the phase.
- Once converged the cloud MUST keep turning, and the turn that matters is **around the cap's own
  tube**, not around the stem. The cap is a vortex ring: material climbs the inner face of the
  doughnut, turns outward across the top, and rolls down the outer edge. That circulation is the
  shape of the thing rather than a decoration on it — it is why a mushroom cloud curls under at
  its rim instead of spreading like a disc — so a cap that only spun about the stem, or only
  wobbled, would be wrong even if it never held still. A revolution SHOULD take around twenty
  seconds, so the roll reads across a gather phase without the cloud looking spun.
- The roll MUST be **driven as well as aimed at**. Turning the target shape alone is not enough: a
  spring follows a moving target at a lag and at a fraction of its swing, which comes out as a
  doughnut being tracked rather than one that turns. Fragments MUST also carry the velocity their
  place on the tube has.
- That drive MUST be confined to the cap, and MUST fade out with distance from the tube. A
  fragment still crossing the desert on its way in sits a tube's width outside the ring; a roll
  term applied to it is not a rim curling under but a steady downdraft on everything in flight.
- The whole cloud drifts slowly in one constant wind direction.
- Fragments retain their building colours throughout, so the cloud reads as the city it was made
  of. Fragments MAY be tinted toward the fireball's colour near the stem base, falling off with
  height.
- Embers (5.1) rise through the stem, and go out with the cloud (7.7).

This replaces revision 1's raymarched volumetric cloud entirely. There is no density field and
no march.

### 7.6 Countdown board (phases 1–6)

A physical object in the world, not an overlay. It is built with the city, it lights the city,
and the blast takes it apart with everything else.

**Intent**

The reference is diegetic film typography — the title sequences where the words are built into
the shot, standing among the buildings at the scale of the buildings, lit by the scene's own
light, with the city passing in front of them as the camera moves. The countdown should read
that way: not signage the city happens to contain, but numerals that belong to the shot.

This is an art-direction constraint with teeth. It means the digits are **monumental** rather
than sign-sized, they sit **in** the skyline rather than above it, and the camera's motion
parallaxes them against the buildings. It also means partial occlusion is not a bug.

**Form**

- A freestanding structure standing **on the desert at the near edge of the city**, its one face
  carrying eight 7-segment glyphs reading `HH:MM:SS` — six digits and two colons, and nothing
  else. No name, no marking, no branding.
- The glyph band MUST start at ground level. The numerals stand on the ground with the city
  behind and beside them; they are not carried above the roofs on a mast.
- Position MUST be chosen against the camera: on the arc of the footprint **nearest** the camera
  during the countdown, and off to the camera's **right**. Placed on the far edge the whole city
  is in the way and most of the glyphs are lost; placed on the city axis nothing can ever occlude
  it. The near arc, a third of a turn round to the right, is what makes the outermost blocks —
  and only those — pass in front of the numerals as the orbit carries the camera along the front
  of the city.
- Scale is monumental: glyph height MUST be a substantial fraction of the tallest buildings, so
  the digits read as part of the skyline rather than as a board mounted above it. Scale with the
  city's extent rather than fixing dimensions, so a small city does not get numerals twice its
  width. The face MUST NOT be so wide that it wraps past the footprint at either end.
- Foreground buildings MAY partially occlude the glyphs, and as the camera orbits they SHOULD —
  that parallax is the effect. The requirement is only that all eight glyphs stay *identifiable*
  throughout phases 3 and 4, not that they stay unobstructed.
- Segments are **geometry**, not a texture: extruded bars set into a recessed dark face, deep
  enough that they self-shadow and catch the key light at a grazing angle.
- The supporting structure MUST be minimal — an open lattice, or nothing visible at all. A
  heavy frame turns monumental typography back into a billboard.

**Orientation**

The board has **one face**. Four faces on a square mast make the numerals legible from every
bearing and therefore revealed by nothing: the camera sweeps only 30–90° over a cycle (11.1), and
a board that reads the same from all of those takes no part in that motion. One face turned
towards where the camera will be squares up during the countdown and comes round into view as the
orbit carries the camera toward it.

- Yaw MUST be fixed at cycle start and held. The board MUST NOT billboard, and MUST NOT rotate to
  follow the camera. A sign that turns with the viewer destroys the illusion that it is a physical
  object, which is the entire reason it is one.
- Yaw MUST be set so the face points back towards the camera's countdown position, within about
  15°, so the numerals are square to the viewer somewhere in phase 3 rather than never.
- The whole face MUST update in the same frame. A glyph showing a stale digit is a defect.

**Behavior**

- Rises last during phase 1 (6.5), **already lit, showing `00:00:05`**. A board that stood dark
  from the moment it appeared until phase 3 read as scenery rather than as the thing about to
  happen, and made the display look as though it switched on rather than started counting.
- Holds `00:00:05`, lit, through phase 2.
- Steps to `00:00:00` once per second across phase 3.
- Holds `00:00:00`, still lit, through phase 4.
- Goes dark at the flash and fragments when the shell reaches it in phase 6.
- Unlit segments MUST be visible as dark recessed bars, the way real 7-segment hardware looks. A
  display where unlit segments vanish is a defect.

**Light**

- Lit segments are emissive at 30–60 linear (5.1) and MUST bloom.
- The board MUST be a real light source: an area or point light at its face, amber, illuminating
  the rooftops and upper storeys beneath it. At night this MUST be plainly visible as the
  dominant light on the city. At noon it MAY be subtle, but it MUST NOT be absent.
- Segment geometry MUST be generated procedurally from the digit value. No font, no texture, no
  glyph atlas — consistent with 6.1.

### 7.7 Disperse (phase 9)

The cloud does not hold. Having assembled itself, it comes apart.

- Fragments are released from their mushroom targets progressively, **bottom up**: the stem
  empties first, then the cap from its underside outward, so the silhouette thins and sags rather
  than dissolving uniformly.
- Release MUST be staggered across the phase, not applied to all fragments at once. A cloud that
  drops in a single frame is a defect.
- On release a fragment returns to the phase 7 force model — gravity, drag, tumble — with a small
  outward nudge inherited from the cap's rotation, so cap fragments fan out as they fall.
- Fragments falling through the fireball's remaining light MUST pick it up, so the fall is a rain
  of lit debris rather than silhouettes.
- Fragments settle on the ground on contact, under the same rule as 7.4.
- By the end of the phase the cloud MUST be gone and the majority of fragments MUST be at rest,
  leaving a debris field over the scorched footprint for the phase 10 fade.
- **Embers go out and come down with the cloud.** They stop burning when it lets go, turn grey,
  and fall through the debris rather than climbing past it. An ember still rising through a cloud
  that is collapsing reads as a second event happening at the same time, not as part of this one.
- Nothing new is set alight after the cloud lets go. The emitter does not stop, though: what it
  makes from that point on is ash, shed — falling — from the body of the cloud. An ember only
  lives about five seconds, so the cinders that were already climbing are grey and gone inside the
  first few, and without the shedding the effect would be over before the phase was a quarter
  through. Ash MUST come from the cloud and not from the ground: a grey point appearing at the
  stem's foot has nowhere to fall from.
- The turn MUST be staggered by how high the ember was headed, on the same bottom-up principle as
  the fragments. It MUST also be spent in a couple of seconds rather than across the phase, for
  the same reason — a turn an ember does not reach before it dies is a turn nobody sees.
- Ash MUST be legible against the cloud, which by this point is the darkest thing in the frame.
  That means pale rather than dark, and a visibly larger sprite than the cinder it came from: a
  point of light that goes out and stays a point simply disappears.

## 8. Rendering

### 8.1 Pipeline

Forward renderer, HDR throughout, targeting Vulkan 1.2 core with no hard extension requirement
beyond a swapchain.

```
depth prepass
  -> opaque forward (terrain, buildings, board, missile, fragments) -> HDR R16G16B16A16_SFLOAT
  -> fireball (emissive sphere, depth-tested)
  -> transparent particles (dust, smoke, embers; sorted)
  -> blast refraction (screen-space, phases 6-7 only)
  -> bloom (downsample chain, 5-6 mips, tent-filter upsample)
  -> auto-exposure (compute histogram, adapt)
  -> tonemap (ACES) + dither -> swapchain B8G8R8A8_UNORM
```

The volumetric pass of revision 1 is gone. Fragments are opaque geometry, which is both cheaper
and sharper than marching a density field. The countdown overlay pass of revision 2 is also gone:
the board is world geometry and goes through the forward pass with everything else.

### 8.2 Light and exposure

Auto-exposure is not a nicety. It is the effect that sells the detonation.

- Phases 0–2 are lit by sun and sky per the time of day, plus window emission.
- Phases 3–4 add the countdown board as a local source (7.6).
- From phase 5 the fireball MUST be the dominant light, its intensity following the curve in 5.1,
  casting the shadows that rake across the debris field.
- Exposure MUST adapt over time, and MUST adapt *down* faster than *up* — roughly 0.2 s to darken,
  2–4 s to brighten. The white-out and the slow recovery come from this, not from a scripted fade.

### 8.3 Particles

Separate from fragments (7.3), which are their own system.

| System | Peak live | Phases |
|---|---:|---|
| Growth puffs | 10,000 | 1 |
| Missile exhaust and trail | 15,000 | 4 |
| Ground collar dust | 30,000 | 6–7 |
| Settled dust | 20,000 | 7–10 |
| Embers | 8,000 | 8–9 |

GPU-simulated, instanced camera-facing quads, sorted back to front where alpha-blended. Peak
counts scale with quality.

The sort MAY be a bucketed one — a counting sort into depth slabs rather than a full comparison
sort — provided the slabs are fine enough that the residual mis-ordering is smaller than one
particle's own extent. A full sort of eighty thousand keys is a bitonic network of roughly 150
dispatches a frame, which 11.2 rules out. Additive systems need no sort at all: addition commutes,
so their order carries no information.

## 9. Screensaver behavior

### 9.1 Modes

Argument parsing, prefixes and case-insensitivity MUST match ghost-saver exactly. The skeleton in
`main.cpp` already does this and MUST NOT be reworked.

| Argument | Behavior |
|---|---|
| (none) / `/s` | Full-screen simulation on every monitor |
| `/p <hwnd>` / `/p:<hwnd>` | Preview in the host's child window — **no Vulkan**, see 9.3 |
| `/c` / `/c:<hwnd>` | Settings dialog, see section 10 |
| anything else | Exit 0 immediately |

### 9.2 Full-screen and multiple monitors

- One borderless top-most popup window per monitor, sized to that monitor's bounds. The single
  virtual-desktop window ghost-saver uses is **not** adequate: it forces one swapchain across
  displays that may differ in DPI, refresh rate and HDR state.
- All windows MUST share one `VkInstance`, one `VkPhysicalDevice` and one `VkDevice`, and one copy
  of every mesh, pipeline and simulation buffer. Only the swapchain and its per-image resources
  are per window.
- Every monitor MUST show the same simulation at the same instant, from its own camera, with its
  own aspect.
- The cursor MUST be hidden.
- Input MUST tear the whole process down, not just the window that received it. Any `WM_KEYDOWN`,
  `WM_SYSKEYDOWN`, mouse button, or mouse move beyond a small dead zone on any window MUST exit.
  The first `WM_MOUSEMOVE` after startup MUST be swallowed, because Windows delivers one
  immediately.
- On exit the process MUST wait for device idle before destroying anything, and MUST leave no GPU
  allocations outstanding.

### 9.3 Preview

Preview windows are a few hundred pixels across and the host may destroy them at any moment.
Standing up a Vulkan device for that is not worth the risk or the wait.

- Preview MUST NOT initialise Vulkan.
- It MUST cross-fade slowly between a small number of stills baked into the executable's
  resources — frames captured from a real run, one per major phase.
- It MUST survive its parent window vanishing.

**Interim state.** The stills cannot exist until there is a renderer to capture them from, which
makes baking them an M6 task. Until then, preview MUST paint black and MUST still meet every
other requirement above — no Vulkan, no crash when the parent goes away, correct exit. A black
preview is an accepted interim state, not an open defect, and A7 is scored against the behavior
rather than the picture until M6.

## 10. Settings

Stored under `HKCU\Software\nuke-saver`. Every value has a working default; a fresh install MUST
run correctly against an empty registry key.

| Value | Type | Default | Meaning |
|---|---|---|---|
| `Quality` | DWORD | 0 | 0 = auto (11.2), 1 = low, 2 = medium, 3 = high |
| `TimeOfDay` | DWORD | 3 | 0 = random per cycle, 1 = morning, 2 = noon, **3 = twilight (default)**, 4 = night |
| `CameraMode` | DWORD | 0 | 0 = random shot per cycle, 1–4 = pin to one shot type |

The `/c` dialog MUST expose these three and nothing else, and MUST be a plain Win32 dialog
resource.

## 11. Camera and performance

### 11.1 Camera

**The camera is never still.** It orbits the city for the whole cycle, so the scene is always
moving even when nothing in it is. A static shot of a static city is exactly the failure this
project cannot afford — it is a screen saver, and a still frame is the one thing it must never
show.

- The camera MUST follow a continuous orbit around the city centre for the entire cycle, from the
  first frame of phase 0 to the last frame of phase 10. It MUST NOT stop, hold, or cut at any
  point, including during the countdown.
- Orbit rate MUST be slow enough to read as drift rather than as a turntable: a full revolution
  SHOULD take 4–8 minutes, so one cycle sweeps 30–90° of arc, not a full circle.
- Radius, height and look-at MUST also vary slowly and independently over the cycle — easing in
  or out, rising or descending — so the motion is not a flat circular track. Each cycle draws its
  own combination from a small library of shot types: distant ridge orbit, low fast orbit across
  the desert floor, high oblique descending orbit, close orbit rising from street level.
- Direction of travel MUST be randomised per cycle.
- Opening **elevation** MUST vary per cycle for the low shots. A pass across the desert floor is
  defined by being low, but opening every cycle from the same handspan above the sand makes the
  one shot that should read as a pass read as the same pass. The opening elevation is drawn from
  level up to about 30°, weighted heavily toward the bottom of that range so most cycles keep the
  low pass and a minority look down onto the basin.
- The framing solver runs before the cycle starts and MUST satisfy all of these across the whole
  orbit arc, without a cut or a zoom: the city fits during phase 1; the countdown board is legible
  through phases 3 and 4 (7.6 places and turns the board against this camera, so the solver and
  the board are solved in that order); the missile is on screen for its arrival (7.1); the full cloud
  fits at its widest in phase 8; the settled debris field fits in phase 10.
- Cloud framing is the binding constraint — it is the widest thing in the cycle — so the solver
  MUST size the orbit radius from phase 8 and then check the rest against it.
- A brief damped jolt when the blast front passes the camera is permitted. Nothing else
  interrupts the motion.
- Aspect-aware, so the shot works at 16:9 and 21:9 without cropping the cloud.

### 11.2 Performance

Reference machine: AMD Radeon 8060S, 3440 × 1440 @ 100 Hz.

- Target 60 FPS sustained at native resolution at `medium`. This is a screen saver; it MUST NOT
  work the GPU harder than it needs to.
- The renderer MUST measure its own frame time over a rolling window and, in `auto`, step down
  after missing budget for 2 s and back up after beating it with margin for 10 s, damped so it
  does not oscillate.
- Quality controls, in the order they are sacrificed: fragments per building (300 → 200 → 120 →
  60), particle peak counts, bloom mip count, shadow resolution, terrain LOD distance. Building
  count stays at 500 at every level — the city is the subject.
- A frame MUST NOT be rendered for a window whose monitor is asleep or whose swapchain reports
  `VK_ERROR_OUT_OF_DATE_KHR`; recreate and continue.
- Presentation SHOULD prefer `FIFO` and MUST NOT busy-wait to pace frames.

## 12. Failure behavior

- Vulkan MUST be loaded dynamically at runtime. The executable MUST NOT import `vulkan-1.dll`
  statically, so a machine with no Vulkan runtime still launches it.
- If the loader is missing, no suitable device exists, device creation fails, or a required format
  is unsupported, the saver MUST fall back to a plain black GDI screen saver that still exits on
  input. It MUST NOT show an error dialog and MUST NOT exit immediately — either looks like a
  crash to whoever set it.
- Device loss MUST be caught: one full teardown and re-init, then the GDI fallback.
- Any unhandled failure path MUST end with the process exiting 0, quietly.

## 13. Build

### 13.1 Language standard

`-std=c++11` MUST be raised to `-std=c++17`. Nothing here needs C++20, and staying at 17 keeps
MinGW's static runtime uncomplicated.

### 13.2 Dependencies

Neither of the first two is installed on the current machine; M0 in the plan handles that.

| Dependency | Why | How |
|---|---|---|
| Vulkan headers | Building at all | `pacman -S mingw-w64-ucrt-x86_64-vulkan-devel` |
| `glslc` | Compiling GLSL to SPIR-V | `pacman -S mingw-w64-ucrt-x86_64-shaderc` |
| `volk` | Dynamic loader, meta-loader for entry points | Vendored, one `.c` plus header |
| VMA | Device memory suballocation | Vendored, header-only |
| `vulkan-1.dll` | Runtime | Already present in `System32` |

Vendored dependencies MUST live in `third_party/` and be checked in, so the build needs no
network.

### 13.2.1 Diagnostics

Neither of these is required to build or run, and neither MUST ever be on in a shipped build.

`NUKE_SAVER_LOG=1` writes a trace to `%TEMP%\nuke-saver.log`. A `-mwindows` binary has no
console, so this is the only way to see what a run did.

Vulkan validation is gated on that same variable, because a screen saver has no business loading
a layer on a user's machine. MSYS2 ships the layer but does not register it with the loader, so
it also needs `VK_LAYER_PATH=C:\msys64\ucrt64\bin` (package
`mingw-w64-ucrt-x86_64-vulkan-validation-layers`, already installed). Without that variable the
renderer silently runs unvalidated: the log line `vulkan: validation enabled` is the only
confirmation it took.

### 13.3 Shaders

- GLSL under `shaders/`, compiled to SPIR-V by `glslc` as a Makefile rule.
- Compiled SPIR-V MUST be embedded in the executable. Loading `.spv` from disk violates 6.1.
- Shader compilation MUST fail the build.

### 13.4 Size and structure

- The finished `.scr` SHOULD stay under 8 MB.
- The Makefile MUST keep its current shape — `all`, `clean`, `install`, temp files in
  `build/tmp`, `-municode -DUNICODE -D_UNICODE`, `-mwindows`, `-static`. It grows a shader rule, a
  multi-TU object rule and the new libraries. It does not become CMake.

## 14. Acceptance tests

| # | Test | Pass condition |
|---:|---|---|
| A1 | `make` from a clean tree | Builds `nuke-saver.scr`, zero warnings under `-Wall -Wextra` |
| A2 | Run with no arguments | Full-screen on every monitor, cursor hidden |
| A3 | Press a key at any point | Exits within 250 ms, exit code 0 |
| A4 | Move the mouse | Same, and not triggered by the spurious first move |
| A5 | Two consecutive cycles | Different city, different camera shot |
| A6 | Same seed forced twice | Identical city, missile approach and gross cloud shape |
| A7 | Preview in Screen Saver Settings | Creates no Vulkan device, survives dialog close; black before M6, cross-fading stills after |
| A8 | `/c` | Dialog opens, all three settings persist across a restart |
| A9 | Rename `vulkan-1.dll` and run | Black screen saver, exits on input, no error dialog |
| A10 | Reference machine, medium, 3440×1440 | 60 FPS sustained through phases 6–9 |
| A11 | 30-minute soak | No growth in working set or VRAM, no crash |
| A12 | Unplug a monitor mid-run | Survives; remaining monitors keep rendering |
| A13 | Count buildings at end of phase 1 | Exactly 500, all standing, none visible before its start time, board up last |
| A14 | Time the countdown against a stopwatch | 5.00 s ±50 ms, digits step once per second, no drift |
| A15 | Frame-step phase 6 | No building is fragmented before the shell reaches it |
| A16 | Frame at end of phase 7 | Majority of fragments at rest on the ground, outside the city footprint |
| A17 | Frame at mid-phase 8 | Recognisable mushroom, fragments still carrying building colours |
| A31 | Frame-step 10 s of phase 8 across the cap | Tube circulates about its own axis: material rising on the inner face, descending at the outer rim; roughly one revolution per 20 s |
| A32 | Frame-step the first 5 s of phase 9 | Every ember turns grey and falls; none still climbing; no new hot embers after the phase starts |
| A18 | Sample 50 building colours | Spread consistent with ±10% HSV variation; no two adjacent buildings identical |
| A19 | Run each `TimeOfDay` setting | Sun angle, sun colour, sky and exposure all differ; windows brightest at night |
| A20 | Board legibility, every shot type, 16:9 and 21:9 | All eight glyphs identifiable through phases 3–4 from every point on the orbit arc, partial occlusion permitted |
| A21 | Board light at night | Rooftops beneath the board visibly lit amber; board is the dominant city light |
| A22 | Board yaw across a cycle | Fixed after phase 1; no per-frame rotation toward the camera; all four faces agree every frame |
| A23 | Frame-step phase 9 | Stem empties before the cap; release is staggered, never all at once |
| A24 | Frame at end of phase 9 | Cloud gone, majority of fragments at rest, debris field over the scorched footprint |
| A25 | Sample camera position every second of a full cycle | Always moving; no frame-to-frame repeat; total arc 30–90°; direction varies between cycles |
| A26 | Orbit a full arc at every shot height, all four times of day | Mountain range closes the horizon at every point; no gap of sky meeting flat ground; no building breaks the skyline behind the range |
| A27 | Compare drawn sun/moon position to shadow direction | Shadows fall consistently with the body on the skybox, checked at morning and twilight where shadows are longest |
| A28 | Night and twilight | Stars visible, fixed to sky rather than camera, brightness scaling with ambient; moon reads as the key light at night |
| A29 | Board glyph scale against skyline | Glyph height comparable to the tallest buildings; digits read as part of the skyline, foreground buildings parallax across them as the camera orbits |
| A30 | Fresh install, empty registry | Runs at twilight |

## 15. Out of scope

Audio. HDR display output — the pipeline is HDR internally, the swapchain is SDR. Networked sync.
Configuration beyond section 10. Physically accurate anything. Real-world geography or any
identifiable real city. Ray tracing extensions. Non-Windows platforms. Any 32-bit build.
Rigid-body collision between fragments — they collide with the ground and nothing else.

**Weather.** No cloud layer, no rain, no snow, no dust storms, no wind gusts. The sky is a
gradient, stars and one celestial body. Atmosphere is distance haze and nothing else. Wind exists
only as a single constant drift direction per cycle for the mushroom cloud and the smoke. This is
a deliberate simplification: a volumetric cloud layer would cost more than the entire fragment
simulation, and a clear sky suits a desert.

## 16. Open questions

None. Every question raised across revisions 1–4 is resolved:

| | Question | Resolution | Revision |
|---|---|---|---|
| Q1 | Default time of day | Twilight (section 10) | 4 |
| Q2 | Countdown placement | World object, not an overlay (7.6) | 3 |
| Q3 | Preview before stills exist | Black until M6, accepted (9.3) | 5 |
| Q4 | Does the cloud hold or disperse | Disperses, own phase (7.7) | 3 |
| Q5 | Horizon at the widest camera | Far-field impostor, triangular range (6.3) | 4 |
| Q6 | Board content | The time and nothing else (7.6) | 4 |

Numbering is kept so earlier discussion still refers to the right thing. New questions append
from Q7.
