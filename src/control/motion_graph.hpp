// Control.dll motion graph: the CTLD states with their bind-time root motion, the transition costs
// and the path search. MotionGraph::SetState / BuildTransitionCosts / ShortestPath (10001730 /
// 10001790 / 100019d0), MotionState_Matches (10001000), PrecomputeMotionStates (10019df0).
// Spec: notes/motion-runtime.md §2, §3, §5.
#pragma once

#include "core/math.hpp"
#include "res/ctld.hpp"
#include "res/mesh.hpp"

#include <span>

namespace parkan {

// Runtime copy of a CTLD state (0xa0 bytes at ctl+0x39c). PrecomputeMotionStates overwrites the
// file's +0x58..+0x8b with the root motion of each track and rewrites the contact flags.
struct MotionState : CtldState {
    Vec3 disp_a{}, start_a{}, disp_b{}, start_b{}; // +0x58 / +0x68, +0x74 / +0x84
    float len_a = 0, len_b = 0;                    // +0x64, +0x80
};

// A CTPT marker point (notes/formats-wea-cpt-ndp.md) as GetContactPoint (1001b4f0) uses it.
struct ContactPoint {
    std::uint32_t part = 0; // mesh node whose matrix places the point
    Vec3 pos{}, vec{};      // in that node's frame
};

// CTPT marker points (notes/formats-wea-cpt-ndp.md "CTPT"): u32 n, n × 36-byte records (flags, part,
// damage part, pos[3], vec[3]), then the names (not read here).
std::vector<ContactPoint> decode_ctpt(std::span<const std::uint8_t> data);

// The runtime contact record (ctl+0xc4, 0x5c bytes), one per constraint of state 0; only the fields
// the motion code reads.
struct ContactRecord {
    std::uint32_t point = 0;
    Vec3 rest{};       // +0x0c: the point at frame 0 plus the frame-0 root
    bool touch = true; // +0x59: "this foot touches the ground", written by FollowGround
};

// MotionState_Matches: inclusive range tests on the masked axes, the contact constraints, uses left
// and the mode (-1 = any). Flag bit0 is the callers' business.
bool motion_state_matches(const CtldState& s, Vec3 v, Vec3 a, std::span<const ContactRecord> contacts,
                          std::int32_t mode);

class MotionGraph {
public:
    static constexpr float kNoPath = 1e6f;

    MotionGraph() = default;
    // SetState for every file state, then BuildTransitionCosts with the file weights.
    explicit MotionGraph(const Ctld& ctld);

    std::size_t size() const { return states.size(); }
    // C[to][from]: row = destination, column = source (as the file weights).
    float cost(std::size_t to, std::size_t from) const { return cost_[to * size() + from]; }
    // Backward Dijkstra from `tgt`; the target is not pre-settled, so (x, x) is the cheapest cycle
    // through x. Returns kNoPath when unreachable and leaves next() filled: the path is
    // src, next(src), next(next(src)), ..., tgt.
    float shortest_path(std::size_t src, std::size_t tgt);
    std::size_t next(std::size_t u) const { return next_[u]; }

    std::vector<MotionState> states; // uses-left is decremented here at run time, never in the live copy
    std::size_t contact_count = 0;   // K constraints per state

private:
    std::vector<float> cost_, dist_;
    std::vector<std::uint32_t> next_;
    std::vector<std::uint8_t> visited_;
};

// IAniMesh::GetRootMotion (AniMesh 10005df0): node 0's sampled translation for track ranges `a`/`b`
// at `time` (0..1 across the step) and track-B weight `weight` (mesh scale taken as 1).
Vec3 root_motion(const Mesh& mesh, std::array<float, 2> a, std::array<float, 2> b, float time, float weight);

// PrecomputeMotionStates: fills each state's track displacements / lengths / start positions from
// the mesh's node-0 keys, builds the runtime contact records from state 0's constraints, and derives
// contact flags 1 / 2 / 0x1000 from the pose at the end of track B. Without CTPT `points` the
// contact part is skipped (rest = 0, file flags kept).
std::vector<ContactRecord> precompute_motion_states(MotionGraph& graph, const Mesh& mesh,
                                                    std::span<const ContactPoint> points = {});

} // namespace parkan
