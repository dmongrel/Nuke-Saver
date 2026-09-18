// Auto-exposure, shared between the two compute passes and the tonemap (spec 8.2).
//
// "Auto-exposure is not a nicety. It is the effect that sells the detonation." The white-out and
// the slow recovery are supposed to fall out of the adaptation rather than out of a scripted fade,
// which means the flash has to be in the HDR buffer this reads and the recovery has to be this
// climbing back up afterwards. Nothing about phase 5 is written anywhere as a curve.

#ifndef NUKE_SAVER_EXPOSURE_GLSL
#define NUKE_SAVER_EXPOSURE_GLSL

// The average display luminance the adaptation aims for.
//
// Not the textbook 0.18. That figure assumes a scene whose average reflectance is mid grey, and
// none of the four times of day in spec 5.4 is: twilight is a dark basin under a bright band, and
// its log-average luminance at the authored exposure measures 0.07. Targeting 0.18 pushed twilight
// two and a half stops brighter than spec 5.4's base exposure authored it, which is not adaptation
// — it is overriding the time of day.
//
// So the key is the measured average of the authored scene, which makes the adaptation neutral on
// an ordinary frame and leaves it doing the one job spec 8.2 actually gives it: slamming down when
// the flash arrives and taking three seconds to come back.
const float kExposureKey = 0.07;

// The histogram covers this range of log2 luminance. The bottom is a night desert lit by nothing
// but a moon; the top is the fireball, which is four orders of magnitude above it. Wider than it
// needs to be at any one moment, and it has to be, because both of those are in the same cycle.
const float kLogLumMin   = -10.0;
const float kLogLumRange = 26.0;
const int   kBins        = 256;

// The tonemap only reads it; the histogram and the adapt pass write it. Same reason as the
// fragment buffers: a fragment stage that might write needs fragmentStoresAndAtomics, which is
// not enabled, so the read-only path has to say it is read-only.
#ifdef EXPOSURE_READONLY
#define EXPOSURE_ACCESS readonly
#else
#define EXPOSURE_ACCESS
#endif

// Binding 1 in the tonemap's set, which the histogram and adapt passes share. The first bloom
// downsample bins the histogram too and binds it after its two images, at 2.
#ifndef EXPOSURE_BINDING
#define EXPOSURE_BINDING 1
#endif

layout(set = 0, binding = EXPOSURE_BINDING, std430) EXPOSURE_ACCESS buffer Exposure {
    float exposure;     // what the tonemap multiplies by; carried across frames
    float initialised;  // 0 until the first adapt has run, so frame 0 does not fade in from black
    float measured;     // the last measured average luminance, for logging
    float pad;
    uint  bins[kBins];
} ex;

float BinToLogLum(float bin) { return kLogLumMin + (bin / float(kBins - 1)) * kLogLumRange; }

int LogLumToBin(float luminance) {
    if (luminance < 1e-6) return 0;  // bin 0 is the "black" bucket and is excluded from the average
    float t = (log2(luminance) - kLogLumMin) / kLogLumRange;
    return clamp(int(t * float(kBins - 1) + 1.0), 1, kBins - 1);
}

float Luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

#endif
