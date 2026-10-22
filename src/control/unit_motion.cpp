#include "control/unit_motion.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace parkan {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kGravity = 10; // IWorld::GetGravity default

float& comp(Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// FastProc +0x1c: v = R·v.
Vec3 rotate(const Mat4& m, Vec3 v) {
    return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z,
            m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
            m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}
// FastProc +0x8c: v = Rᵀ·v.
Vec3 inv_rotate(const Mat4& m, Vec3 v) {
    return {m.at(0, 0) * v.x + m.at(1, 0) * v.y + m.at(2, 0) * v.z,
            m.at(0, 1) * v.x + m.at(1, 1) * v.y + m.at(2, 1) * v.z,
            m.at(0, 2) * v.x + m.at(1, 2) * v.y + m.at(2, 2) * v.z};
}
Vec3 translation(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }
void set_translation(Mat4& m, Vec3 t) { m.at(0, 3) = t.x, m.at(1, 3) = t.y, m.at(2, 3) = t.z; }

// FastProc +0x3c QuatToMatrixFlipZ: S·R(q)·S with S = diag(1,1,-1), i.e. R of (-x, -y, z, w). Yaw
// keeps its sense (positive = counter-clockwise seen from above); pitch and roll flip.
Mat4 flip_z(Quat q) { return Mat4::rotation({-q.x, -q.y, q.z, q.w}); }

// QuatMul (10016980), Hamilton product.
Quat mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// Rz(z)·Rx(x)·Ry(y) from QuatFromAxisAngle (100141c0).
Quat euler_zxy(Vec3 r) {
    const Quat qx{std::sin(r.x / 2), 0, 0, std::cos(r.x / 2)}, qy{0, std::sin(r.y / 2), 0, std::cos(r.y / 2)},
        qz{0, 0, std::sin(r.z / 2), std::cos(r.z / 2)};
    return mul(mul(qz, qx), qy);
}

} // namespace

UnitMotion::UnitMotion(const Ctld& ctld, const Mesh& mesh, std::span<const ContactPoint> points)
    : body(ctld.body), graph_(ctld), mesh_(&mesh), file_body_(ctld.body) {
    contacts_ = precompute_motion_states(graph_, mesh, points);
    default_state_.contacts.resize(graph_.contact_count);
    live_ = graph_.size() ? graph_.states[0] : default_state_;
    // PrecomputeMotionStates leaves the mesh at range {0,0}{0,0}, time 0, weight 1.
    anim_.time = 0, anim_.weight = 1;
}

void UnitMotion::set_linear_velocity(Vec3 v) { v_ = v, velocity_mode_ = true; }
void UnitMotion::set_throttle(Vec3 t) { throttle_ = t, velocity_mode_ = false; }
void UnitMotion::set_angular_command(Vec3 a) { ang_cmd_ = a, attitude_mode_ = false; }
void UnitMotion::set_attitude_target(Vec3 turns) { attitude_ = turns, attitude_mode_ = true; }

void UnitMotion::start(float now_ms, const Mat4& pose) {
    active_ = true;
    last_time_ = step_start_ = clock_ = now_ms;
    step_dur_ = 0;
    end_ = start_ = out_ = pose;
    queue_.clear();
    cur_ = 0;
    live_ = graph_.size() ? graph_.states[0] : default_state_;
}

void UnitMotion::update(float now_ms) {
    const float dt = now_ms - last_time_;
    if (!(dt >= 1.0f)) return; // not stamped: the next call retries
    last_time_ = now_ms;
    if (!active_) {
        step_start_ = clock_ = now_ms;
        return;
    }
    // Variable-step catch-up: one whole motion state per iteration, no iteration cap.
    while (clock_ <= now_ms) {
        select_next_motion();
        if (queue_.empty()) break; // stall: the clock stays <= now and the next Update retries
        cur_ = queue_.front();
        queue_.pop_front();
        live_ = cur_ < graph_.size() ? graph_.states[cur_] : default_state_;
        if (live_.enter_event_list >= 0 && on_enter_events) on_enter_events(live_.enter_event_list);
        advance_motion();
    }
    interpolate(now_ms);
}

