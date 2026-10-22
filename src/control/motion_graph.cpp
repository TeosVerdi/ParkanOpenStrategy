#include "control/motion_graph.hpp"

#include "scene/model.hpp"

#include <bit>
#include <cfloat>
#include <cmath>

namespace parkan {

namespace {

// x87 FSTP to a float: out-of-range values become ±inf (a plain cast would be undefined).
float to_f32(double v) {
    if (v > FLT_MAX) return INFINITY;
    if (v < -FLT_MAX) return -INFINITY;
    return float(v);
}

float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Vec3 rotate(const Mat4& m, Vec3 v) { return m.transform_point(v) - m.transform_point({}); }

// The part matrices AniMesh's UpdateNodeMatrices (10008b30) builds for one (frameA, frameB, weight):
// every node sampled and chained to its parent, root translation zeroed.
std::vector<Mat4> pose_parts(const Mesh& mesh, float frame_a, float frame_b, float weight) {
    std::vector<Mat4> world(mesh.nodes.size(), Mat4::identity());
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        NodePose p = sample_node_blend(mesh, i, frame_a, frame_b, weight);
        const bool root = mesh.nodes[i].parent == 0xFFFF;
        if (root) p.pos = {};
        // Native local matrices rotate by the conjugate (see scene/model.cpp pose_matrix).
        const Mat4 local = Mat4::translation(p.pos) * Mat4::rotation(p.rot.conjugate());
        world[i] = root ? local : world[mesh.nodes[i].parent] * local;
    }
    return world;
}

} // namespace

bool motion_state_matches(const CtldState& s, Vec3 v, Vec3 a, std::span<const ContactRecord> contacts,
                          std::int32_t mode) {
    const float vv[3] = {v.x, v.y, v.z}, aa[3] = {a.x, a.y, a.z};
    for (int i = 0; i < 3; ++i) {
        if ((s.velocity_mask & (1u << i)) && !(s.lin_min[i] <= vv[i] && vv[i] <= s.lin_max[i])) return false;
        if ((s.velocity_mask & (0x10u << i)) && !(s.ang_min[i] <= aa[i] && aa[i] <= s.ang_max[i])) return false;
    }
    for (std::size_t c = 0; c < s.contacts.size() && c < contacts.size(); ++c) {
        const std::uint32_t f = s.contacts[c].flags;
        if ((f & 0x100) && !contacts[c].touch) return false;
        if ((f & 0x200) && contacts[c].touch) return false;
    }
    return s.uses != 0 && (s.mode == mode || s.mode == -1);
}

MotionGraph::MotionGraph(const Ctld& ctld) {
    const std::size_t n = ctld.states.size();
    states.resize(n);
    for (std::size_t i = 0; i < n; ++i) static_cast<CtldState&>(states[i]) = ctld.states[i];
    contact_count = n ? ctld.states[0].contacts.size() : 0;
    cost_.resize(n * n);
    dist_.resize(n);
    next_.resize(n);
    visited_.resize(n);

    // BuildTransitionCosts: distance from the source's minimum corner to the destination's centre on
    // the source's masked axes; a file weight of 0 is a free edge, not a missing one.
    for (std::size_t i = 0; i < n; ++i) {
        const CtldState& si = ctld.states[i];
        double cl[3], ca[3];
        for (int k = 0; k < 3; ++k) {
            cl[k] = (double(si.lin_min[k]) + si.lin_max[k]) / 2;
            ca[k] = (double(si.ang_min[k]) + si.ang_max[k]) / 2;
        }
        for (std::size_t j = 0; j < n; ++j) {
            const CtldState& sj = ctld.states[j];
            double dl = 0, da = 0;
            for (int k = 0; k < 3; ++k) {
                if (sj.velocity_mask & (1u << k)) dl = std::max(dl, std::abs(cl[k] - sj.lin_min[k]));
                if (sj.velocity_mask & (0x10u << k)) da = std::max(da, std::abs(ca[k] - sj.ang_min[k]));
            }
            cost_[i * n + j] = to_f32((da + dl + 1) * ctld.weights[i * n + j]);
        }
    }
}

float MotionGraph::shortest_path(std::size_t src, std::size_t tgt) {
    const std::size_t n = size();
    if (tgt >= n) return kNoPath;
    for (std::size_t u = 0; u < n; ++u) {
        dist_[u] = cost(tgt, u);
        next_[u] = std::uint32_t(tgt);
        visited_[u] = 0;
    }
    // Ascending scans with strict comparisons: ties go to the lowest index, and a relaxed node keeps
    // its first predecessor.
    float best = kNoPath;
    std::size_t u = 0;
    for (std::size_t k = 0; k < n; ++k)
        if (dist_[k] < best) best = dist_[k], u = k;
    if (best == kNoPath) return kNoPath;
    visited_[u] = 1;
    if (u == src) return dist_[u];
    for (;;) {
        best = kNoPath;
        std::size_t w = u;
        for (std::size_t v = 0; v < n; ++v) {
            if (visited_[v]) continue;
            const double alt = double(dist_[u]) + cost(u, v); // edge v -> u, summed at x87 precision
            if (alt < dist_[v]) dist_[v] = to_f32(alt), next_[v] = std::uint32_t(u);
            if (dist_[v] < best) best = dist_[v], w = v;
        }
        if (best == kNoPath) return kNoPath;
        visited_[w] = 1;
        u = w;
        if (u == src) return dist_[src];
    }
}

