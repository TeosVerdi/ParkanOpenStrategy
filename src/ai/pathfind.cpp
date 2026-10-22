#include "ai/pathfind.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace parkan::ai {

ArealGraph ArealGraph::build(const LandMap& map) {
    ArealGraph g;
    g.nodes.resize(map.areals.size());
    for (std::size_t i = 0; i < map.areals.size(); ++i) {
        const LandAreal& a = map.areals[i];
        GraphNode& n = g.nodes[i];
        n.centre = a.anchor;
        if (!a.walkable) continue;
        for (std::size_t k = 0; k < a.verts.size() && k < a.links.size(); ++k) {
            const std::int32_t nb = a.links[k][0];
            if (nb < 0 || !map.areals[std::size_t(nb)].walkable) continue;
            if (std::ranges::any_of(n.edges, [&](const GraphEdge& e) { return e.to == std::uint32_t(nb); })) continue;
            n.edges.push_back({std::uint32_t(nb), 0xffff, dist2d(a.anchor, map.areals[std::size_t(nb)].anchor) + 1.0f});
        }
    }
    return g;
}

std::vector<std::uint32_t> search_path(const ArealGraph& graph, std::uint32_t start, std::uint32_t goal, bool can_fly,
                                       const RandomSource& rand, float randomness) {
    const std::size_t n = graph.nodes.size();
    if (start >= n || goal >= n) return {};
    if (start == goal) return {start};
    // The per-node search record (+0 stamp, +4 parent, +8 g, +0xc h); one search = one stamp.
    struct Rec {
        bool seen = false, open = false;
        std::uint32_t parent = UINT32_MAX;
        float g = 0, h = 0;
        std::uint64_t seq = 0;
    };
    std::vector<Rec> rec(n);
    const auto heuristic = [&](std::uint32_t a) {
        const Vec3 d = graph.nodes[a].centre - graph.nodes[goal].centre;
        return std::sqrt(dot(d, d)); // 3D
    };
    // The open tree keyed by f = g + h; equal keys keep insertion order (comparator not read: inferred).
    std::set<std::tuple<float, std::uint64_t, std::uint32_t>> open;
    std::uint64_t seq = 0;
    const auto push = [&](std::uint32_t k) {
        Rec& r = rec[k];
        if (r.open) open.erase({r.g + r.h, r.seq, k}); // re-keyed when a cheaper g is found (inferred)
        r.seq = seq++;
        r.open = true;
        open.insert({r.g + r.h, r.seq, k});
    };
    const auto form_path = [&] { // FormPath (10020be0): parents goal→start, at most 0x800, reversed
        std::vector<std::uint32_t> path;
        for (std::uint32_t k = goal; k != UINT32_MAX && path.size() < 0x800; k = rec[k].parent) path.push_back(k);
        std::ranges::reverse(path);
        return path;
    };

    rec[start] = {true, false, UINT32_MAX, 0, heuristic(start), 0};
    push(start);
    for (int expansions = 0; expansions < 0x800; ++expansions) {
        if (open.empty()) return {};
        const std::uint32_t cur = std::get<2>(*open.begin());
        open.erase(open.begin());
        rec[cur].open = false;
        for (const GraphEdge& e : graph.nodes[cur].edges) {
            if (e.flags & 0x20000) continue;
            if ((e.flags & 0x10000) && !can_fly) continue;
            if (e.to >= n) continue;
            const float g = (e.cost + rec[cur].g) * (1 + r01(rand) * randomness);
            Rec& m = rec[e.to];
            if (!m.seen || g < m.g) {
                m.seen = true;
                m.g = g;
                m.h = heuristic(e.to);
                m.parent = cur;
                push(e.to);
                if (e.to == goal) return form_path(); // the goal is generated, not popped
            }
        }
    }
    return {};
}

namespace {

// 10022060, reduced: the crossing of segments p0→p1 and a→b, both parameters within [0, 1].
std::optional<Vec3> segment_cross(Vec3 p0, Vec3 p1, Vec3 a, Vec3 b) {
    const float rx = p1.x - p0.x, ry = p1.y - p0.y, sx = b.x - a.x, sy = b.y - a.y;
    const float den = rx * sy - ry * sx;
    if (std::abs(den) < 1e-12f) return std::nullopt;
    const float qx = a.x - p0.x, qy = a.y - p0.y;
    const float t = (qx * sy - qy * sx) / den, u = (qx * ry - qy * rx) / den;
    if (t < 0 || t > 1 || u < 0 || u > 1) return std::nullopt;
    return Vec3{a.x + sx * u, a.y + sy * u, 0};
}

} // namespace

std::vector<CorridorPoint> build_corridor(const LandMap& map, std::span<const std::uint32_t> path, Vec3 from, Vec3 target) {
    std::vector<CorridorPoint> out;
    Vec3 prev{from.x, from.y, 0};
    const Vec3 goal{target.x, target.y, 0};
    for (std::size_t k = 0; k + 1 < path.size(); ++k) {
        const LandAreal &A = map.areals[path[k]], &B = map.areals[path[k + 1]];
        float best = INFINITY;
        Vec3 cross{}, normal{};
        for (std::size_t i = 0; i < A.verts.size() && i < A.links.size(); ++i) {
            if (A.links[i][0] != std::int32_t(path[k + 1])) continue;
            const Vec3 a{A.verts[i].x, A.verts[i].y, 0}, b{A.verts[(i + 1) % A.verts.size()].x, A.verts[(i + 1) % A.verts.size()].y, 0};
            const float len = dist2d(a, b);
            if (len < 1e-6f) continue;
            const Vec3 dir = (b - a) * (1 / len);
            Vec3 cand[2];
            int nc = 0;
            if (auto c = segment_cross(prev, goal, a, b)) {
                cand[nc++] = *c;
            } else {
                const float in = std::min(3.0f, len * 0.5f); // a shorter edge: its midpoint (port guard)
                cand[nc++] = a + dir * in;
                cand[nc++] = b - dir * in;
            }
            for (int c = 0; c < nc; ++c) {
                const float cost = dist2d(prev, cand[c]) + dist2d(cand[c], goal);
                if (cost < best) best = cost, cross = cand[c], normal = {dir.y, -dir.x, 0}; // CCW: outward = right
            }
        }
        if (best == INFINITY) continue; // no shared edge (cannot happen on symmetric links)
        // 100214a0: step along the normal until inside; the last try stands when none is (inferred).
        Vec3 in_a = cross, in_b = cross;
        for (int s = 1; s <= 5; ++s) {
            in_a = cross - normal * (0.5f * float(s));
            if (A.contains(in_a.x, in_a.y)) break;
        }
        for (int s = 1; s <= 9; ++s) {
            in_b = cross + normal * (0.2f * float(s));
            if (B.contains(in_b.x, in_b.y)) break;
        }
        out.push_back({in_a, path[k], false});
        out.push_back({in_b, path[k + 1], false});
        prev = in_b;
    }
    out.push_back({goal, path.empty() ? 0u : path.back(), false});
    return out;
}

} // namespace parkan::ai
