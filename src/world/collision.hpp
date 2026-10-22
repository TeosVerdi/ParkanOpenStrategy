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
#include <functional>
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

// A segment's hit on a triangle soup: segment parameter t in [0, 1] (a + t·(b − a)), the point, the
// face normal (unit, facing the segment's start), the triangle and its source transform.
struct TraceHit {
    float t = 1;
    Vec3 point{}, normal{0, 0, 1};
    std::uint32_t triangle = 0, transform = 0;
};

// Möller–Trumbore, two-sided: t in [0, 1] of the segment a → b, or nullopt.
std::optional<float> segment_triangle(Vec3 a, Vec3 b, Vec3 p0, Vec3 p1, Vec3 p2);

// Visits, in order along a → b, the cells of a uniform xy grid (origin `grid_min`, square cells of
// `cell`, nx × ny, index x·ny + y) the segment crosses; `visit(index, t_exit)` returns false to stop.
void walk_grid(Vec3 grid_min, float cell, int nx, int ny, Vec3 a, Vec3 b,
               const std::function<bool(std::size_t, float)>& visit);

// Every triangle of the static scene, for segment traces (bullets, lines of fire): a stand-in for
// Terrain's IWorld::TraceRay (flags 0x41e) over the object meshes. Floors and roofs count, unlike
// StaticColliders' walls.
class TraceMesh {
public:
    void add(const RenderMesh& mesh, std::uint32_t first_transform, std::uint32_t last_transform);
    void finish();
    // The nearest triangle hit along a → b.
    std::optional<TraceHit> trace(Vec3 a, Vec3 b) const;
    std::size_t triangle_count() const { return tris_.size(); }

private:
    struct Tri {
        Vec3 a, b, c;
        std::uint32_t transform;
    };
    std::vector<Tri> tris_;
    std::vector<std::vector<std::uint32_t>> cells_;
    Vec3 min_{}, max_{};
    int nx_ = 0, ny_ = 0;
    float cell_ = 8.f;
};

class StaticColliders {
public:
    // Adds the wall triangles of `mesh`'s draws whose transform index is in [first, last), tagged `tag`.
    void add(const RenderMesh& mesh, std::uint32_t first_transform, std::uint32_t last_transform, std::uint32_t tag = 0);
    void finish(); // builds the grid; call after the last add()
    // Drops the triangles tagged `tag` (a removed building) and rebuilds the grid.
    void remove(std::uint32_t tag);

    // Push that moves a sphere out of every wall it penetrates (two relaxation passes).
    Vec3 push_sphere(Vec3 c, float r) const;
    std::size_t triangle_count() const { return tris_.size(); }

private:
    struct Tri {
        Vec3 a, b, c;
        std::uint32_t tag = 0;
    };
    std::vector<Tri> tris_;
    std::vector<std::vector<std::uint32_t>> cells_;
    Vec3 min_{}, max_{};
    int nx_ = 0, ny_ = 0;
    float cell_ = 4.f;
};

} // namespace parkan
