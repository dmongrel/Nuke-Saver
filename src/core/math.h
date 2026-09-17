// Small vector and matrix types (implementation plan, M3).
//
// Deliberately not a general maths library. It carries exactly what the project needs and
// nothing else, because a screen saver that must stay under 8 MB and build with no network
// cannot justify pulling in GLM for four operations.
//
// Conventions, chosen once so nothing has to guess later:
//   - Right-handed, Y up, metres.
//   - Matrices are column-major and multiply column vectors, matching GLSL, so a Mat4 can be
//     memcpy'd into a uniform buffer without transposing.
//   - Perspective produces Vulkan clip space: depth 0..1, and Y already flipped, so no shader
//     and no viewport has to remember to do it.
#ifndef NUKE_SAVER_CORE_MATH_H
#define NUKE_SAVER_CORE_MATH_H

#include <cmath>
#include <cstring>

namespace core {

constexpr float kPi     = 3.14159265358979323846f;
constexpr float kTwoPi  = 6.28318530717958647692f;
constexpr float kDegToRad = kPi / 180.0f;

inline float Radians(float degrees) { return degrees * kDegToRad; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Hermite smoothstep on an already-normalised t.
inline float SmoothStep(float t) {
    t = Saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// Cubic ease-out: fast start, decelerating into place. Growth (spec 6.5) requires a building to
// decelerate into its final height without overshoot, which rules out anything springy.
inline float EaseOutCubic(float t) {
    t = Saturate(t);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(const Vec2& a, float s) { return {a.x * s, a.y * s}; }
inline float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float Length(const Vec2& v) { return std::sqrt(Dot(v, v)); }

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3& a) { return a * s; }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { a = a + b; return a; }
inline Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float LengthSq(const Vec3& v) { return Dot(v, v); }
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Returns the zero vector for a zero-length input rather than NaN. Callers here are generating
// directions from random data, and one degenerate sample should not poison a whole frame.
inline Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    return len > 1e-8f ? v * (1.0f / len) : Vec3{0.0f, 0.0f, 0.0f};
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// Column-major, m[column][row] — the same memory order GLSL expects.
struct Mat4 {
    float m[4][4]{};

    static Mat4 Identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r.m[c][row] = a.m[0][row] * b.m[c][0] + a.m[1][row] * b.m[c][1] +
                          a.m[2][row] * b.m[c][2] + a.m[3][row] * b.m[c][3];
        }
    }
    return r;
}

inline Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z + a.m[3][0] * v.w,
            a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z + a.m[3][1] * v.w,
            a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z + a.m[3][2] * v.w,
            a.m[0][3] * v.x + a.m[1][3] * v.y + a.m[2][3] * v.z + a.m[3][3] * v.w};
}

inline Mat4 Translate(const Vec3& t) {
    Mat4 r  = Mat4::Identity();
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}

inline Mat4 Scale(const Vec3& s) {
    Mat4 r;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 RotateY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4        r = Mat4::Identity();
    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
}

// Right-handed look-at. `up` is a hint; the caller may pass world up even when looking straight
// down, and this picks a fallback rather than producing a degenerate basis.
inline Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = Normalize(target - eye);
    if (LengthSq(f) < 1e-12f) f = Vec3{0.0f, 0.0f, -1.0f};

    Vec3 s = Cross(f, up);
    if (LengthSq(s) < 1e-8f) s = Cross(f, Vec3{0.0f, 0.0f, 1.0f});  // looking along up
    s = Normalize(s);

    const Vec3 u = Cross(s, f);

    Mat4 r    = Mat4::Identity();
    r.m[0][0] = s.x;  r.m[1][0] = s.y;  r.m[2][0] = s.z;
    r.m[0][1] = u.x;  r.m[1][1] = u.y;  r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -Dot(s, eye);
    r.m[3][1] = -Dot(u, eye);
    r.m[3][2] = Dot(f, eye);
    return r;
}

// Vulkan clip space: z in 0..1, Y flipped here so no downstream code repeats the correction.
inline Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar) {
    const float t = 1.0f / std::tan(fovYRadians * 0.5f);

    Mat4 r{};
    r.m[0][0] = t / aspect;
    r.m[1][1] = -t;  // Vulkan's NDC Y points down
    r.m[2][2] = zFar / (zNear - zFar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (zNear * zFar) / (zNear - zFar);
    return r;
}

}  // namespace core

#endif