bool UnitMotion::matches(const CtldState& s) const { return motion_state_matches(s, v_, ang_cmd_, contacts_, mode_); }

void UnitMotion::select_next_motion() {
    if (!(live_.flags & 1)) return; // a chained sub-state: keep consuming the queue
    const auto use = [&](std::uint32_t k) {
        if (k >= graph_.size()) return;
        std::int32_t& uses = graph_.states[k].uses; // the state array, never the live copy
        if (uses != 0 && uses != -1) --uses;
    };
    if (!matches(live_)) {
        float best = MotionGraph::kNoPath;
        std::uint32_t best_k = 0;
        for (std::uint32_t k = 0; k < graph_.size(); ++k) {
            if (k == cur_) continue;
            const MotionState& s = graph_.states[k];
            if ((s.flags & 1) && matches(s)) {
                const float c = graph_.shortest_path(cur_, k);
                if (c < best) best = c, best_k = k; // strict: the lowest index wins ties
            }
        }
        if (best < MotionGraph::kNoPath) {
            queue_motion(best_k, true);
            use(best_k);
            return;
        }
    }
    // Still matching, or no target reachable: replay the cheapest cycle back to the current state.
    queue_motion(cur_, true);
    use(cur_);
}

void UnitMotion::queue_motion(std::uint32_t target, bool clear) {
    if (graph_.size() == 0) {
        queue_.push_back(cur_);
        return;
    }
    if (clear) queue_.clear();
    const std::uint32_t from = queue_.empty() ? cur_ : queue_.back();
    if (graph_.shortest_path(from, target) < MotionGraph::kNoPath) {
        // The path excluding `from`, including `target`.
        std::size_t x = graph_.next(from);
        queue_.push_back(std::uint32_t(x));
        for (std::size_t guard = 0; x != target && guard < graph_.size(); ++guard) {
            x = graph_.next(x);
            queue_.push_back(std::uint32_t(x));
        }
    }
}

