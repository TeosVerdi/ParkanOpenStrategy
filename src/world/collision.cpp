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

void add_to_grid(std::vector<std::vector<std::uint32_t>>& cells, Vec3 min, float cell, int nx, int ny, std::uint32_t i, Vec3 a,
                 Vec3 b, Vec3 c) {
    const int x0 = std::clamp(int((std::min({a.x, b.x, c.x}) - min.x) / cell), 0, nx - 1);
    const int x1 = std::clamp(int((std::max({a.x, b.x, c.x}) - min.x) / cell), 0, nx - 1);
    const int y0 = std::clamp(int((std::min({a.y, b.y, c.y}) - min.y) / cell), 0, ny - 1);
    const int y1 = std::clamp(int((std::max({a.y, b.y, c.y}) - min.y) / cell), 0, ny - 1);
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y) cells[std::size_t(x) * std::size_t(ny) + std::size_t(y)].push_back(i);
}

} // namespace

std::optional<float> segment_triangle(Vec3 a, Vec3 b, Vec3 p0, Vec3 p1, Vec3 p2) {
    const Vec3 d = b - a, e1 = p1 - p0, e2 = p2 - p0;
    const Vec3 h = cross(d, e2);
    const float det = dot(e1, h);
    if (std::abs(det) < 1e-12f) return std::nullopt;
    const float inv = 1 / det;
    const Vec3 s = a - p0;
    const float u = dot(s, h) * inv;
    if (u < 0 || u > 1) return std::nullopt;
    const Vec3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return std::nullopt;
    const float t = dot(e2, q) * inv;
    if (t < 0 || t > 1) return std::nullopt;
    return t;
}

void walk_grid(Vec3 lo, float cell, int nx, int ny, Vec3 a, Vec3 b, const std::function<bool(std::size_t, float)>& visit) {
    if (nx <= 0 || ny <= 0 || cell <= 0) return;
    const float hx = lo.x + float(nx) * cell, hy = lo.y + float(ny) * cell;
    const float dx = b.x - a.x, dy = b.y - a.y;
    // Clip the segment's xy projection to the grid rectangle (slab test).
    float t0 = 0, t1 = 1;
    auto slab = [&](float p, float d, float mn, float mx) {
        if (std::abs(d) < 1e-12f) return p >= mn && p <= mx;
        float ta = (mn - p) / d, tb = (mx - p) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta), t1 = std::min(t1, tb);
        return t0 <= t1;
    };
    if (!slab(a.x, dx, lo.x, hx) || !slab(a.y, dy, lo.y, hy)) return;
    const float sx = a.x + dx * t0, sy = a.y + dy * t0;
    int x = std::clamp(int((sx - lo.x) / cell), 0, nx - 1), y = std::clamp(int((sy - lo.y) / cell), 0, ny - 1);
    const int step_x = dx > 0 ? 1 : -1, step_y = dy > 0 ? 1 : -1;
    const float inf = 1e30f;
    const float delta_x = std::abs(dx) > 1e-12f ? cell / std::abs(dx) : inf;
    const float delta_y = std::abs(dy) > 1e-12f ? cell / std::abs(dy) : inf;
    float next_x = std::abs(dx) > 1e-12f ? (lo.x + float(x + (dx > 0)) * cell - a.x) / dx : inf;
    float next_y = std::abs(dy) > 1e-12f ? (lo.y + float(y + (dy > 0)) * cell - a.y) / dy : inf;
    for (;;) {
        const float exit = std::min({next_x, next_y, t1});
        if (!visit(std::size_t(x) * std::size_t(ny) + std::size_t(y), exit) || exit >= t1) return;
        if (next_x < next_y) {
            x += step_x, next_x += delta_x;
            if (x < 0 || x >= nx) return;
        } else {
            y += step_y, next_y += delta_y;
            if (y < 0 || y >= ny) return;
        }
    }
}

void TraceMesh::add(const RenderMesh& mesh, std::uint32_t first, std::uint32_t last) {
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
            if (dot(n, n) <= 1e-18f) continue;
            tris_.push_back({v[0], v[1], v[2], d.transform});
        }
    }
}

void TraceMesh::finish() {
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
    for (std::uint32_t i = 0; i < tris_.size(); ++i) add_to_grid(cells_, min_, cell_, nx_, ny_, i, tris_[i].a, tris_[i].b, tris_[i].c);
}

std::optional<TraceHit> TraceMesh::trace(Vec3 a, Vec3 b) const {
    if (cells_.empty()) return std::nullopt;
    if (std::max(a.z, b.z) < min_.z || std::min(a.z, b.z) > max_.z) return std::nullopt;
    std::optional<TraceHit> best;
    walk_grid(min_, cell_, nx_, ny_, a, b, [&](std::size_t c, float exit) {
        for (std::uint32_t i : cells_[c]) {
            const Tri& t = tris_[i];
            const auto hit = segment_triangle(a, b, t.a, t.b, t.c);
            if (!hit || (best && *hit >= best->t)) continue;
            const Vec3 face = normalize(cross(t.b - t.a, t.c - t.a));
            const Vec3 n = dot(face, b - a) > 0 ? face * -1.f : face;
            best = TraceHit{*hit, a + (b - a) * *hit, n, i, t.transform, face};
        }
        return !(best && best->t <= exit); // a hit inside this cell is the nearest
    });
    return best;
}

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

void StaticColliders::add(const RenderMesh& mesh, std::uint32_t first, std::uint32_t last, std::uint32_t tag) {
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
            tris_.push_back({v[0], v[1], v[2], tag});
        }
    }
}

void StaticColliders::remove(std::uint32_t tag) {
    std::erase_if(tris_, [tag](const Tri& t) { return t.tag == tag; });
    finish();
}

void StaticColliders::finish() {
    cells_.clear();
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
