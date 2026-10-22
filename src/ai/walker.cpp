#include "ai/walker.hpp"

#include "ai/behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ai {

namespace {
// BehConstants defaults (BehConstants::SetDefaults 10016250) and fixed walker constants.
constexpr float kMovementSpeedPercent = 1, kMovementMinSpeedPercent = 1, kMovementMaxSpeed = 600;
constexpr float kSpeedMaximumFactor = 1; // BehDifficulty Speed_MaximumFactor (no sheet loaded: 1)
constexpr std::size_t kMinPointInTrajectory = 3; // PathFind_MinPointInTrajectory
constexpr float kOffRouteTolerance = 10;         // 2.5 m with hallway locks (not modelled)
} // namespace

Walker::Walker(const LandMap& map, const ArealGraph& graph, RandomSource rand)
    : map_(&map), graph_(&graph), rand_(std::move(rand)) {}

void Walker::clear() {
    corridor_.clear();
    legs_.clear();
    points_.clear();
}

bool Walker::set_target(Vec3 target, float speed) {
    // W+0x164 (a building re-target pending) is never set without buildings: always plan.
    if (tick_timer) tick_timer->reset();
    hold_request_ = true;
    clear();
    target_ = target;
    const auto a = map_->areal_at(target.x, target.y); // "Target is out of map" is only logged
    if (!a || (!map_->areals[*a].walkable && !can_fly)) return false;
    cruise_ = speed;
    cruise_ = std::min(cruise_, max_speed * kMovementSpeedPercent * kSpeedMaximumFactor);
    cruise_ = std::min(cruise_, kMovementMaxSpeed);
    cruise_ = std::max(cruise_, min_speed * kMovementMinSpeedPercent);
    if (cruise_ < 2.0f) cruise_ = 2.0f;
    return plan_global_path();
}

void Walker::stop() {
    if (tick_timer) tick_timer->reset();
    hold_request_ = true;
    clear();
}

void Walker::retry_if_stuck() {
    if (off_route_ > 0) {
        set_target(target_, cruise_);
        off_route_ = 0;
        ++replans_;
    }
}

bool Walker::plan_global_path() {
    const UnitState st = unit_state();
    const auto from = map_->areal_at(st.pos.x, st.pos.y), to = map_->areal_at(target_.x, target_.y);
    route_.clear();
    if (!from || !to) return false;
    route_ = search_path(*graph_, *from, *to, can_fly, rand_);
    if (route_.empty()) return false;
    for (const CorridorPoint& c : build_corridor(*map_, route_, st.pos, target_)) corridor_.push_back(c);
    return true;
}

bool Walker::refine_corridor_point() {
    if (corridor_.empty()) return false;
    legs_.push_back(corridor_.front()); // obstacle detours not modelled
    corridor_.pop_front();
    return true;
}

void Walker::set_point_altitude(Vec3& p, float fallback_z) {
    if (!ground_height) {
        p.z = fallback_z;
        return;
    }
    // 15 m (constant 10059974); animals 30 + r01·50.
    p.z = ground_height(p.x, p.y) + (animal ? 30 + r01(rand_) * 50 : 15.0f);
}

bool Walker::emit_trajectory_points(const TrajectoryPoint& prev, float end_v, float v0) {
    if (legs_.empty()) return false;
    const CorridorPoint leg = legs_.front();
    legs_.pop_front();
    if (leg.stop) end_v = 0;
    // Ground legs are never subdivided (round(len)/7777); flyers every 20 m (not modelled).
    Vec3 p = leg.pos;
    set_point_altitude(p, prev.pos.z);
    const Vec3 d = p - prev.pos;
    const float len = std::sqrt(dot(d, d));
    if (len <= 0.01f) return true; // logged and skipped
    const Vec3 dir = d * (1 / len);
    const float vsum = std::max(v0 + end_v, 2.0f);
    const float t = std::max(2 * len / vsum, 0.2f); // trapezoid: the mean of start and end speed
    TrajectoryPoint pt;
    pt.pos = p;
    pt.vel = dir * end_v;
    pt.heading = end_v > 0 ? dir * end_v : dir;
    pt.time = prev.time + std::uint32_t(std::lround(t * 1000));
    pt.areal = leg.areal;
    points_.push_back(pt);
    if (end_v < 0.1f) { // hold at the end
        TrajectoryPoint h = pt;
        h.time = pt.time + 500;
        points_.push_back(h);
        h.time = pt.time + 700;
        points_.push_back(h);
    }
    return true;
}

void Walker::on_behaviour_takt(Wizard& wz) {
    if (hold_request_) {
        if (wz.point_count() > 0) wz.clear_points();
        send_hold_position(wz);
        hold_request_ = false;
    }
    sync_with_wizard(wz);
    const UnitState st = unit_state();
    if (!map_->areal_at(st.pos.x, st.pos.y)) return; // "is out of ArealMap": nothing planned or pushed
    bool done = false;
    for (int i = 0; points_.size() < kMinPointInTrajectory && !done && i <= 5; ++i) {
        TrajectoryPoint prev;
        if (points_.empty()) prev.pos = st.pos, prev.vel = st.vel, prev.heading = st.dir, prev.time = st.time;
        else prev = points_.back();
        float end_v = cruise_;
        if (corridor_.empty() && legs_.size() == 1 && !animal) end_v = 0; // stop at the end
        if (!emit_trajectory_points(prev, end_v, cruise_) && !refine_corridor_point()) done = true;
    }
    push_points_to_wizard(wz);
}

void Walker::push_points_to_wizard(Wizard& wz) {
    const std::size_t n = wz.point_count();
    if (n == 0 && idle()) {
        send_hold_position(wz);
        return;
    }
    if (points_.size() > 300) return; // "too many points"
    for (std::size_t i = n; i < points_.size(); ++i) {
        const TrajectoryPoint& p = points_[i];
        WizardPoint wp{p.pos, p.vel, p.heading, p.time, p.linear_heading ? 1u : 0u};
        if (walk_chassis || !can_fly) wp.flags |= 0x3030; // body x and z forced to 0
        wz.add_point(wp);
    }
}

void Walker::send_hold_position(Wizard& wz) {
    const UnitState s = unit_state();
    wz.set_destination({s.pos + s.vel * 0.5f, {}, s.dir, s.time + 1000, (walk_chassis || !can_fly) ? 0x3031u : 0x0001u});
}

void Walker::sync_with_wizard(Wizard& wz) {
    const std::size_t n = wz.point_count();
    if (n >= points_.size()) return;
    const std::size_t k = points_.size() - n; // the wizard consumed k points
    last_reached_ = points_[k - 1];
    points_.erase(points_.begin(), points_.begin() + std::ptrdiff_t(k));
    if (dist2d(unit_state().pos, last_reached_.pos) <= kOffRouteTolerance) {
        off_route_ = 0;
        return;
    }
    if (++off_route_ <= 1) return;
    // Twice in a row off the route: clear, hold, re-plan.
    off_route_ = 0;
    clear();
    if (wz.point_count() > 0) wz.clear_points();
    send_hold_position(wz);
    hold_request_ = false;
    ++replans_;
    set_target(target_, cruise_);
    on_behaviour_takt(wz);
}

} // namespace parkan::ai