void UnitMotion::advance_motion() {
    const std::uint32_t flags = live_.flags;
    // Finish the previous step at u = 1. Quirk: live_ already holds the new state, so its flags and
    // track starts are used for that last translation; only the rotation (InvRotate below) matters.
    interpolate(clock_);
    start_ = end_; // BeginStep
    att_delta_ = {}; // BeginStep also clears m+0x188
    step_start_ = clock_;
    prev_w_ = w_;
    w_ = 1;
    if (flags & 0x100000) {
        // No mover and no animation calls: the previous step's tracks and rotation replay.
        step_dur_ = live_.duration_ms;
        clock_ += live_.duration_ms;
        return;
    }
    const bool unclamped = flags & 0x40000;

    // ClampVelocity: the request against the runtime max speed (written back), then a copy into the
    // state's linear range on all three axes whatever the mask, rotated into the world.
    if (!unclamped)
        for (int i = 0; i < 3; ++i) comp(v_, i) = std::clamp(comp(v_, i), -body[6 + i], body[6 + i]);
    Vec3 vc;
    for (int i = 0; i < 3; ++i) comp(vc, i) = std::clamp(comp(v_, i), live_.lin_min[i], live_.lin_max[i]);
    const Vec3 vw = rotate(start_, vc);

    float dt = live_.duration_ms * 0.001f;
    const Vec3 vl = inv_rotate(out_, vw); // the output matrix carries the lean
    const float vmax = std::max({std::abs(vl.x), std::abs(vl.y), std::abs(vl.z)});
    if ((flags & 0x10000) && dt != 0 && vmax > 1e-9f && vmax * dt > 5) dt = 5 / vmax; // at most 5 m per step
    if (dt == 0 && vmax > 1e-9f) {
        if (!(flags & 0x10000)) {
            // Root-motion state: track-B weight from where vmax sits in the range (mask ignored).
            float rng = 0, hi_max = 0, hi_min = 0;
            for (int i = 0; i < 3; ++i) {
                rng = std::max(rng, std::abs(std::abs(live_.lin_max[i]) - std::abs(live_.lin_min[i])));
                hi_max = std::max(hi_max, std::abs(live_.lin_max[i]));
                hi_min = std::max(hi_min, std::abs(live_.lin_min[i]));
            }
            if (rng > 0) {
                const float lower = hi_min < hi_max ? hi_min : hi_max;
                const float p = std::clamp((vmax - lower) / rng, 0.f, 1.f);
                w_ = (1 - p) * live_.min_b_weight + p;
            }
        }
        dt = (w_ * live_.len_b + (1 - w_) * live_.len_a) / vmax; // root-motion speed = vmax exactly
    }
    if (flags & 0x1000000) { // ±12.5 % jitter (LFSR 10042230)
        std::uint16_t lo = std::uint16_t(jitter_lfsr), hi = std::uint16_t(jitter_lfsr >> 16);
        lo = std::uint16_t((lo << 1) ^ hi);
        hi = std::uint16_t((hi >> 1) ^ lo);
        jitter_lfsr = lo | std::uint32_t(hi) << 16;
        dt = float(hi) * (dt * 0.25f / 65536) - dt * 0.25f * 0.5f + dt;
    }
    dt = std::clamp(dt, 0.01f, 5.0f); // a zero dt (no speed) becomes 10 ms
    step_dur_ = dt * 1000;
    clock_ += dt * 1000;

    step_rotation(dt, unclamped);
    step_linear(dt, unclamped);

    // The animation weight lags one step: the blend starts at prev_w_ and Interpolate moves it to w_.
    anim_.a = live_.track_a, anim_.b = live_.track_b;
    anim_.set_blend(0, prev_w_);

    if (!(flags & 0x10000)) { // ApplyRootMotion: the blended track displacement, in the end frame
        const Vec3 d = live_.disp_a * (1 - w_) + live_.disp_b * w_;
        const Vec3 dw = rotate(end_, d);
        set_translation(end_, translation(end_) + dw);
        world_vel_ = dw * (1 / dt);
    } else { // IntegrateVelocity: explicit Euler with the velocity from before StepLinear
        set_translation(end_, translation(end_) + vw * dt);
        world_vel_ = vw;
    }
}

void UnitMotion::step_rotation(float dt, bool unclamped) {
    const std::uint32_t flags = live_.flags;
    Vec3 rate_dt;
    for (int i = 0; i < 3; ++i) comp(rate_dt, i) = body[9 + i] * dt;

    if (attitude_mode_) {
        // The command that consumes the remaining turn, written back so SelectNextMotion sees it.
        for (int i = 0; i < 3; ++i) {
            const float r = comp(rate_dt, i);
            float& t = comp(attitude_, i);
            const float c = r != 0 ? std::clamp(-(t - 0.5f) * kTwoPi / r, -1.f, 1.f) : 0.f;
            comp(ang_cmd_, i) = c;
            t += c * r / kTwoPi;
            t -= std::floor(t);
        }
    } else {
        attitude_ = {0.5f, 0.5f, 0.5f};
    }

    Vec3 cmd = ang_cmd_; // the pitch/roll masks act on a copy (assumed; the request stays as given)
    if (flags & (0x10 | 0x40)) cmd.x = 0;
    if (flags & (0x20 | 0x80)) cmd.y = 0;
    Vec3 rot;
    for (int i = 0; i < 3; ++i) comp(rot, i) = comp(cmd, i) * comp(rate_dt, i) + comp(level_, i) * body[12 + i];
    // Differential drive: a side with less health makes the unit veer.
    const float drive = velocity_mode_ ? (body[7] != 0 ? v_.y / body[7] : 0.f) : throttle_.y;
    rot.z += drive * (drive_right - drive_left);
    if (!unclamped)
        for (int i = 0; i < 3; ++i) comp(rot, i) = std::clamp(comp(rot, i), -comp(rate_dt, i), comp(rate_dt, i));
    // The attitude offset's change since the last step, after the clamp (strafing).
    att_delta_ = att_offset_ - att_prev_;
    att_prev_ = att_offset_;
    rot = rot + att_delta_;

    q_step_ = euler_zxy(rot);
    end_ = end_ * flip_z(q_step_); // body-frame post-multiply; translation unchanged

    // Visual lean: per axis, byte & 0x7f picks a source, bit 7 negates it.
    lean_prev_ = lean_cur_;
    for (int i = 0; i < 3; ++i) {
        const unsigned sel = live_.lean[i] & 0x7f;
        float src = 0;
        if (sel >= 1 && sel <= 3 && comp(rate_dt, sel - 1) != 0) src = comp(rot, sel - 1) / comp(rate_dt, sel - 1);
        else if (sel >= 4 && sel <= 6 && file_body_[6 + sel - 4] != 0) src = comp(v_, sel - 4) / file_body_[6 + sel - 4];
        else if (sel >= 8 && sel <= 10 && file_body_[sel - 8] != 0) src = comp(dv_, sel - 8) / file_body_[sel - 8];
        if (live_.lean[i] & 0x80) src = -src;
        const float max_lean = file_body_[15 + i], step = body[9 + i] * dt;
        float& l = comp(lean_angles_, i);
        l += std::clamp(max_lean * src - l, -step, step);
        l = std::clamp(l, -max_lean, max_lean);
    }
    lean_cur_ = euler_zxy(lean_angles_);
}

