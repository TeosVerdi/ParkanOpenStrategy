// Minimal vector/matrix math. Matrices are column-major (column vectors, M * v), matching Metal.
// The world is Z-up with +Y forward (notes/control-physics.md).
#pragma once

#include <array>
#include <cmath>

namespace parkan {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 v) {
    float l = std::sqrt(dot(v, v));
    return l > 0 ? v * (1 / l) : v;
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat conjugate() const { return {-x, -y, -z, w}; }
};

// Ngi32 g_FastProc QuatSlerp (10017f50, slots +0x44/+0x58): shortest path, plain lerp when
// 1 - |cos| <= 1e-5, no renormalisation.
inline Quat slerp(Quat a, Quat b, float t) {
    float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, sgn = 1, w1, w2;
    if (c < 0) c = -c, sgn = -1;
    if (1 - c <= 1e-5f) {
        w1 = 1 - t, w2 = t;
    } else {
        const float th = std::acos(c);
        w2 = std::sin(th * t) / std::sqrt(1 - c * c);
        w1 = std::cos(th * t) - w2 * c;
    }
    w2 *= sgn;
    return {w2 * b.x + w1 * a.x, w2 * b.y + w1 * a.y, w2 * b.z + w1 * a.z, w2 * b.w + w1 * a.w};
}

struct Mat4 {
    std::array<float, 16> m{}; // column-major: m[col * 4 + row]
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 identity() {
        Mat4 r;
        r.at(0, 0) = r.at(1, 1) = r.at(2, 2) = r.at(3, 3) = 1;
        return r;
    }
    static Mat4 translation(Vec3 t) {
        Mat4 r = identity();
        r.at(0, 3) = t.x, r.at(1, 3) = t.y, r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 rotation(Quat q) {
        Mat4 r = identity();
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        r.at(0, 0) = 1 - 2 * (y * y + z * z), r.at(0, 1) = 2 * (x * y - z * w), r.at(0, 2) = 2 * (x * z + y * w);
        r.at(1, 0) = 2 * (x * y + z * w), r.at(1, 1) = 1 - 2 * (x * x + z * z), r.at(1, 2) = 2 * (y * z - x * w);
        r.at(2, 0) = 2 * (x * z - y * w), r.at(2, 1) = 2 * (y * z + x * w), r.at(2, 2) = 1 - 2 * (x * x + y * y);
        return r;
    }
    // Right-handed view looking from `eye` at `target`.
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = normalize(target - eye), s = normalize(cross(f, up)), u = cross(s, f);
        Mat4 r = identity();
        r.at(0, 0) = s.x, r.at(0, 1) = s.y, r.at(0, 2) = s.z, r.at(0, 3) = -dot(s, eye);
        r.at(1, 0) = u.x, r.at(1, 1) = u.y, r.at(1, 2) = u.z, r.at(1, 3) = -dot(u, eye);
        r.at(2, 0) = -f.x, r.at(2, 1) = -f.y, r.at(2, 2) = -f.z, r.at(2, 3) = dot(f, eye);
        return r;
    }
    // Right-handed perspective with clip-space depth in [0, 1] (Metal).
    static Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
        const float f = 1.f / std::tan(fovy / 2);
        Mat4 r;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = zfar / (znear - zfar);
        r.at(2, 3) = znear * zfar / (znear - zfar);
        r.at(3, 2) = -1;
        return r;
    }
    Vec3 transform_point(Vec3 p) const {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(rr, k) * b.at(k, c);
            r.at(rr, c) = s;
        }
    return r;
}

} // namespace parkan
