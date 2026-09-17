// The key light's shadow map (spec 8.2, 11.2).
//
// One directional cascade, not several. The scene is bounded — a city of a known radius, a debris
// field of a known reach, a cloud of a known height — so a single box fitted to the whole of it
// covers everything that casts, and cascades would be splitting a frustum that was never the
// problem. The fit is done once per cycle, from numbers the world already settled, which also
// means the matrix does not move while the cycle runs: a light matrix that changes every frame
// makes the shadow edges crawl, and the usual cure for that is to snap the fit to texel
// increments. Not moving at all is cheaper and exact.
//
// Kept out of the renderer so the fit can be checked against a box and a sun angle in a test
// rather than by looking at a picture.
#ifndef NUKE_SAVER_RENDER_SHADOW_H
#define NUKE_SAVER_RENDER_SHADOW_H

#include "core/math.h"

#include <cstdint>

namespace render {

// What the map has to cover: a box centred on the city, from the ground up.
//
// A box rather than the exact hull of the city, because the things that cast move — fragments
// scatter to the shell's reach and then gather a kilometre up — and a fit that tracked them would
// be a fit that changes every frame.
struct ShadowBounds {
    float radius = 1.0f;  // horizontal half-extent about the world origin, metres
    float top    = 1.0f;  // the highest thing that casts, metres above the ground
};

// The light's view-projection, for a key shining *from* `towardLight` — the direction the surface
// looks along to see the light, which is how scene.keyDirection holds it.
core::Mat4 ShadowViewProj(const core::Vec3& towardLight, const ShadowBounds& bounds);

// How wide one texel of the map is, in metres, at the fitted extent. The receiver offsets along
// its own normal by about this much before looking the depth up: the alternative is a constant
// bias big enough for the worst texel, which is a bias big enough to lift every shadow off the
// thing casting it.
float ShadowTexelWorldSize(const ShadowBounds& bounds, uint32_t mapSize);

// Spec 11.2 names shadow resolution as a quality lever and it is the only one of the five that is
// a single number. Level 0 is the best rung.
uint32_t ShadowMapSize(int qualityLevel);

}  // namespace render

#endif
