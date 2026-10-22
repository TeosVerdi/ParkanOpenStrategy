// A unit that moves: its posable model, the motion runtime (Control.dll's motion graph + mover),
// the turret rig, and the per-frame corrections Control applies after each update — a reduced
// FollowGround (notes/control-physics.md "Per frame") and the collision push.
#pragma once

#include "control/turret.hpp"
#include "control/unit_motion.hpp"
#include "mission/tma.hpp"
#include "scene/mission_scene.hpp"
#include "world/collision.hpp"
#include "world/ground.hpp"

#include <memory>

namespace parkan {

class LiveUnit {
public:
    // A unit design placed by the TMA. nullptr when its chassis has no motion graph (it cannot move).
    static std::unique_ptr<LiveUnit> load(AssetLibrary& lib, const mission::TmaObject& o, const TerrainGround& ground);

    const mission::TmaObject* placement = nullptr;
    UnitModel model;
    std::unique_ptr<UnitMotion> motion; // refers to model's chassis mesh: LiveUnit never moves in memory
    std::vector<ContactPoint> points;   // chassis CTPT
    // The turret (first component with a turret device) and its marker points.
    std::size_t turret_part = 0;
    std::optional<TurretRig> rig;
    std::vector<ContactPoint> turret_points;
    // Collision sphere around the chassis' static-pose bounds (inferred; CCollObject +0x50 untraced).
    Vec3 sphere_centre{};
    float sphere_radius = 1;
    float mass = 1; // CCollObject +0x34 = control property 0x7c (source untraced): equal for now
    std::uint32_t first_transform = 0; // where its parts start in the scene's transforms
    MotionFrame frame;                  // the last interpolated frame

    // After motion->update(now): ground following from the feet, then out of static walls and
    // inside the landscape box. Returns the collision push applied.
    Vec3 settle(float now_ms, const TerrainGround& ground, const StaticColliders& statics);
    // Collision sphere centre in the world (after settle).
    Vec3 sphere_world() const { return frame.matrix.transform_point(sphere_centre); }
    Vec3 position() const { return frame.matrix.transform_point({}); }
    // Pose the parts into `transforms` (turret target 0..1 per axis; hidden = collapsed, e.g. the own
    // unit seen from its TURRET camera).
    void pose(std::vector<Mat4>& transforms, float turret_x, float turret_y, bool hidden);

private:
    LiveUnit() = default;
};

// Unit-vs-unit collision (CCollManager broad phase + PushApart) for the tick: overlapping spheres are
// separated along the line of centres, the push split by squared mass; ground units are pushed
// horizontally.
void collide_units(std::vector<LiveUnit*>& units, float now_ms);

} // namespace parkan