void UnitMotion::step_linear(float dt, bool unclamped) {
    const Vec3 old = v_;
    if (!velocity_mode_) // ramp toward the throttle target without overshooting, same rate both ways
        for (int i = 0; i < 3; ++i) {
            const float step = body[i] * dt;
            comp(v_, i) += std::clamp(comp(throttle_, i) * body[6 + i] - comp(v_, i), -step, step);
        }
    // body[21] is an int: 2 = ground (slope limit, needs the ground normal: not modelled here),
    // 3 = ballistic.
    if (std::bit_cast<std::int32_t>(body[21]) == 3) v_ = v_ + inv_rotate(start_, {0, 0, -kGravity}) * dt;
    if (!unclamped)
        for (int i = 0; i < 3; ++i) comp(v_, i) = std::clamp(comp(v_, i), -body[6 + i], body[6 + i]);
    dv_ = v_ - old;
}

MotionFrame UnitMotion::interpolate(float now_ms) {
    // u = 1 for a zero duration (the original divides by it; only the first step can see one).
    const float u = step_dur_ > 0 ? std::clamp((now_ms - step_start_) / step_dur_, 0.f, 1.f) : 1.f;
    const float b = std::min(4 * u, 1.f); // the blend reaches w over the first 25 % of the step
    const float blend = b * w_ + (1 - b) * prev_w_;
    anim_.set_blend(u, blend);

    // CMover::Interpolate
    Mat4 m = start_ * flip_z(slerp(Quat{}, q_step_, u)); // translation stays start's
    if (live_.flags & 0x10000) {
        set_translation(m, translation(m) + (translation(end_) - translation(start_)) * u);
    } else {
        // The mesh's actual root at this animation time minus the blended track start, so the feet
        // do not slide. The root uses AniMesh's stored weight, the start the requested blend.
        const Vec3 r = root_motion(*mesh_, anim_.a, anim_.b, anim_.time, anim_.weight);
        const Vec3 s0 = live_.start_a * (1 - blend) + live_.start_b * blend;
        set_translation(m, translation(m) + rotate(m, r) - rotate(m, s0));
    }
    out_ = m * flip_z(slerp(lean_prev_, lean_cur_, u));

    MotionFrame f;
    f.matrix = out_;
    f.frame_a = anim_.time * anim_.a[1] + (1 - anim_.time) * anim_.a[0];
    f.frame_b = anim_.time * anim_.b[1] + (1 - anim_.time) * anim_.b[0];
    f.weight_b = anim_.weight;
    f.attitude_offset = att_prev_ - att_delta_ * (1 - u);
    return f;
}

void UnitMotion::apply_offset(Vec3 d) {
    set_translation(start_, translation(start_) + d);
    set_translation(end_, translation(end_) + d);
    set_translation(out_, translation(out_) + d);
}

} // namespace parkan
