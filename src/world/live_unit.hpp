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

#include <functional>
#include <memory>

namespace parkan {

class LiveUnit {
public:
    // A unit design placed by the TMA. nullptr when its chassis has no motion graph (it cannot move).
    // `scheme` (optional) is the design itself when it is not a game file (a design from the HQ
    // constructor); `start_ms` the motion clock's start (a unit created while the mission runs).
    static std::unique_ptr<LiveUnit> load(AssetLibrary& lib, const mission::TmaObject& o, const TerrainGround& ground,
                                          const mission::Scheme* scheme = nullptr, float start_ms = 0);
    // A building (data.tma kind 0: a UNITS\BUILDS scheme whose root is an objects.rlb FORT record): the
    // body of Terrain's CBuilding (ctor 100324d0). The FORT names the agent prototype (a BTLU record,
    // AniMesh LoadAgent) whose CTLD / CTPT / NDPR are the building's (notes/buildings.md §1). It has no
    // motion (`motion` is null) and stands at its TMA placement; doors move through `device_frames`.
    // nullptr when the scheme or its prototypes cannot be read.
    static std::unique_ptr<LiveUnit> load_building(AssetLibrary& lib, const mission::TmaObject& o,
                                                   const mission::Scheme* scheme = nullptr);

    const mission::TmaObject* placement = nullptr;
    UnitModel model;
    std::unique_ptr<UnitMotion> motion; // refers to model's chassis mesh: LiveUnit never moves in memory;
                                        // null for a building
    bool is_static() const { return !motion; }
    std::vector<ContactPoint> points;   // chassis CTPT
    // The turret (first component with a turret device) and its marker points.
    std::size_t turret_part = 0;
    std::optional<TurretRig> rig;
    std::vector<ContactPoint> turret_points;
    // Components under the turret with gun joints (flag 8) that follow its pitch: {part, CTLD joints}.
    std::vector<std::pair<std::size_t, std::vector<CtldJoint>>> gun_joints;
    // Collision sphere around the chassis' static-pose bounds (inferred; CCollObject +0x50 untraced).
    Vec3 sphere_centre{};
    float sphere_radius = 1;
    float mass = 1; // CCollObject +0x34 = control property 0x7c (source untraced): equal for now
    std::uint32_t first_transform = 0; // where its parts start in the scene's transforms
    MotionFrame frame;                  // the last interpolated frame
    Ctld ctld;                          // the chassis' motion graph (event lists for the touch-downs)

    // Feet that touched down during the last settle(), with the event list their constraint fires
    // (CTLD contact +8, flag 0x1000; FollowGround fires it once per landing).
    struct Touchdown {
        std::int32_t event_list = -1;
        Vec3 position{};
    };
    std::vector<Touchdown> touchdowns;
    // The effect a touch-down list starts: its START_EFFECT (type 10) event, the default surface one
    // (flag 0x10000000) when present, names an effect handle created by situation 0's list (type 3,
    // lib/entry). nullopt when the lists do not say. The surface choice itself is not decoded.
    std::optional<std::pair<std::string, std::string>> touchdown_effect(std::int32_t event_list) const;

    // Floors above the landscape (object floors, bridges, building roofs): the highest surface on the
    // segment top → bottom, if any. The original's FollowGround probes the whole world (IWorld faces).
    using FloorProbe = std::function<std::optional<float>(Vec3 top, Vec3 bottom)>;
    // DropToGround: the current state's contact points onto the ground (mean height); `floor` adds the
    // object faces (load() has none yet).
    void drop_to_ground(float now_ms, const TerrainGround& ground, const FloorProbe& floor = {});
    // Back to the TMA placement, then DropToGround with the object faces (play, once they exist).
    void place(float now_ms, const TerrainGround& ground, const FloorProbe& floor);
    // After motion->update(now): ground following from the feet, then out of static walls and
    // inside the landscape box. Returns the collision push applied.
    Vec3 settle(float now_ms, const TerrainGround& ground, const StaticColliders& statics, const FloorProbe& floor = {});
    // Collision sphere centre in the world (after settle).
    Vec3 sphere_world() const { return frame.matrix.transform_point(sphere_centre); }
    Vec3 position() const { return frame.matrix.transform_point({}); }
    // Turret target 0..1 per axis (TurretRig::node_frames): HERO.TBL's turret channels for the hero,
    // the AI's aim for the others; the manual-entry default straight ahead.
    float aim_x = TurretRig::kManualX, aim_y = TurretRig::kManualY;
    // Further joint-driven keyframe times applied after the turret rig: the gun devices' barrel
    // channels (recoil), set by combat. {part index, mesh node, frame}.
    struct JointFrame {
        std::size_t part = 0;
        std::int32_t node = -1;
        float frame = 0;
    };
    std::vector<JointFrame> joint_frames;
    // The same for the unit's own non-weapon devices (building doors), kept by their owner (combat
    // clears joint_frames every step).
    std::vector<JointFrame> device_frames;
    // Transforms not drawn (collapsed): parts thrown off as debris (DetachPartDebris), set by combat.
    std::vector<bool> detached;
    // Port option (the player's unit): mount the turret and its guns as if the chassis root kept its
    // rest rotation, so the cockpit view, the barrels and the sight do not sway with the walk clip
    // (pose_unit `steady_root`). The original sways them all together.
    bool steady_mounts = false;
    Mat4 root_rest = Mat4::identity(); // the chassis root's static-pose rotation
    // World matrix of every transform of model.mesh (part base + node) after the last pose_parts().
    std::vector<Mat4> posed;
    // Pose every part at the current frame with the turret at (aim_x, aim_y) and the joint frames.
    void pose_parts();
    // pose_parts(), then copy into `transforms` from first_transform (hidden = collapsed, e.g. the own
    // unit seen from its TURRET camera, or a removed unit).
    void pose(std::vector<Mat4>& transforms, bool hidden);

private:
    LiveUnit() = default;
    // Touch-down tracking per contact: lands during the current step / was down at the previous step's end.
    std::vector<bool> landing_, down_at_end_;
    float last_step_ = -1;
};

// Unit-vs-unit collision (CCollManager broad phase + PushApart) for the tick: overlapping spheres are
// separated along the line of centres, the push split by squared mass; ground units are pushed
// horizontally.
void collide_units(std::vector<LiveUnit*>& units, float now_ms);

} // namespace parkan
