// Ground probes on the landscape: the face under a point, its height and normal. Stands in for
// Terrain's IWorld::FindWorldFace / FindNearestFace10 + Control's ProjectPointOntoPlaneZ
// (1001bfc0) as used by CControlBase::FollowGround (1001a450) and DropToGround (10018800);
// notes/control-physics.md "Per frame". Landscape faces only; buildings and bridges come later.
#pragma once

#include "core/math.hpp"
#include "res/terrain.hpp"
#include "world/collision.hpp"

#include <optional>

namespace parkan {

struct GroundHit {
    float z = 0;           // ground height at the probe's x, y
    Vec3 normal{0, 0, 1};  // face plane normal (unit, z > 0)
    std::uint32_t face = 0;
    std::uint32_t flags = 0; // face flags
};

class TerrainGround {
public:
    // Uses the LOD0 faces (each node's slot 0 through the type-11 draw list), without water-surface
    // faces (flag 0x20000).
    explicit TerrainGround(const Terrain& t);

    // The face under (p.x, p.y). Of several stacked faces, the highest one not more than
    // `max_rise` above p.z wins (the original accepts a rise below the step limit, ≤ 7.5 m);
    // nullopt off the map or when every face there is higher.
    std::optional<GroundHit> probe(Vec3 p, float max_rise = 7.5f) const;
    // CLandscape::GetWalkFace along Z (10014b20) with mode bit 4 (`above`: faces at or above p) or bit 8
    // (faces at or below p): of the faces under (p.x, p.y), the one nearest to p.z on that side.
    std::optional<GroundHit> probe_vertical(Vec3 p, bool above) const;

    // The first landscape face along the segment a → b (bullets: TraceBulletVsWorld 1001d9d0 on the
    // landscape); TraceHit::triangle is the terrain face index. Water surfaces are not hit (as in probe).
    std::optional<TraceHit> trace(Vec3 a, Vec3 b) const;

    Vec3 bounds_min() const { return min_; }
    Vec3 bounds_max() const { return max_; }

private:
    struct Tri {
        Vec3 a, b, c;
        Vec3 n;
        std::uint32_t face, flags;
    };
    std::vector<Tri> tris_;
    std::vector<std::vector<std::uint32_t>> cells_; // grid of triangle indices
    int nx_ = 0, ny_ = 0;
    float cell_ = 1;
    Vec3 min_{}, max_{};
};

} // namespace parkan
