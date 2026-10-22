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
