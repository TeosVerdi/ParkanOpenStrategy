#include "ai/behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ai {

namespace {

// BehConstants defaults (SetDefaults 10016250).
constexpr float kPatrolPlaceSpeedPercent = 0.8f, kPatrolPlaceRadius = 60;
constexpr float kPatrolPlaceChangeMin = 20, kPatrolPlaceChangeRandom = 10;
constexpr float kGoArrival = 30;    // M_Task_Go: a place is reached within 30 m (2D) once the walker is idle
constexpr float kLoneGoPatrol = 150; // the patrol radius a lone finished Go order leaves behind

// M_Task_Stop / M_Task_StayGround: hold position (fire mode {2, −1, 0.5} not modelled).
struct StopTask : Task {
    using Task::Task;
    bool takt(Behaviour& b, const UnitState&) override {
        b.walker.stop();
        return true;
    }
};

// M_Task_Go (type 2) to a place.
struct GoTask : Task {
    using Task::Task;
    float initial = 1; // +0x58
    bool start(Behaviour& b, const UnitState& st) override {
        if (order.target_kind != kTargetPlace) return false; // "go inside Building": NOT IMPLEMENTED
        initial = dist2d(st.pos, order.place) + 1.0f;
        return b.walker.set_target(order.place, b.go_speed()); // "Target is unreachable... Fail GoOrder"
    }
    bool takt(Behaviour& b, const UnitState& st) override {
        progress = std::sqrt(dot(st.pos - order.place, st.pos - order.place)) / initial;
        if (b.walker.idle()) {
            if (dist2d(st.pos, order.place) < kGoArrival) { // "We are staying, task over"
                if (b.order_count() == 1) b.give_order({kOrderPatrol, kLoneGoPatrol, kTargetPlace, order.place}, 1);
                return false;
            }
            if (!b.walker.set_target(order.place, b.go_speed())) return false; // re-plan failed
        }
        return true;
    }
};

// M_Task_Migrate (0xf): random points in the clan's migration areals.
struct MigrateTask : Task {
    using Task::Task;
    MTimer area_timer, point_timer; // +0x58, +0x64
    int area = -1;                  // +0x60
    void go_to_random_point(Behaviour& b) { // 1002cba0
        if (area < 0 || !b.migration || std::size_t(area) >= b.migration->areals.size()) return;
        const auto& m = b.migration->areals[std::size_t(area)];
        for (int i = 0; i < 50; ++i) {
            const float rx = std::max(r01(b.rand), 0.2f), ry = std::max(r01(b.rand), 0.2f);
            if (b.walker.set_target({m[0] + rx * m[3], m[1] + ry * m[3], m[2]}, b.go_speed())) return; // MTask::GoTo
        }
    }
    int pick(Behaviour& b) { return b.migration ? b.migration->pick(b.now, b.rand) : -1; }
    bool start(Behaviour& b, const UnitState&) override {
        area_timer.set_period(60, 120);
        area = pick(b);
        area_timer.rearm(b.now, b.rand);
        go_to_random_point(b);
        point_timer.set_period(5, 10);
        point_timer.rearm(b.now, b.rand);
        return true;
    }
    bool takt(Behaviour& b, const UnitState&) override {
        if (area_timer.check_and_rearm(b.now, b.rand)) {
            area = pick(b);
            area_timer.rearm(b.now, b.rand);
            go_to_random_point(b);
            point_timer.rearm(b.now, b.rand);
            return true;
        }
        if (!b.walker.idle()) point_timer.rearm(b.now, b.rand); // the 5–15 s count starts when it stops
        else if (point_timer.check_and_rearm(b.now, b.rand)) go_to_random_point(b);
        return true; // never ends by itself
    }
};

// M_Task_Patrol (4) around a place. The ring builder (1002dd90) is not decoded: inferred as 24 points
// on a circle of radius `param` at a random phase, rebuilt on the ChangeTrajectory timer; the next
// point is taken when the walker is idle.
struct PatrolTask : Task {
    using Task::Task;
    std::vector<Vec3> ring;
    std::size_t next = 0;
    MTimer ring_timer;
    void build_ring(Behaviour& b) {
        const float r = order.param > 0 ? order.param : kPatrolPlaceRadius, phase = 6.2831853f * r01(b.rand);
        ring.clear();
        for (int i = 0; i < 24; ++i) {
            const float a = phase + 6.2831853f * float(i) / 24;
            ring.push_back(order.place + Vec3{r * std::cos(a), r * std::sin(a), 0});
        }
        next = 0;
    }
    void go_next(Behaviour& b) {
        for (std::size_t tries = 0; tries < ring.size(); ++tries) {
            const Vec3 p = ring[next];
            next = (next + 1) % ring.size();
            if (b.walker.set_target(p, b.go_speed() * kPatrolPlaceSpeedPercent)) return;
        }
    }
    bool start(Behaviour& b, const UnitState&) override {
        if (order.target_kind != kTargetPlace) return false; // building / unit patrols: NOT IMPLEMENTED
        ring_timer.set_period(kPatrolPlaceChangeMin, kPatrolPlaceChangeRandom);
        ring_timer.rearm(b.now, b.rand);
        build_ring(b);
        go_next(b);
        return true;
    }
    bool takt(Behaviour& b, const UnitState&) override {
        if (ring_timer.check_and_rearm(b.now, b.rand)) build_ring(b);
        if (b.walker.idle()) go_next(b);
        return true;
    }
};

// Orders whose tasks are not ported: they cannot start, so the stack drops them.
struct NotImplementedTask : Task {
    using Task::Task;
    bool start(Behaviour&, const UnitState&) override { return false; }
    bool takt(Behaviour&, const UnitState&) override { return false; }
};

std::unique_ptr<Task> make_task(const Order& o) { // MTaskStack::CreateTaskFromOrder (10033a80), reduced
    switch (o.type) {
    case kOrderStop:
    case kOrderStayGround: return std::make_unique<StopTask>(o);
    case kOrderGo: return std::make_unique<GoTask>(o);
    case kOrderPatrol: return std::make_unique<PatrolTask>(o);
    case kOrderMigrate: return std::make_unique<MigrateTask>(o);
    default: return std::make_unique<NotImplementedTask>(o);
    }
}

} // namespace

