#include "ai/wizard.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ai {

namespace {

Vec3 column(const Mat4& m, int c) { return {m.at(0, c), m.at(1, c), m.at(2, c)}; }
float& comp(Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// HermiteCubic::Init: p(τ) = p0 + v0·τ + c2·τ² + c3·τ³ with p(T) = p1, p'(T) = v1; zero for T <= 1e-4.
void hermite(Vec3 p0, Vec3 p1, Vec3 v0, Vec3 v1, float T, Vec3& c2, Vec3& c3) {
    c2 = c3 = {};
    if (!(T > 0.0001f)) return;
    const Vec3 d = p1 - v0 * T - p0, dv = (v1 - v0) * T;
    c2 = (d * 3 - dv) * (1 / (T * T));
    c3 = (dv - d * 2) * (1 / (T * T * T));
}

} // namespace

void Wizard::clear_points() {
    target_.time = tick_time_;
    route_.clear();
    has_destination_ = false;
}

void Wizard::set_destination(const WizardPoint& p) {
    const Vec3 d = destination_.pos - p.pos;
    if (has_destination_ && !(std::sqrt(dot(d, d)) > 0.5f)) return;
    has_destination_ = true;
    destination_ = p;
}

std::optional<WizardOutput> Wizard::takt(std::uint32_t now, float step_ms, const Mat4& placement,
                                         const std::array<float, 27>& body) {
    if (disabled || suspended) return std::nullopt;
    body_ = body;
    look_ahead_ = 0.001f * step_ms;
    placement_ = placement;
    if (!ai_mode) return std::nullopt;
    // The current segment would end inside this step: start the next one.
    if (target_.time < now + std::uint32_t(std::lround(step_ms))) begin_segment(now);
    evaluate_segment(now);
    if (!output_enabled) return std::nullopt;
    return output_to_control();
}

void Wizard::begin_segment(std::uint32_t now) {
    if (suspended) return;
    p0_ = column(placement_, 3);
    d0_ = column(placement_, 1);
    v0_ = target_.vel; // the old target velocity
    start_flags_ = target_.flags;
    t0_ = now;
    // Drop the point just reached and every point whose time has passed (corner cutting).
    while (!route_.empty()) {
        if (target_.time == route_.front().time) {
            route_.pop_front();
            if (route_.empty()) break;
        }
        target_ = route_.front();
        if (now < route_.front().time) break;
    }
    if (route_.empty()) {
        if (has_destination_) {
            target_ = destination_; // its own time, unless that has passed
            if (!(now < target_.time)) target_.time = now + 1000;
        } else { // hold here
            target_ = {p0_, {}, d0_, now + 1000, start_flags_ | 0x3331};
        }
    }
    seg_len_ = float(target_.time - t0_) * 0.001f;
    hermite(p0_, target_.pos, v0_, target_.vel, seg_len_, c2_, c3_);
    hermite(p0_, target_.pos, d0_, target_.heading, seg_len_, hc2_, hc3_);
    evaluate_segment(now);
}

void Wizard::evaluate_segment(std::uint32_t now) {
    const float tau = float(now - t0_) * 0.001f; // unsigned difference, as the original
    eval_pos_ = ((c3_ * tau + c2_) * tau + v0_) * tau + p0_;
    const float tv = tau + look_ahead_ * 0.5f; // velocity at mid-step
    eval_vel_ = (c3_ * (3 * tv) + c2_ * 2) * tv + v0_;
    const float th = tau + look_ahead_; // heading tangent one step ahead
    if (target_.flags & 1) {
        const float u = th / seg_len_;
        eval_heading_ = target_.heading * u + d0_ * (1 - u);
    } else {
        eval_heading_ = (hc3_ * (3 * th) + hc2_ * 2) * th + d0_;
    }
    active_flags_ = target_.flags;
}

float Wizard::compute_turn_command() const {
    Vec3 f = column(placement_, 1);
    f.z = 0;
    const float hl = std::sqrt(dot(eval_heading_, eval_heading_));
    if (!(hl > 0)) return 0; // the original divides by zero here (NaN); never seen with walker points
    Vec3 h = eval_heading_ * (1 / hl);
    h.z = 0;
    const float lf = std::sqrt(dot(f, f)), lh = std::sqrt(dot(h, h));
    if (lf < 1e-9f || lh < 1e-9f || (f.x == h.x && f.y == h.y && f.z == h.z)) return 0;
    const float ang = std::acos(std::clamp(dot(f, h) / (lf * lh), -1.f, 1.f));
    const float c = cross(f, h).z; // + = the heading is to the left (counter-clockwise)
    const float s = c == 0 ? 0.f : c > 0 ? 1.f : -1.f;
    const float rate = body_[11];
    if (rate < 0.001f) return s;
    const float k = ang / (look_ahead_ * rate); // close the error in one motion step
    return k <= 1 ? k * s : s;
}

WizardOutput Wizard::output_to_control() const {
    WizardOutput out;
    out.yaw = compute_turn_command();
    // FastProc +0x8c InverseRotate: world → body.
    Vec3 v{dot(column(placement_, 0), eval_vel_), dot(column(placement_, 1), eval_vel_), dot(column(placement_, 2), eval_vel_)};
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t nib = (active_flags_ >> (4 + 4 * i)) & 0xf;
        float& x = comp(v, i);
        if ((nib & 1) && (nib & 2)) {
            x = 0;
            continue;
        }
        if (nib & 1) {
            if (x < 0) x = 0;
            if (nib & 4) x = body_[6 + i];
        }
        if (nib & 2) {
            if (x > 0) x = 0;
            if (nib & 4) x = -body_[6 + i];
        }
    }
    out.linear = v;
    return out;
}

} // namespace parkan::ai
