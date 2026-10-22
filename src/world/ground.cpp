#include "world/ground.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>

namespace parkan {

TerrainGround::TerrainGround(const Terrain& t) {
    std::set<std::uint32_t> lod0;
    for (const auto& n : t.nodes) {
        if (n.slots[0] == 0xFFFF || n.slots[0] >= t.slots.size()) continue;
        const MeshSlot& s = t.slots[n.slots[0]];
        for (std::size_t e = s.tri_start; e < std::size_t(s.tri_start) + s.tri_count; ++e) lod0.insert(t.draw_list[e].face);
    }
    min_ = {FLT_MAX, FLT_MAX, FLT_MAX};
    max_ = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    auto pos = [&](std::uint16_t i) { return Vec3{t.positions[i][0], t.positions[i][1], t.positions[i][2]}; };
    for (std::uint32_t f : lod0) {
        const auto& face = t.faces[f];
        if (face.flags & 0x20000) continue; // water surface (z = water level); units walk the bottom (inferred)
        Tri tri{pos(face.v[0]), pos(face.v[1]), pos(face.v[2]), {}, f, face.flags};
        Vec3 n = cross(tri.b - tri.a, tri.c - tri.a);
        if (n.z < 0) n = n * -1.f;
        if (n.z <= 1e-6f) continue; // vertical: never under a point
        tri.n = normalize(n);
        for (const Vec3& p : {tri.a, tri.b, tri.c}) {
            min_ = {std::min(min_.x, p.x), std::min(min_.y, p.y), std::min(min_.z, p.z)};
            max_ = {std::max(max_.x, p.x), std::max(max_.y, p.y), std::max(max_.z, p.z)};
        }
        tris_.push_back(tri);
    }
    if (tris_.empty()) return;
    // Grid of about 4 triangles per cell.
    const float area = std::max(1.f, (max_.x - min_.x) * (max_.y - min_.y));
    cell_ = std::max(1.f, std::sqrt(area * 4.f / float(tris_.size())));
    nx_ = std::max(1, int((max_.x - min_.x) / cell_) + 1);
    ny_ = std::max(1, int((max_.y - min_.y) / cell_) + 1);
    cells_.resize(std::size_t(nx_) * std::size_t(ny_));
    for (std::uint32_t i = 0; i < tris_.size(); ++i) {
        const Tri& tr = tris_[i];
        const float x0 = std::min({tr.a.x, tr.b.x, tr.c.x}), x1 = std::max({tr.a.x, tr.b.x, tr.c.x});
        const float y0 = std::min({tr.a.y, tr.b.y, tr.c.y}), y1 = std::max({tr.a.y, tr.b.y, tr.c.y});
        const int cx0 = std::clamp(int((x0 - min_.x) / cell_), 0, nx_ - 1), cx1 = std::clamp(int((x1 - min_.x) / cell_), 0, nx_ - 1);
        const int cy0 = std::clamp(int((y0 - min_.y) / cell_), 0, ny_ - 1), cy1 = std::clamp(int((y1 - min_.y) / cell_), 0, ny_ - 1);
        for (int cx = cx0; cx <= cx1; ++cx)
            for (int cy = cy0; cy <= cy1; ++cy) cells_[std::size_t(cx) * std::size_t(ny_) + std::size_t(cy)].push_back(i);
    }
}

std::optional<GroundHit> TerrainGround::probe(Vec3 p, float max_rise) const {
    if (tris_.empty() || p.x < min_.x || p.y < min_.y || p.x > max_.x || p.y > max_.y) return std::nullopt;
    const int cx = std::clamp(int((p.x - min_.x) / cell_), 0, nx_ - 1), cy = std::clamp(int((p.y - min_.y) / cell_), 0, ny_ - 1);
    std::optional<GroundHit> best;
    for (std::uint32_t i : cells_[std::size_t(cx) * std::size_t(ny_) + std::size_t(cy)]) {
        const Tri& t = tris_[i];
        // Point in the triangle's XY projection (barycentric signs, edges inclusive).
        auto edge = [&](Vec3 a, Vec3 b) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
        const float e0 = edge(t.a, t.b), e1 = edge(t.b, t.c), e2 = edge(t.c, t.a);
        const bool inside = (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
        if (!inside) continue;
        // ProjectPointOntoPlaneZ: the plane's z at (x, y).
        const float z = t.a.z - (t.n.x * (p.x - t.a.x) + t.n.y * (p.y - t.a.y)) / t.n.z;
        if (z > p.z + max_rise) continue;
        if (!best || z > best->z) best = GroundHit{z, t.n, t.face, t.flags};
    }
    return best;
}

std::optional<TraceHit> TerrainGround::trace(Vec3 a, Vec3 b) const {
    if (cells_.empty()) return std::nullopt;
    std::optional<TraceHit> best;
    walk_grid(min_, cell_, nx_, ny_, a, b, [&](std::size_t c, float exit) {
        for (std::uint32_t i : cells_[c]) {
            const Tri& t = tris_[i];
            const auto hit = segment_triangle(a, b, t.a, t.b, t.c);
            if (!hit || (best && *hit >= best->t)) continue;
            best = TraceHit{*hit, a + (b - a) * *hit, t.n, t.face, 0};
        }
        return !(best && best->t <= exit);
    });
    return best;
}

} // namespace parkan
