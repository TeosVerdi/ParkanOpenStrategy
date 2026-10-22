#include "ai/fire_control.hpp"

#include "ai/behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ai {

namespace {
constexpr float kAnimalFacing = 0.85f;     // 10059948: cos of the bite cone
constexpr float kTargetChoiceRange = 500;  // 100254b0..100256e0 (2-D)
constexpr float kFriendRayRadius = 0.5f;   // the ray's sphere in 10025c60
constexpr float kDefaultFriendRadius = 2;  // a friend without a radius (port)

// 1001b650: the weapon timer for fire frequency factor F (≤ 0 → 1).
void retime(Weapon& w, float f) {
    if (!(f > 0)) f = 1;
    if (w.rate > 2) {
        const float p = 30 / w.rate / f;
        w.timer.set_period(p, p);
    } else {
        w.timer.set_period(0.5f / f, 1.5f / f);
    }
}

// Segment a→b against a sphere: the sweep test of 10025c60 with both spheres at rest (inferred).
bool segment_hits_sphere(Vec3 a, Vec3 b, Vec3 c, float r) {
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    float t = len2 > 0 ? dot(c - a, ab) / len2 : 0;
    t = std::clamp(t, 0.f, 1.f);
    const Vec3 p = a + ab * t - c;
    return dot(p, p) < r * r;
}
} // namespace

AimAngles aim_angles(const Mat4& m, Vec3 mount, Vec3 aim) {
    const Vec3 fwd{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, right{m.at(0, 0), m.at(1, 0), m.at(2, 0)},
        up{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    const Vec3 d = aim - mount;
    const float x = dot(d, right), y = dot(d, fwd), z = dot(d, up);
    return {std::atan2(x, y), std::atan2(z, std::hypot(x, y))};
}

float range_factor(const Weapon& w, float d, float dz) {
    if (w.flags & 0x10) return 1.1f;
    if (w.flags & 0x8) return 1.0f;
    const float k = w.height > 0 ? 1 - dz / w.height : 1;
    if (d < w.near) return k * (d / w.near);
    if (d <= w.opt) return k;
    if (d < w.far) return k * ((d - w.far) / (w.opt - w.far));
    return 0;
}

void FireControl::set_mode(const FireMode& m) {
    if (!overridden) mode_ = m;
    for (Weapon& w : weapons) retime(w, freq_factor);
}

// The target choice of modes 2–4 among the sensor's enemies (observer list MB+0x4f8).
std::uint32_t FireControl::choose(Behaviour& b, Vec3 own) const {
    float best = 0;
    std::uint32_t lid = kNoObject;
    for (const std::uint32_t id : b.enemies) {
        const auto o = b.object(id);
        if (!o) continue;
        const float d = dist2d(own, o->pos);
        float s = 0;
        if (mode_.mode == kFireAtWill) s = 1 / (10 + d);
        else if (mode_.mode == kFireStrongest) s = (o->weapon + o->armour) / (d + 10);
        else s = 1 / ((d + 10) * (o->weapon + o->armour + 10));
        if (d < kTargetChoiceRange && best < s) best = s, lid = id;
    }
    return lid;
}

void FireControl::takt(Behaviour& b, std::uint32_t now) {
    out_.trigger = 0;
    if (!gate.check_and_rearm(now, b.rand)) return;
    if (b.self.animal() && b.walker.idle()) return; // animals bite only on the move
    const UnitState st = b.walker.unit_state();
    if (retarget.check_and_rearm(now, b.rand)) {
        hold.reset();
        switch (mode_.mode) {
        case kFireTarget: current_ = mode_.target; break;
        case kFireAtWill:
        case kFireStrongest:
        case kFireWeakest: current_ = choose(b, st.pos); break;
        default: current_ = kNoObject; break;
        }
    }
    const auto o = b.object(current_);
    out_.target = o ? current_ : kNoObject;
    out_.track = false;
    out_.line_clear = false;
    if (overridden || !o) return; // manual fire (10025a00) not modelled; no target: nothing to do

    const Vec3 tpos = o->pos;
    const float dist = std::sqrt(dot(st.pos - tpos, st.pos - tpos)), dz = st.pos.z - tpos.z;
    const Vec3 to_target = tpos - st.pos;
    const float facing = dist > 0 ? dot(st.dir, to_target * (1 / dist)) : 1;
    const float radius = o->radius > 1e-4f ? o->radius : 1.f; // property 0x206 (≤ 1e-4: asked again; port: 1 m)

    // Line of fire: the world ray, then no friendly unit in a 0.5 m sweep along it.
    const Vec3 from = st.pos + Vec3{0, 0, muzzle_height};
    bool clear = b.world->line_clear(from, tpos, b.self.logic_id, current_);
    if (clear) {
        std::vector<std::uint32_t> near;
        b.world->objects_near(st.pos, dist + 10, near);
        for (const std::uint32_t id : near) {
            if (id == b.self.logic_id || id == current_ || (id & kBuildingBit)) continue;
            const auto f = b.object(id);
            if (!f || f->clan != b.self.clan) continue;
            const float r = (f->radius > 0 ? f->radius : kDefaultFriendRadius) + kFriendRayRadius;
            if (segment_hits_sphere(from, tpos, f->pos, r)) {
                clear = false;
                break;
            }
        }
    }
    out_.aim = tpos;
    out_.distance = dist;
    out_.line_clear = clear;
    for (const Weapon& w : weapons) out_.track = out_.track || w.group >= 0;

    // The fire threshold: a unit without independent targeting must aim better when it can move.
    const float threshold = (!b.self.can_targeting && (b.walker.max_speed >= 0.5f || b.self.building())) ? 0.85f : 0.45f;
    for (std::size_t i = 0; i < weapons.size(); ++i) {
        Weapon& w = weapons[i];
        const float g_err = (w.group >= 0 && std::size_t(w.group) < turret_error.size()) ? turret_error[std::size_t(w.group)] : 0;
        const float g_f = 1 - g_err / radius * dist;
        const float w_f = b.self.animal() ? 1.f : 1 - w.aim_error / radius * dist;
        const float r_f = range_factor(w, dist, dz);
        const bool ok = b.self.animal() ? facing > kAnimalFacing : (g_f > 0 && w_f > 0 && r_f * w_f * g_f > threshold);
        if (!ok) continue;
        if (w.timer.check_and_rearm(now, b.rand) && clear) {
            out_.trigger |= 1u << i;
            b.last_fire = now;
        }
    }
}

} // namespace parkan::ai