void MTimer::set_period(float min_s, float rand_s) {
    base = std::uint16_t(std::lround(min_s * 1000) >> 6);
    jitter = std::uint16_t(std::lround(rand_s * 1000) >> 6);
    next = 0;
}

void MTimer::rearm(std::uint32_t now, const RandomSource& rand) {
    next = now + std::uint32_t(base) * 64 + ((std::uint32_t(rand() & 0xff) * jitter * 64) >> 8);
}

bool MTimer::check_and_rearm(std::uint32_t now, const RandomSource& rand) {
    if (next != 0 && now <= next) return false;
    rearm(now, rand);
    return true;
}

int ClanMigration::pick(std::uint32_t now, const RandomSource& rand) {
    if (areals.empty()) return -1;
    if (current == -1 || timer.check_and_rearm(now, rand)) {
        timer.rearm(now, rand);
        current = int(std::uint32_t(rand()) % areals.size());
    }
    return current;
}

Behaviour::Behaviour(Walker& w, RandomSource r)
    : walker(w), rand(std::move(r)), default_stop_(std::make_unique<StopTask>(Order{kOrderStop})) {
    walker.tick_timer = &task_timer;
}

void Behaviour::give_order(const Order& o, int mode) {
    auto t = make_task(o);
    if (mode == 3) tasks_.clear();
    if (mode == 2) tasks_.push_front(std::move(t));
    else tasks_.push_back(std::move(t));
    if (mode != 1) task_timer.reset(); // the new current task is looked at on the next tick (inferred)
}

float Behaviour::task_takt(const UnitState& st) {
    if (!tasks_.empty() && tasks_.front()->started) {
        Task& cur = *tasks_.front();
        if (cur.takt(*this, st)) return cur.progress; // CheckLimits (time limits) not modelled
        tasks_.pop_front(); // "Task Ended"; report order done
    } else if (tasks_.empty()) {
        default_stop_->takt(*this, st);
        return 0;
    }
    while (!tasks_.empty()) { // StartCurrentTask
        if (tasks_.front()->start(*this, st)) {
            tasks_.front()->started = true;
            return 0;
        }
        tasks_.pop_front(); // "Cannot StartTask"
    }
    return 0;
}

void Behaviour::movement_takt(std::uint32_t t, Wizard& wz) {
    now = t;
    if (!task_timer.check_and_rearm(now, rand)) {
        if (refresh_timer.check_and_rearm(now, rand)) walker.retry_if_stuck(); // EscapeObstacle: not modelled
        return;
    }
    // Decision layer (timer +0x958: CheckRepair / CheckTargets) not modelled.
    if (walker.max_speed < 0.5f || walker.max_speed < walker.min_speed) {
        if (can_move) walker.stop(); // StopAndHold
        can_move = false;
        return;
    }
    can_move = true;
    const UnitState st = walker.unit_state();
    walker.sync_with_wizard(wz);
    progress_ = task_takt(st);
    walker.retry_if_stuck();
    walker.on_behaviour_takt(wz);
}

} // namespace parkan::ai
