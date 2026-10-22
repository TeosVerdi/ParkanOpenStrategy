#include "control/turret.hpp"

#include <cmath>
#include <cstring>

namespace parkan {

namespace {

float wrap01(float v) { return v - std::floor(v); }

template <class T>
T at(const std::uint8_t* p, std::size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

} // namespace

std::vector<CtldJoint> decode_joints(const Ctld& c) {
    std::vector<CtldJoint> out;
    for (const auto& raw : c.joints) {
        const std::uint8_t* p = raw.data();
        out.push_back({at<std::int32_t>(p, 0x00), at<float>(p, 0x04), at<float>(p, 0x08), at<float>(p, 0x0c),
                       at<std::int32_t>(p, 0x10), at<std::int32_t>(p, 0x14), at<float>(p, 0x18), at<float>(p, 0x1c),
                       at<std::uint32_t>(p, 0x20)});
    }
    return out;
}

std::optional<TurretRig> TurretRig::from_ctld(const Ctld& c) {
    TurretRig r;
    r.joints_ = decode_joints(c);
    const CtldDevice* turret = nullptr;
    for (const auto& d : c.devices) {
        if (d.type == 1 && !turret) turret = &d;
        if (d.type == 4 && r.camera_eye_point < 0 && !d.joints.empty() && d.joints[0] < r.joints_.size()) {
            const CtldJoint& j = r.joints_[d.joints[0]];
            r.camera_eye_point = j.point_a, r.camera_axis_point = j.point_b;
            r.camera_near = at<float>(d.header.data(), 0x2c);
            r.camera_far = at<float>(d.header.data(), 0x30);
            r.camera_fov = at<float>(d.header.data(), 0x34);
        }
    }
    if (!turret) return std::nullopt;
    r.invert_inputs_ = (at<std::uint32_t>(turret->header.data(), 0x08) & 0x4000000) != 0;
    for (auto j : turret->joints)
        if (j < r.joints_.size()) r.channels_.push_back(j);
    // LoadControlData appends the gun joints (flag 8) as further channels of the turret device.
    for (std::uint32_t j = 0; j < r.joints_.size(); ++j)
        if (r.joints_[j].flags & 8) r.channels_.push_back(j);
    return r;
}

void TurretRig::node_frames(float target_x, float target_y, float yaw_offset, std::vector<float>& frames) const {
    const float ax = invert_inputs_ ? 1 - target_x : target_x, ay = invert_inputs_ ? 1 - target_y : target_y;
    // SetYawOffsetFromAttitude (10028990): dev+0xe8 = a / range with a = -offset, negated unless the
    // device has flag 0x4000000.
    float prev = 0, pitch = ay;
    for (std::size_t k = 0; k < channels_.size(); ++k) {
        const CtldJoint& j = joints_[channels_[k]];
        float cur;
        if (k == 0) cur = ax;
        else if (k == 1) cur = ay;
        else if (j.flags & 0x40) cur = prev; // follower barrel: the previous channel
        else cur = pitch;                    // AimGunChannel: the pitch channel's target
        prev = cur;
        if (j.flags & 4) continue;
        float t = cur;
        if (k == 0 && j.range != 0) t += (invert_inputs_ ? -yaw_offset : yaw_offset) / j.range;
        t = (j.flags & 1) ? wrap01(t) : std::clamp(t, 0.f, 1.f);
        if (j.flags & 2) t = 1 - t;
        if (j.part >= 0) {
            if (frames.size() <= std::size_t(j.part)) frames.resize(std::size_t(j.part) + 1, -1.f);
            frames[std::size_t(j.part)] = j.frame_from + t * (j.frame_to - j.frame_from);
        }
    }
}

} // namespace parkan
