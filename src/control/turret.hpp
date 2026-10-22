// Turret aiming and the turret camera: Control's CTurretDevice (CTLD device type 1) and
// CCameraDevice (type 4), driven through CTLD joints. A joint does not build a matrix: it plays a
// frame range of its mesh node's own keyframes (IAniMesh SetPartAnimRange/SetPartBlend), so aiming
// is "a time per node". Spec: notes/turret-camera.md §0, §1, §2.1, §3.
#pragma once

#include "core/math.hpp"
#include "res/ctld.hpp"

#include <optional>

namespace parkan {

// CTLD joint record (0x24 bytes).
struct CtldJoint {
    std::int32_t part = -1;              // mesh node
    float frame_from = 0, frame_to = 0;  // keyframe range played over t = 0..1
    float default_t = 0;
    std::int32_t point_a = -1, point_b = -1; // CTPT points (camera: eye, view axis)
    float rate = 0;                      // t per second (AI states only)
    float range = 0;                     // radians per 1.0 of t
    std::uint32_t flags = 0;             // 1 wrap, 2 invert, 4 no mesh output, 8 gun channel, 0x40 copy previous
};
std::vector<CtldJoint> decode_joints(const Ctld& c);

class TurretRig {
public:
    // nullopt when the CTLD has no turret device (type 1).
    static std::optional<TurretRig> from_ctld(const Ctld& c);

    // The turret device's target vector (dev+0x9c) at entering manual control (SetComponentControl):
    // {0.5, 0.5}, i.e. straight ahead with the barrel at the middle of its range.
    static constexpr float kManualX = 0.5f, kManualY = 0.5f;

    // CTurretDevice::Update (manual state 0x200, no slew) + CDevice::ApplyJoints: the keyframe time of
    // every node the joints drive; other nodes keep -1 (static pose). `yaw_offset` is the chassis'
    // interpolated attitude offset (rad), which the yaw channel counter-rotates (strafing).
    void node_frames(float target_x, float target_y, float yaw_offset, std::vector<float>& frames) const;

    // The sight the guns converge on (CGunDevice +0x114 / +0x110): the yaw joint's CTPT point A
    // (origin) and the pitch joint's point B (direction); -1 when the rig has no such joints.
    std::int32_t sight_origin_point = -1, sight_dir_point = -1;

    // Gun components mounted on the turret (o_gun_* on o_tur_*): LoadControlData merges their CTLD
    // joints into the unit's, so their gun joints (flag 8) are further channels of the turret device and
    // follow its pitch channel like the turret's own (AimGunChannel). `joints` is the component's CTLD.
    void gun_frames(const std::vector<CtldJoint>& joints, float target_y, std::vector<float>& frames) const;

    // CTurretDevice state 0x400 ("AI point", turret-camera.md §1.3): the yaw / pitch channel times for
    // angles in the body frame (yaw > 0 right, pitch > 0 up) are t = angle / joint range + default t; the
    // result is in node_frames' input space (target_x, target_y). The angle's sign flips on a joint with
    // inverted output (flag 2): inferred, checked on o_tur_ht_02 by tools/combat_tests.cpp.
    std::pair<float, float> inputs_for_angles(float yaw, float pitch) const;
    // Moves (x, y) toward (tx, ty) at the yaw / pitch joint rates (t per second) for `dt_s` seconds,
    // the short way round on a wrapping joint (SlewChannel 100289f0).
    void slew(float& x, float& y, float tx, float ty, float dt_s) const;

    // The camera device (type 4): its joint's CTPT points (eye, view axis) and projection
    // (near p0, far p1, FOV p2 in radians).
    std::int32_t camera_eye_point = -1, camera_axis_point = -1;
    float camera_near = 0.1f, camera_far = 1000.f, camera_fov = 1.3f;

private:
    std::vector<CtldJoint> joints_;
    std::vector<std::uint32_t> channels_; // joint indices: yaw, pitch, then gun joints (flag 8)
    bool invert_inputs_ = false;          // device flag 0x4000000: a = 1 - target
};

} // namespace parkan