Vec3 root_motion(const Mesh& mesh, std::array<float, 2> a, std::array<float, 2> b, float time, float weight) {
    if (mesh.nodes.empty()) return {};
    const float fa = time * a[1] + (1 - time) * a[0], fb = time * b[1] + (1 - time) * b[0];
    return sample_node_blend(mesh, 0, fa, fb, weight).pos;
}

std::vector<ContactRecord> precompute_motion_states(MotionGraph& graph, const Mesh& mesh,
                                                    std::span<const ContactPoint> points) {
    const std::size_t k_count = graph.contact_count;
    std::vector<ContactRecord> contacts(k_count);
    // GetContactPoint, space 0: the CTPT point through its part's matrix (object scale taken as 1).
    const auto contact_point = [&](const std::vector<Mat4>& parts, std::uint32_t point, Vec3& pos, Vec3& vec) {
        if (point >= points.size()) return false;
        const ContactPoint& p = points[point];
        const Mat4& m = p.part < parts.size() ? parts[p.part] : Mat4::identity();
        pos = m.transform_point(p.pos);
        vec = rotate(m, p.vec);
        return true;
    };

    // Runtime contact records from state 0's constraints, at range {0,0}{0,0}, time 0, weight 0.
    const Vec3 r0 = root_motion(mesh, {0, 0}, {0, 0}, 0, 0);
    if (!graph.states.empty() && !points.empty()) {
        const std::vector<Mat4> parts = pose_parts(mesh, 0, 0, 0);
        for (std::size_t c = 0; c < k_count; ++c) {
            contacts[c].point = graph.states[0].contacts[c].point;
            Vec3 pos, vec;
            if (contact_point(parts, contacts[c].point, pos, vec)) contacts[c].rest = pos + r0;
        }
    }

    for (MotionState& s : graph.states) {
        const Vec3 p0 = root_motion(mesh, s.track_a, s.track_b, 0, 0), p1 = root_motion(mesh, s.track_a, s.track_b, 1, 0);
        s.disp_a = p1 - p0, s.len_a = length(s.disp_a), s.start_a = p0;
        const Vec3 q0 = root_motion(mesh, s.track_a, s.track_b, 0, 1), q1 = root_motion(mesh, s.track_a, s.track_b, 1, 1);
        s.disp_b = q1 - q0, s.len_b = length(s.disp_b), s.start_b = q0;
        if (points.empty()) continue;

        // The mesh is now posed at the end of track B (time 1, weight 1).
        const std::vector<Mat4> parts = pose_parts(mesh, s.track_a[1], s.track_b[1], 1);
        for (std::size_t c = 0; c < k_count && c < s.contacts.size(); ++c) {
            CtldContact& k = s.contacts[c];
            Vec3 pos, vec;
            if (!contact_point(parts, k.point, pos, vec)) continue;
            const bool down = std::abs(pos.z + q1.z - contacts[c].rest.z) < 0.1f;
            std::uint32_t f = k.flags;
            if (f & 0x10) f = down ? (f | 1) : (f & ~1u); // support point := foot down at the end
            if (down) f |= 0x1000;                        // never cleared
            if (f & 0x20) f = (vec.z > 0 && 1 - vec.z < 0.05f) ? (f | 2) : (f & ~2u); // IK: vector nearly vertical
            // +0xc becomes a float: the foot's height offset at the end of the step. Mesh part flag
            // 0x10 (IAniMesh slot 0x20) for flag 2 is not modelled.
            if (f & 1) k.raw_c = std::bit_cast<std::uint32_t>(contacts[c].rest.z - pos.z - q1.z);
            k.flags = f;
        }
    }
    return contacts;
}

std::vector<ContactPoint> decode_ctpt(std::span<const std::uint8_t> d) {
    Reader r(d);
    const std::uint32_t n = r.u32();
    if (std::size_t(n) * 36 > r.remaining()) throw FormatError("CTPT count exceeds the entry");
    std::vector<ContactPoint> out(n);
    for (auto& p : out) {
        r.u32(); // flags
        p.part = r.u32();
        r.u32(); // damage part
        p.pos = {r.f32(), r.f32(), r.f32()};
        p.vec = {r.f32(), r.f32(), r.f32()};
    }
    return out;
}

} // namespace parkan
