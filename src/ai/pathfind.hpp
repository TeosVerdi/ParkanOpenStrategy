// Global path planning over the areal graph: SystemArealMap::BuildArealEdges (ArealMap 10023240),
// MGraph::Search (Behavior 10020910) + RunBestFirst (10020760) with MWorldGraph::ExpandNode (10042c10)
// and Heuristic (100431c0), and the areal→areal corridor of MWalker::PlanGlobalPath (10036730).
// Spec: notes/unit-ai-runtime.md §2.4, §2.5.
//
// Only whole-areal nodes (code type 1) exist here: building hallways (types 3/6/7), subdivided areals
// (type 2) and obstacle codes (8–10) are not modelled, so flyer-only / closed edge flags never occur
// on map data (they are honoured for synthetic graphs).
#pragma once

#include "core/math.hpp"
#include "res/landmap.hpp"

#include <functional>
#include <span>
#include <optional>
#include <vector>

namespace parkan::ai {

// rand() as the AI modules call it (MSVC: 0..32767). Injected so tests and replays are deterministic.
using RandomSource = std::function<int()>;

// The MSVC CRT generator: seed·214013 + 2531011, bits 16..30.
struct MsvcRand {
    std::uint32_t seed = 1;
    int operator()() {
        seed = seed * 214013u + 2531011u;
        return int((seed >> 16) & 0x7fff);
    }
};

// r01 = rand()·3.051851e-05 (the constant the Behaviour code multiplies by, ≈ 1/32767).
inline float r01(const RandomSource& rand) { return float(rand()) * 3.051851e-05f; }

inline float dist2d(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct GraphEdge {
    std::uint32_t to = 0;
    std::uint32_t flags = 0xffff; // 0x20000 closed, 0x10000 flyers only; terrain edges carry 0xffff
    float cost = 0;
};
struct GraphNode {
    Vec3 centre{}; // the areal anchor (IArealMap slot 50 GetNodeCenter)
    std::vector<GraphEdge> edges;
};
struct ArealGraph {
    std::vector<GraphNode> nodes; // index = areal id

    // BuildArealEdges: a walkable areal gets one edge per walkable neighbour (the first polygon edge
    // linking to it), cost = dist2D(anchor, anchor_nb) + 1.0.
    static ArealGraph build(const LandMap& map);
};

// MGraph::Search: randomised best-first A* from `start` to `goal` (areal ids). Every relaxation
// perturbs the whole accumulated g by (1 + r01·randomness); the search stops when the goal is first
// generated, after at most 0x800 expansions. Returns start..goal, or empty on failure.
std::vector<std::uint32_t> search_path(const ArealGraph& graph, std::uint32_t start, std::uint32_t goal, bool can_fly,
                                       const RandomSource& rand, float randomness = 0.7f);

// A corridor point (0x24-byte record, MWalker +0xe8): z = 0 until EmitTrajectoryPoints sets it.
struct CorridorPoint {
    Vec3 pos{};
    std::uint32_t areal = 0; // node code id (type 1)
    bool stop = false;       // +0x18 stop-here
};

// PlanGlobalPath's areal→areal portals for `path` (from search_path), then the target. Per pair A→B:
// the crossing of the line prev→target with the shared edge (or an edge end moved 3 m inwards),
// emitted pushed back into A (0.5 … < 3 m) and then into B (0.2 … < 2 m) along the edge normal.
// The boundary walk when the line leaves A elsewhere (§2.5 step 3) is not modelled.
std::vector<CorridorPoint> build_corridor(const LandMap& map, std::span<const std::uint32_t> path, Vec3 from, Vec3 target);

} // namespace parkan::ai
