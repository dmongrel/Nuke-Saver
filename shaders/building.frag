#version 450

// Building surfaces (spec 6.4, 5.3, 5.4).
//
// Windows are generated from the surface, never textured. The grid is laid out in *metres* rather
// than in the face's own uv, so a forty metre facade gets ten bays and a twelve metre one gets
// three — with uv the two would get the same count and the small building would read as a model of
// the large one rather than as a smaller building.

#include "atmosphere.glsl"

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec3  vBodyColor;
layout(location = 3) in vec3  vWindowColor;
layout(location = 4) in float vFacadeU;
layout(location = 5) in float vWindowSeed;

layout(location = 0) out vec4 outColor;

const float kGroundFloor = 5.0;  // no windows below this, so the base reads as a base

float WindowHash(vec2 cell, float seed) {
    vec3 p = fract(vec3(cell.x, cell.y, seed) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

void main() {
    vec3  toCamera = scene.cameraPos.xyz - vWorldPos;
    float distance = length(toCamera);
    vec3  viewDir  = toCamera / max(distance, 1e-4);

    vec3 normal = normalize(vNormal);

    // Only the walls have windows; the roof does not.
    float wall = step(abs(normal.y), 0.5);

    // The window pitch, per building. Derived from the same seed that decides which panes are
    // lit, so it costs nothing to carry: with one pitch for the whole city, five hundred
    // buildings read as five hundred crops of a single texture.
    float bay   = 3.4 + 2.0 * fract(vWindowSeed * 0.0137);
    float floorH = 3.1 + 1.2 * fract(vWindowSeed * 0.0261 + 0.37);

    vec2 grid = vec2(vFacadeU / bay, vWorldPos.y / floorH);
    vec2 cell = floor(grid);
    vec2 f    = grid - cell;

    // The pane sits inside its bay with a mullion around it, antialiased against the pixel
    // footprint of the grid.
    vec2 fw   = max(fwidth(grid), vec2(1e-5));
    vec2 lo   = vec2(0.20, 0.26);
    vec2 hi   = vec2(0.80, 0.82);
    vec2 pane = smoothstep(lo - fw, lo + fw, f) * (1.0 - smoothstep(hi - fw, hi + fw, f));

    float facade = wall * step(kGroundFloor, vWorldPos.y);

    // Spec 5.4: the lit state is stable per window and per building. A pane lit one frame and
    // dark the next reads as noise, so this is a hash of the cell, not of anything per frame.
    //
    // How many are lit, and how hard, both fall off far faster than the daylight does. The two
    // are not the same curve as the ambient: at morning a window with its light on is a slightly
    // darker rectangle, not a bright one, because the sky outside is brighter than the room. A
    // linear ramp put a quarter of every facade at full emission under a morning sun, and the
    // city read as a circuit board.
    float night    = scene.ambientColor.w;
    float litShare = 0.03 + 0.50 * pow(night, 1.5);
    float isLit    = step(WindowHash(cell, vWindowSeed), litShare);

    // Level of detail, and the thing that decides whether a city three kilometres away reads as a
    // city or as static. Once a bay covers less than about a pixel, no amount of antialiasing
    // inside the bay helps: each pixel lands on one arbitrary pane and the whole facade turns into
    // a field of sparkling dots that crawls as the camera moves. So past that point the pattern is
    // dissolved into the *average* it would have had, which is what a correctly filtered version
    // of it converges to anyway.
    float resolved = 1.0 - smoothstep(0.30, 0.85, max(fw.x, fw.y));

    const float kPaneCoverage = 0.60 * 0.56;  // mean pane area within a bay, matching lo and hi

    float window = mix(kPaneCoverage, pane.x * pane.y, resolved) * facade;
    float lit    = mix(kPaneCoverage * litShare, pane.x * pane.y * isLit, resolved) * facade;

    // Glass unlit is darker than the wall around it, not lighter: it is a hole into an unlit room
    // with a little sky reflected off it. The palette entry of spec 5.3 is the colour of that
    // reflection, so it is the *tone* that has to come down, not the hue.
    vec3 glass  = vWindowColor * 0.35;
    vec3 albedo = mix(vBodyColor, glass, window * 0.7);

    vec3  keyDir   = scene.keyDirection.xyz;
    float lambert  = max(dot(normal, keyDir), 0.0);
    float keyAbove = smoothstep(-0.08, 0.06, keyDir.y);

    vec3 shaded = albedo * scene.keyColor.rgb * lambert * keyAbove;

    // The sky this surface actually sits under, not a single authored constant. At twilight the
    // ambient colour of spec 5.4 is the violet overhead, which ignores the orange band filling
    // half the hemisphere and left the desert floor as a void with a lit city floating on it.
    // Taking the greater of the two keeps night — where the authored value is deliberately above
    // the almost-black gradient — from going darker still.
    //
    // Weighted toward the zenith because irradiance on a level surface weights by the cosine, and
    // the horizon band arrives at a grazing angle. A twilight foreground is meant to be dark; it
    // is not meant to be nothing.
    vec3 skyAmbient = max(scene.ambientColor.rgb,
                          mix(scene.horizonColor.rgb, scene.zenithColor.rgb, 0.7));

    // Weighted upward the same way the ground is, plus a little bounce off the street so the
    // shaded sides of the boxes do not go to black in a city with no global illumination.
    float skyFacing = 0.5 + 0.5 * normal.y;
    shaded += albedo * skyAmbient * scene.groundColor.w * skyFacing;
    shaded += albedo * scene.groundColor.rgb * scene.groundColor.w * (1.0 - skyFacing) * 0.35;

    // Emission. A window's lamp does not get brighter after dark — what changes is everything
    // around it — so the radiance here is held constant in *display* terms by dividing out the
    // base exposure of the time of day (scene.horizonColor.w). Scaling it by the ambient instead
    // made twilight, whose exposure is the highest of the four, a city of white rectangles.
    //
    // What the time of day does change is how many are on, which is `litShare` above.
    //
    // The lamp is tungsten, not the colour of the glass. Spec 5.3's window entry is a blue, and it
    // is the right blue — it is the sky reflected off a dark pane — but the light *behind* the
    // pane is a room, and a city lit blue at night reads as an office block at three in the
    // morning rather than as a city. Per-building warmth, so the skyline is not one lamp.
    vec3 lamp = vec3(1.0, 0.76, 0.46) * (0.75 + 0.5 * fract(vWindowSeed * 0.0713));

    shaded += lamp * lit * (2.2 / max(scene.horizonColor.w, 1e-3));

    vec3 haze = SkyGradient(-viewDir);
    outColor  = vec4(mix(shaded, haze, HazeAmount(distance)), 1.0);
}
