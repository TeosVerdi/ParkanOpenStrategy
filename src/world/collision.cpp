#include "world/collision.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace parkan {

namespace {

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

std::optional<float> swept_sphere_toi(Vec3 a0, Vec3 a1, float ra, Vec3 b0, Vec3 b1, float rb) {
    const Vec3 dp = b0 - a0, d = (b1 - b0) - (a1 - a0);
    const float r = ra + rb;
    const float c = dot(dp, dp) - r * r;
    if (c <= 0) return 0.f; // overlapping at the start
    const float a = dot(d, d), b = 2 * dot(dp, d);
    if (a <= 1e-12f) return std::nullopt;
    const float disc = b * b - 4 * a * c;
    if (disc < 0) return std::nullopt;
    const float t = (-b - std::sqrt(disc)) / (2 * a);
    if (t < 0 || t > 1) return std::nullopt;
    return t;
}

std::pair<Vec3, Vec3> split_push(Vec3 p, float mass_a, float mass_b) {
    const float a2 = mass_a * mass_a, b2 = mass_b * mass_b, sum = a2 + b2;
    if (sum <= 0) return {p * -0.5f, p * 0.5f};
    return {p * (-b2 / sum), p * (a2 / sum)};
}

Vec3 contain_in_world_bounds(Vec3 c, float r, Vec3 lo, Vec3 hi) {
    const float ceiling = hi.z + 20.f;
    Vec3 out = c;
    out.x = std::clamp(out.x, lo.x + r, hi.x - r);
    out.y = std::clamp(out.y, lo.y + r, hi.y - r);
    out.z = std::min(out.z, ceiling - r);
    // Soft walls: 80 m border band in xy and the 20 m band under the ceiling.
    constexpr float kBorder = 80.f, kBorderGain = 3 * 0.0125f, kCeilingBand = 20.f, kCeilingGain = 3 * 0.05f;
    if (const float d = (lo.x + kBorder) - out.x; d > 0) out.x += kBorderGain * d;
    if (const float d = out.x - (hi.x - kBorder); d > 0) out.x -= kBorderGain * d;
    if (const float d = (lo.y + kBorder) - out.y; d > 0) out.y += kBorderGain * d;
    if (const float d = out.y - (hi.y - kBorder); d > 0) out.y -= kBorderGain * d;
    if (const float d = out.z - (ceiling - kCeilingBand); d > 0) out.z -= kCeilingGain * d;
    return out - c;
}

Vec3 horizontal_push(Vec3 p) {
    const float len = std::sqrt(dot(p, p)), xy = std::sqrt(p.x * p.x + p.y * p.y);
    if (xy <= 1e-6f) return {};
    const float k = std::min(len / xy, 4.f);
    return {p.x * k, p.y * k, 0};
}

void StaticColliders::add(const RenderMesh& mesh, std::uint32_t first, std::uint32_t last) {
    for (const auto& d : mesh.draws) {
        if (d.transform < first || d.transform >= last) continue;
        const Mat4& m = mesh.transforms[d.transform];
        for (std::uint32_t i = 0; i + 2 < d.index_count; i += 3) {
            Vec3 v[3];
            for (int k = 0; k < 3; ++k) {
                const auto& rv = mesh.vertices[std::size_t(d.base_vertex + std::int32_t(mesh.indices[d.index_start + i + std::uint32_t(k)]))];
                v[k] = m.transform_point({rv.pos[0], rv.pos[1], rv.pos[2]});
            }
            const Vec3 n = cross(v[1] - v[0], v[2] - v[0]);
            const float len = std::sqrt(dot(n, n));
            if (len <= 1e-9f || std::abs(n.z) / len >= 0.7f) continue; // floors and roofs do not push
            tris_.push_back({v[0], v[1], v[2]});
        }
    }
}

void StaticColliders::finish() {
    if (tris_.empty()) return;
    min_ = {FLT_MAX, FLT_MAX, FLT_MAX};
    max_ = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& t : tris_)
        for (const Vec3& p : {t.a, t.b, t.c}) {
            min_ = {std::min(min_.x, p.x), std::min(min_.y, p.y), std::min(min_.z, p.z)};
            max_ = {std::max(max_.x, p.x), std::max(max_.y, p.y), std::max(max_.z, p.z)};
        }
    nx_ = std::max(1, int((max_.x - min_.x) / cell_) + 1);
    ny_ = std::max(1, int((max_.y - min_.y) / cell_) + 1);
    cells_.assign(std::size_t(nx_) * std::size_t(ny_), {});
    for (std::uint32_t i = 0; i < tris_.size(); ++i) {
        const Tri& t = tris_[i];
        const int x0 = std::clamp(int((std::min({t.a.x, t.b.x, t.c.x}) - min_.x) / cell_), 0, nx_ - 1);
        const int x1 = std::clamp(int((std::max({t.a.x, t.b.x, t.c.x}) - min_.x) / cell_), 0, nx_ - 1);
        const int y0 = std::clamp(int((std::min({t.a.y, t.b.y, t.c.y}) - min_.y) / cell_), 0, ny_ - 1);
        const int y1 = std::clamp(int((std::max({t.a.y, t.b.y, t.c.y}) - min_.y) / cell_), 0, ny_ - 1);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) cells_[std::size_t(x) * std::size_t(ny_) + std::size_t(y)].push_back(i);
    }
}

Vec3 StaticColliders::push_sphere(Vec3 c, float r) const {
    if (cells_.empty()) return {};
    Vec3 total{};
    for (int pass = 0; pass < 2; ++pass) {
        const Vec3 p = c + total;
        const int x0 = std::clamp(int((p.x - r - min_.x) / cell_), 0, nx_ - 1), x1 = std::clamp(int((p.x + r - min_.x) / cell_), 0, nx_ - 1);
        const int y0 = std::clamp(int((p.y - r - min_.y) / cell_), 0, ny_ - 1), y1 = std::clamp(int((p.y + r - min_.y) / cell_), 0, ny_ - 1);
        if (p.x + r < min_.x || p.x - r > max_.x || p.y + r < min_.y || p.y - r > max_.y) break;
        Vec3 push{};
        std::vector<std::uint32_t> seen;
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (std::uint32_t i : cells_[std::size_t(x) * std::size_t(ny_) + std::size_t(y)]) {
                    if (std::find(seen.begin(), seen.end(), i) != seen.end()) continue;
                    seen.push_back(i);
                    const Tri& t = tris_[i];
                    const Vec3 q = closest_on_triangle(p, t.a, t.b, t.c);
                    const Vec3 d = p - q;
                    const float dist = std::sqrt(dot(d, d));
                    if (dist >= r || dist <= 1e-6f) continue;
                    const Vec3 dir = d * (1 / dist);
                    // Keep the largest push along each direction rather than summing coplanar faces.
                    if (dot(push, dir) < r - dist) push = push + dir * ((r - dist) - dot(push, dir));
                }
        if (dot(push, push) <= 1e-10f) break;
        total = total + push;
    }
    return total;
}

} // namespace parkan
