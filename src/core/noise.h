// Noise (implementation plan, M3).
//
// Written once and shared by terrain, the horizon range, fragment swirl and particles, so that
// the world has one underlying character rather than four unrelated ones.
//
// Gradient noise rather than value noise for the primary: value noise has visible axis-aligned
// structure that reads as a grid on a large flat desert, which is precisely the case here.
#ifndef NUKE_SAVER_CORE_NOISE_H
#define NUKE_SAVER_CORE_NOISE_H

#include "core/math.h"
#include "core/rng.h"

namespace core {

// Parameters for a summed-octave fBm. Spec 6.2 asks for 5-7 octaves with ridged noise at the
// low frequencies.
struct FbmParams {
    int   octaves     = 6;
    float frequency   = 1.0f;
    float amplitude   = 1.0f;
    float lacunarity  = 2.0f;   // frequency multiplier per octave
    float gain        = 0.5f;   // amplitude multiplier per octave
};

// 2D gradient (Perlin-style) noise in roughly [-1,1]. The seed selects the gradient field, so
// two different seeds give genuinely different terrain rather than the same terrain offset.
float GradientNoise2(float x, float y, uint64_t seed);

// Summed octaves of GradientNoise2. Roughly [-1,1] for the default gain.
float Fbm2(float x, float y, uint64_t seed, const FbmParams& p);

// Ridged multifractal: 1 - |noise|, squared, which turns the zero crossings into sharp crests.
// This is what produces mesas and a hill rim rather than smooth dunes (spec 6.2).
float Ridged2(float x, float y, uint64_t seed, const FbmParams& p);

// The frequency of the finest octave a set of parameters produces. Anything sampling the field
// by finite difference needs this: a step coarser than the finest octave silently returns the
// derivative of a smoother field than the one that was asked for.
float FbmMaxFrequency(const FbmParams& p);

// The differencing step Curl2 uses by default: a quarter-wavelength of the finest octave, so
// the result is the curl of the field as authored rather than of an accidentally smoothed
// version of it. Exposed so an integrator stepping through the field, or a test measuring its
// divergence, can match it - discrete divergence only vanishes when the stencils agree.
float Curl2Step(const FbmParams& p);

// Curl of a 2D noise field, giving a divergence-free vector field. Used for swirl in the
// fragment simulation and the ember drift: divergence-free is what keeps particles from piling
// up in sinks, which is how noise-driven motion usually gives itself away.
//
// `step` defaults to Curl2Step(p). Pass a larger one deliberately to get broader, smoother
// swirl from the same field.
Vec2 Curl2(float x, float y, uint64_t seed, const FbmParams& p, float step = 0.0f);

}  // namespace core

#endif
