// Collision as positional separation (Control.dll CCollManager iid 0x203 / CCollObject iid 0x28;
// notes/control-physics.md "Collision"): no velocities change and no damage is dealt; units are
// pushed out of each other and out of other objects, and kept inside the landscape box.
//
// Port choices (inferred where the original's narrow phase is not decoded):
// - static objects (buildings, trees, rocks) collide through their triangles; only near-vertical
//   faces (|n.z| < 0.7) push, so floors and foundations do not lift a unit;
// - a unit is a sphere around its body.
#pragma once

#include "core/math.hpp"
#include "scene/render_mesh.hpp"

#include <array>
#include <optional>

namespace parkan {

// SweptSphereTOI (1001e9f0): two spheres moving linearly over the tick; returns the time of first
// contact in [0, 1] (0 when they already overlap at the start), or nullopt.
std::optional<float> swept_sphere_toi(Vec3 a0, Vec3 a1, float ra, Vec3 b0, Vec3 b1, float rb);

// PushApart (1001daf0) split: B moves by p·mA²/(mA²+mB²), A by −p·mB²/(mA²+mB²).
std::pair<Vec3, Vec3> split_push(Vec3 p, float mass_a, float mass_b);

// ContainInWorldBounds (1001e650) for a unit sphere in the landscape box: a hard push back inside
// the box shrunk by the radius (ceiling = max z + 20), then a soft push of 3·0.0125·depth inside an
// 80 m border (xy) and 3·0.05·depth under the 20 m ceiling band.
Vec3 contain_in_world_bounds(Vec3 c, float r, Vec3 box_min, Vec3 box_max);

// The ApplyCollisionPush (1000c990) rule for ground units (state flag 4): the push becomes
// horizontal, its horizontal part scaled by |p|/|p_xy| up to 4×.
Vec3 horizontal_push(Vec3 p);

class StaticColliders {
public:
    // Adds the wall triangles of `mesh`'s draws whose transform index is in [first, last).
    void add(const RenderMesh& mesh, std::uint32_t first_transform, std::uint32_t last_transform);
    void finish(); // builds the grid; call after the last add()

    // Push that moves a sphere out of every wall it penetrates (two relaxation passes).
    Vec3 push_sphere(Vec3 c, float r) const;
    std::size_t triangle_count() const { return tris_.size(); }

private:
    struct Tri {
        Vec3 a, b, c;
    };
    std::vector<Tri> tris_;
    std::vector<std::vector<std::uint32_t>> cells_;
    Vec3 min_{}, max_{};
    int nx_ = 0, ny_ = 0;
    float cell_ = 4.f;
};

} // namespace parkan
