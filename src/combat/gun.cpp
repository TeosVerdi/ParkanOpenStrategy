#include "combat/gun.hpp"

#include <algorithm>

namespace parkan::combat {

GunDevice::GunDevice(GunDeviceSpec spec, std::vector<CtldJoint> joints, const BulletProto* bullet)
    : spec_(std::move(spec)), joints_(std::move(joints)), bullet_(bullet) {
    param_ = spec_.param;
    // ReloadBulletInfo (100296f0): range from the bullet's body[22] (min the seeker range); lock-on
    // time and cone cosine from the seeker, −1 without one.
    param_[9] = param_[10] = -1;
    if (bullet_) {
        if (bullet_->range > 0) param_[8] = bullet_->range;
        if (bullet_->seeker) {
            param_[8] = std::min(param_[8], (*bullet_->seeker)[1]);
            param_[9] = (*bullet_->seeker)[2] * 0.001f;
            param_[10] = std::cos((*bullet_->seeker)[0]);
        }
    }
    for (auto j : spec_.barrels) channels_.push_back({j});
    ammo_ = std::int32_t(std::lround(param_[0])); // Load: round(param0), −1 = unlimited
    energy_ = param_[1];                          // starts full (port; spec §7.1)
    lock_ = param_[9];
}

void GunDevice::set_state(std::uint32_t s) {
    if (s == kGunReset) {
        state_ = kGunReady;
        lock_ = param_[9];
    } else if (s != command_) {
        command_ = s;
        state_ = kGunFiring;
    }
}

bool GunDevice::skip_joint(std::size_t b) const {
    const std::uint32_t j = channels_[b].joint;
    return j < joints_.size() && (joints_[j].flags & 0x40);
}

void GunDevice::apply_joints(float now) {
    const float span = end_ - start_;
    const float k = span > 0 ? std::clamp((now - start_) / span, 0.f, 1.f) : 1.f;
    for (auto& c : channels_) c.cur = c.from + (c.to - c.from) * k;
}

void GunDevice::extend_move_end(std::size_t b) {
    // ExtendMoveEndTime (10022120): end = max(end, start + |to − from|·1000/rate), the short way round
    // for a wrapping joint.
    const Channel& c = channels_[b];
    float d = std::abs(c.to - c.from);
    const CtldJoint* j = c.joint < joints_.size() ? &joints_[c.joint] : nullptr;
    if (j && (j->flags & 1) && d > 0.5f) d = 1 - d;
    if (!j || j->rate <= 0) return; // port guard: no rate, no duration
    end_ = std::max(end_, d * 1000.f / j->rate + start_);
}

bool GunDevice::step_barrel(std::size_t b, GunHost& host) {
    Channel& c = channels_[b];
    if (c.counter == 0) c.counter = 4;
    --c.counter;
    switch (c.counter) {
    case 0:
        c.cur = c.from = c.to = 0;
        if (ammo_ != -1 && ammo_ != 0) --ammo_;
        return true;
    case 1:
        c.from = c.cur, c.to = 1; // FUN_1002b370(channel, 1.0)
        break;
    case 2: {
        c.from = c.cur, c.to = 0.5f;
        Vec3 pos, dir;
        if (bullet_ && host.muzzle(c.joint, pos, dir)) {
            ++shots_;
            host.spawn(*this, pos, dir);
        }
        end_ = start_ + param_[4];
        break;
    }
    case 3:
        c.from = c.cur, c.to = 0.5f;
        host.recoil(*this, std::uint32_t(b));
        break;
    }
    extend_move_end(b);
    return false;
}

void GunDevice::tick(float dt, GunHost& host) {
    if (std::isinf(recharge_per_s)) energy_ = param_[1];
    else energy_ = std::min(param_[1], energy_ + recharge_per_s * dt * 0.001f);
    if (channels_.empty()) {
        state_ = kGunEmpty; // a module device without barrels cannot fire
        return;
    }
    if (channels_[cur_].counter == 0) {
        if (host.broken(spec_.part) || ammo_ == 0) {
            state_ = kGunEmpty;
            return;
        }
        if (param_[1] > 0 && param_[2] > energy_) {
            state_ = kGunNoEnergy;
            return;
        }
        if (!enabled) {
            state_ = kGunOutOfRange;
            return;
        }
        if (param_[8] > 0) {
            const auto target = host.target_position();
            if (!target) {
                if (param_[9] > 0) { // a guided weapon without a target: lock reset, then it fires unguided
                    state_ = kGunReady;
                    lock_ = param_[9];
                }
            } else {
                Vec3 pos, dir;
                const bool have = host.muzzle(channels_[cur_].joint, pos, dir);
                const Vec3 to = *target - pos;
                if (have && dot(to, to) > param_[8] * param_[8]) {
                    state_ = kGunOutOfRange;
                    lock_ = param_[9];
                    return;
                }
                if (param_[10] > 0) {
                    if (!have || dot(dir, normalize(to)) <= param_[10]) {
                        state_ = kGunNotAimed;
                        lock_ = param_[9];
                        return;
                    }
                    state_ = kGunLocking;
                }
                if (param_[9] > 0 && lock_ > 0) {
                    if (state_ == kGunLocking) lock_ -= dt * 0.001f;
                    return;
                }
            }
        }
        if (command_ == kCeaseFire) {
            state_ = kGunIdle;
            return;
        }
    }
    state_ = kGunFiring;
    lock_ = param_[9];
    if (!(spec_.flags & GunDeviceSpec::kSalvo)) {
        if (!step_barrel(cur_, host)) return;
        // The next barrel, skipping follower joints (flag 0x40).
        for (std::size_t n = 0; n < channels_.size(); ++n) {
            cur_ = (cur_ + 1) % channels_.size();
            if (!skip_joint(cur_)) break;
        }
    } else {
        bool done = false;
        for (std::size_t b = 0; b < channels_.size(); ++b)
            if (!skip_joint(b)) done |= step_barrel(b, host);
        if (!done) return;
    }
    if (param_[1] > 0 && param_[2] <= energy_) energy_ -= param_[2];
    state_ = kGunInterval;
    end_ = param_[3] + start_;
    if (command_ == kFireSingle) command_ = kCeaseFire;
}

void GunDevice::update(float now, GunHost& host) {
    // UpdateDevices (1002d260): every step whose end time has passed runs; a step without duration
    // (end == start) is stretched to `now`, so it ends this frame.
    for (int guard = 0; (end_ < now || !started_) && guard < 64; ++guard) {
        apply_joints(now);
        float dt = end_ - start_;
        if (end_ == start_ || !started_) {
            dt = now - start_;
            end_ = now;
        }
        start_ = end_;
        tick(dt, host);
        started_ = true;
    }
}

std::optional<std::pair<std::int32_t, float>> GunDevice::barrel_frame(std::size_t b, float now) const {
    if (b >= channels_.size() || channels_[b].joint >= joints_.size()) return std::nullopt;
    const CtldJoint& j = joints_[channels_[b].joint];
    if (j.part < 0 || (j.flags & 4) || j.frame_from < 0) return std::nullopt;
    const Channel& c = channels_[b];
    const float span = end_ - start_;
    const float k = span > 0 ? std::clamp((now - start_) / span, 0.f, 1.f) : 1.f;
    float t = std::clamp(c.from + (c.to - c.from) * k, 0.f, 1.f);
    if (j.flags & 2) t = 1 - t;
    return std::pair{j.part, j.frame_from + t * (j.frame_to - j.frame_from)};
}

} // namespace parkan::combat
