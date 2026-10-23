#include "ai/behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ai {

namespace {

// BehConstants defaults (SetDefaults 10016250).
constexpr float kPatrolPlaceSpeedPercent = 0.8f, kPatrolPlaceRadius = 60;
constexpr float kPatrolPlaceChangeMin = 20, kPatrolPlaceChangeRandom = 10;
constexpr float kAttackMinFightDistance = 30, kAttackDelFightDistance = 20, kAttackLeftRightRange = 80;
constexpr float kAttackMaxFireDistance = 200;
constexpr float kAttackMinAttackSpeedPercent = 0.7f, kAttackDelAttackSpeedPercent = 0.3f;
constexpr float kAttackNearingChangeMin = 4, kAttackNearingChangeRandom = 4;
constexpr float kAttackFightingChangeMin = 4, kAttackFightingChangeRandom = 4;
constexpr float kGoArrival = 30;     // M_Task_Go: a place is reached within 30 m (2D) once the walker is idle
constexpr float kLoneGoPatrol = 150; // the patrol radius a lone finished Go order leaves behind
constexpr float kTwoPi = 6.2831855f; // 10059a48

Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0 ? v * (1.0f / l) : v;
}
float dist3d(Vec3 a, Vec3 b) { return std::sqrt(dot(a - b, a - b)); }

// M_Task_Stop (10031db0 / 10031e10): fire at will, hold position. The original Takt returns 0 (an
// explicit Stop order ends after one tick and the embedded default Stop takes over, doing the same);
// the port keeps the Stop task current instead (deliberate: current_order() stays 1).
struct StopTask : Task {
    using Task::Task;
    bool start(Behaviour& b, const UnitState&) override {
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        b.walker.stop();
        return true;
    }
    bool takt(Behaviour& b, const UnitState&) override {
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        b.walker.stop();
        return true;
    }
};

// M_Task_StayGround (0x15): as Stop, but nothing interrupts it (slot 12 = 10031c80, returns 0).
struct StayGroundTask : StopTask {
    using StopTask::StopTask;
    float interrupt_desire(Behaviour&, const DecisionEvent&, TaskLimits&, const UnitState&) override { return 0; }
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
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
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
    // 1002b390: a moving unit is not distracted by enemies (events 0, 1, 2, 5).
    float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st) override {
        if (e.type <= 2 || e.type == 5) return 0;
        return Task::interrupt_desire(b, e, l, st);
    }
};

// M_Task_Attack (type 3; 0x7c bytes). Target kind 0x202 orders become M_Task_Go (CreateTaskFromOrder).
struct AttackTask : Task {
    using Task::Task;
    std::uint32_t target = kNoObject;      // +0x58
    bool via_building = false;             // +0x5c: going through a hallway (never set: no buildings)
    std::uint32_t fire_target = kNoObject; // +0x60
    bool nearing = true;                   // +0x64: 1 nearing (≥ 200 m), 0 fighting
    MTimer fight_timer, near_timer;        // +0x68, +0x70
    std::uint32_t fight_since = 0;         // +0x78: when the fighting phase began (0 = not yet)

    // 10026d40.
    bool set_target(Behaviour& b) override {
        target = fire_target = kNoObject;
        if (order.target_kind == kTargetLogicId) {
            target = fire_target = order.target_id;
            const auto o = b.object(target);
            if (o && target != b.self.logic_id && o->clan != b.self.clan && o->clan != kNoClan) {
                nearing = true;
                return true;
            }
            return false; // "Clan": an own-clan (or clanless) target is refused
        }
        if (order.target_kind == kTargetPlace) { // "ByPlace... Nearest" (interrupt event 5)
            target = fire_target = b.nearest_enemy(order.place);
            return true;
        }
        return order.target_kind == kTargetNone; // "NotDefined": StartTask picks
    }

    // 10026fd0.
    bool start(Behaviour& b, const UnitState& st) override {
        if (origin != 1 && !b.self.has_weapons) return false;
        if (b.self.animal()) {
            fight_timer.set_period(5, 5);
            near_timer.set_period(kAttackNearingChangeMin, kAttackNearingChangeRandom);
        } else {
            fight_timer.set_period(2 * kAttackFightingChangeMin, 2 * kAttackFightingChangeRandom);
            near_timer.set_period(2 * kAttackNearingChangeMin, 2 * kAttackNearingChangeRandom);
        }
        if (target == kNoObject) {
            target = fire_target = b.nearest_enemy(st.pos);
            if (target == kNoObject) return false; // "CANCEL TASK ... with -1 target"
        }
        b.set_fire_mode(kFireTarget, target, 0.5f);
        fight_since = 0;
        via_building = false;
        return make_go(b, st);
    }

    // 10027250.
    bool takt(Behaviour& b, const UnitState& st) override {
        const Vec3 tpos = b.object_pos(target);
        if (target == kNoObject || !b.alive(target) || (tpos.x == 0 && tpos.y == 0 && tpos.z == 0)) return false;
        if (nearing) b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        else b.set_fire_mode(kFireTarget, fire_target, 0.5f);
        // While fighting, every > 10 s (and unless the last shot fell in the first 10 s of the fight):
        // a dodge point within ±27.5 m of the target.
        if (!nearing && fight_since != 0 && !via_building && b.now - fight_since > 10000 &&
            b.last_fire - fight_since > 10000) {
            const float speed = b.walker.max_speed; // MB+0x5fc · Go_SpeedPercent
            for (int i = 0; i < 25; ++i) {
                const float rx = r01(b.rand), ry = r01(b.rand);
                if (b.walker.set_target({(rx - 0.5f) * 55 + tpos.x, (ry - 0.5f) * 55 + tpos.y, 0}, speed)) break;
            }
            fight_since = b.now;
            fight_timer.rearm(b.now, b.rand);
            near_timer.rearm(b.now, b.rand);
        }
        // Slot 17 (10028310): the timer of the current phase.
        if (!(nearing ? near_timer : fight_timer).check_and_rearm(b.now, b.rand)) {
            if (!(target & kBuildingBit)) {
                const bool no_route = b.walker.corridor().empty() && b.walker.legs().empty();
                if (!no_route || b.walker.points().size() >= 2) return true;
            } else if (!b.walker.idle() && dist2d(st.pos, b.walker.target()) >= 5) {
                return true;
            }
        }
        return make_go(b, st);
    }

    // Slot 16 (10027f80), unit target: a point `lo + U(0, del)` short of the target on the line from
    // the unit, moved sideways by U(−lr, lr). Animals charge the target itself 30 % of the time.
    Vec3 stand_point(Behaviour& b, Vec3 tpos, Vec3 own, float lo, float del, float lr) const {
        if (via_building) return tpos;
        const Vec3 dir = normalized(tpos - own);
        const Vec3 base = tpos - dir * lo;
        const Vec3 perp = normalized({dir.y, -dir.x, 0});
        if (b.self.animal() && r01(b.rand) < 0.3f) return tpos;
        const float lat = (r01(b.rand) * 2 - 1) * lr;
        const float k = r01(b.rand) * del;
        return base - dir * k + perp * lat;
    }

    // Slot 15 MakeGoCommand (10027580).
    bool make_go(Behaviour& b, const UnitState& st) {
        const auto o = b.object(target);
        if (target == kNoObject || !b.alive(target) || !o) return false; // "CANCEL TASK: MakeGoCommand"
        if (o->clan == b.self.clan) return false;
        float lo = kAttackMinFightDistance, hi = lo + kAttackDelFightDistance;
        if (target & kBuildingBit) {
            // A random point on a ring [r − 20, r − 5] around the building's bounding sphere.
            const float a_lo = o->radius - 20, a_hi = o->radius - 5;
            for (int i = 0; i < 77; ++i) {
                const float ang = r01(b.rand) * kTwoPi;
                const float x = ((a_hi - a_lo) * r01(b.rand) + a_lo) * std::cos(ang) + o->pos.x;
                const float y = ((a_hi - a_lo) * r01(b.rand) + a_lo) * std::sin(ang) + o->pos.y;
                if (b.walker.set_target({x, y, 0}, b.walker.max_speed)) {
                    nearing = false;
                    fight_since = 0;
                    near_timer.rearm(b.now, b.rand);
                    return true;
                }
            }
            return false;
        }
        // 1001cc50: the turret weapons' optimal range +0x48 gives [0.05, 0.1]·max; outside [50, 100] the
        // band is [50, 100], so in practice it always is.
        for (const Weapon& w : b.fire.weapons)
            if (w.group >= 0 && hi < w.opt) hi = w.opt * 0.1f, lo = w.opt * 0.05f;
        if (lo < 50 || hi > 100) lo = 50, hi = 100;
        const Vec3 tpos = b.object_pos(target);
        const Vec3 stand = stand_point(b, tpos, st.pos, lo, hi - lo, kAttackLeftRightRange);
        float speed;
        if (dist2d(tpos, st.pos) >= kAttackMaxFireDistance) { // nearing at Go speed
            speed = b.walker.max_speed;
            nearing = true;
            fight_since = 0;
            near_timer.rearm(b.now, b.rand);
        } else { // fighting at (0.7 + 0.3·r01) of the maximum speed
            const float r = r01(b.rand);
            speed = b.walker.max_speed * kAttackDelAttackSpeedPercent * r + b.walker.max_speed * kAttackMinAttackSpeedPercent;
            if (nearing) fight_since = b.now;
            nearing = false;
            fight_timer.rearm(b.now, b.rand);
        }
        // The target is not on a hallway vertex (no buildings): fire at it and go to the stand point.
        via_building = false;
        fire_target = target;
        b.set_fire_mode(kFireTarget, target, 0.5f);
        if (!b.walker.set_target(stand, speed)) {
            near_timer.reset();
            fight_timer.reset();
        }
        return true;
    }

    // 10026b10: an ordered attack is never re-targeted; an interrupt attack switches to an enemy that
    // scores higher than the current target (× 1.2 for a seen enemy).
    float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st) override {
        if (e.type == 2 || e.type == 5) return 0;
        if (e.type > 1) return Task::interrupt_desire(b, e, l, st);
        if (origin == 1) return 0;
        const auto fresh = b.object(e.lid), cur = b.object(target);
        const float s_new = fresh ? target_score(b, st.pos, *fresh) : 0;
        float s_cur = cur ? target_score(b, st.pos, *cur) : 0;
        if (e.type == 0) s_cur *= 1.2f;
        return s_cur < s_new ? 1.0f : 0.0f;
    }
    // 10026cd0.
    float target_score(const Behaviour&, Vec3 own, const SeenObject& enemy) const override {
        const float d = dist2d(own, enemy.pos);
        return d < 700 ? 1000 / (d + 10) : 0;
    }
};

// M_Task_Follow (0x16; 0x70 bytes): stay near a leader of the own clan.
struct FollowTask : Task {
    using Task::Task;
    std::uint32_t leader = kNoObject; // +0x58
    float distance = 20;              // +0x5c
    MTimer check{0, 78, 15};          // +0x60: MTimer(5, 1)
    MTimer move{0, 109, 31};          // +0x68: MTimer(7, 2)
    // 1002ad80: the distance is the order's param when 21..29, else 20; only a LogicID target.
    bool set_target(Behaviour&) override {
        const auto p = std::uint32_t(order.param);
        distance = (p < 0x15 || p > 0x1d) ? 20.f : float(p);
        if (order.target_kind != kTargetLogicId) return false;
        leader = order.target_id;
        return true;
    }
    bool start(Behaviour&, const UnitState&) override { // 1002ae00
        move.reset();
        check.reset();
        return true;
    }
    // 1002ae20.
    bool takt(Behaviour& b, const UnitState& st) override {
        const auto o = b.object(leader);
        if (!o || o->clan != b.self.clan) return false;
        if (b.walker.idle() || check.check_and_rearm(b.now, b.rand)) {
            const Vec3 lpos = b.object_pos(leader);
            if (dist2d(lpos, st.pos) <= distance + 20 && std::abs(lpos.z - st.pos.z) <= distance + 10) {
                move.reset(); // close enough (and not on a hallway: always, no buildings)
            } else if (move.check_and_rearm(b.now, b.rand)) {
                for (int i = 0; i < 77; ++i) {
                    const float rx = (r01(b.rand) - 0.5f) * distance, ry = (r01(b.rand) - 0.5f) * distance;
                    if (b.walker.set_target({rx + rx + lpos.x, ry + ry + lpos.y, lpos.z + 5}, b.walker.max_speed)) {
                        move.rearm(b.now, b.rand);
                        return true; // (no fire-mode request on this path)
                    }
                }
            }
        }
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        return true;
    }
    // 1002acd0: break off to fight, but stay within 2·distance + 20 of the leader.
    float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st) override {
        if (e.type <= 2 || e.type == 5) {
            if (!b.self.has_weapons) return 0;
            l.unit = leader;
            l.unit_radius = distance + distance + 20;
            return 1;
        }
        return Task::interrupt_desire(b, e, l, st);
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
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
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
    // 1002c640: an animal defends its areal {x, y, z, inner, outer}: enemies inside it are attacked,
    // with a time limit and a place limit around the areal.
    float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st) override {
        if (area >= 0 && b.migration && std::size_t(area) < b.migration->areals.size()) {
            const auto& m = b.migration->areals[std::size_t(area)];
            const Vec3 c{m[0], m[1], m[2]};
            const float d_own = dist2d(st.pos, c), d_en = dist2d(c, b.object_pos(e.lid));
            const auto limit = [&](float time, float extra) {
                l.time = time;
                l.place = c;
                l.place_radius = m[4] + extra;
                return 1.0f;
            };
            if (e.type == 0 || e.type == 2 || e.type == 5) {
                if (m[4] <= d_own || m[4] < d_en) return 0;
                if (d_en < m[3]) return limit(20, 80);
                if (d_en < m[4]) return limit(10, 20);
            } else if (e.type == 1) {
                if (m[4] <= d_own) {
                    l.time = 10;
                    return 1;
                }
                if (m[4] < d_en) return limit(20, 20);
                if (d_en < m[3]) return limit(35, 100);
                if (d_en < m[4]) return limit(25, 80);
            }
        }
        return Task::interrupt_desire(b, e, l, st);
    }
};

// M_Task_Patrol (4) around a place. The ring builder (1002dd90) is not decoded: inferred as 24 points
// on a circle of radius `param` at a random phase, rebuilt on the ChangeTrajectory timer; the next
// point is taken when the walker is idle. The fire mode request is inferred ({2, −1, 0.5} like Go).
struct PatrolTask : Task {
    using Task::Task;
    std::vector<Vec3> ring;
    std::size_t next = 0;
    MTimer ring_timer;
    float radius() const { return order.param > 0 ? order.param : kPatrolPlaceRadius; } // +0x70
    void build_ring(Behaviour& b) {
        const float r = radius(), phase = kTwoPi * r01(b.rand);
        ring.clear();
        for (int i = 0; i < 24; ++i) {
            const float a = phase + kTwoPi * float(i) / 24;
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
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        if (ring_timer.check_and_rearm(b.now, b.rand)) build_ring(b);
        if (b.walker.idle()) go_next(b);
        return true;
    }
    // 1002d250 (place patrol): fight within radius + 60 of the patrol centre.
    float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st) override {
        if (e.type <= 2 || e.type == 5) {
            l.place = order.place;
            l.place_radius = radius() + 60;
            return 1;
        }
        return Task::interrupt_desire(b, e, l, st);
    }
    // 1002d390: only enemies within the patrol radius of the centre count (distance from the centre).
    float target_score(const Behaviour& b, Vec3, const SeenObject& enemy) const override {
        const float d = dist2d(order.place, enemy.pos);
        if (d >= radius()) return 0;
        return Task::target_score(b, order.place, enemy);
    }
};

// M_Task_Search (order 0x11 "захват", 0x80 bytes) in its capture mode: a small warbot walks into a
// building of another clan; the building's capture door does the capture (Terrain 100335d0) and the
// task ends when the building's clan is the unit's ("] captured", 10030474).
// SetTarget (10030110): by LogicID (0x201) — only for size class < 3 (property 0x201) and a building of
// another clan that is not a teleport (type 0x80000200), else refused; by type mask (0x203) 0x80000000 —
// any building (size class < 3); mask 0x10000000 (minerals, the explorer's search) and 0x204 are other
// modes of the class, not ported (refused).
// OnBehaviourTakt (10030300): with a target, the task goes on while the target exists with another clan;
// otherwise a given target ends it, a searched one is replaced (slot 0x3c). While the walker moves it
// waits; when the walker is idle the first time it sets +0x7c and re-issues the go command (slot 0x3c,
// MakeGoCommand: the building's hallway to an entry vertex — port: the capture volume's centre,
// SeenObject::entry); later idle ticks re-issue it as long as the building is not yet the unit's.
// Port choices: the searched target is the nearest non-own, non-teleport building with a capture
// volume within 3000 m (inferred: slot 0x3c's choice is not decoded); the fire mode is StartTask's
// {2 at will, −1, 0.5}.
struct CaptureTask : Task {
    using Task::Task;
    bool capture = false;             // +0x5c
    bool given = false;               // +0x60: a target given by the order
    std::uint32_t target = kNoObject; // +0x6c
    bool arrived = false;             // +0x7c

    bool set_target(Behaviour& b) override {
        if (order.target_kind == kTargetBuilding) {
            if (order.target_id == kNoObject || !(order.target_id & kBuildingBit) || b.self.size_class >= 3) return false;
            capture = true;
            const auto o = b.object(order.target_id);
            if (!o || o->clan == b.self.clan || o->type == kTypeTeleport) return false;
            target = order.target_id, given = true;
            return true;
        }
        if (order.target_kind == kTargetMask && (order.mask & kBuildingBit) && !(order.mask & 0x10000000) && b.self.size_class < 3) {
            capture = true, given = false;
            return true;
        }
        return false; // the explorer's mineral search / no target: NOT IMPLEMENTED
    }
    std::uint32_t pick(const Behaviour& b, Vec3 pos) const {
        if (!b.world) return kNoObject;
        std::vector<std::uint32_t> near;
        b.world->objects_near(pos, 3000, near);
        float best = 1e10f;
        std::uint32_t lid = kNoObject;
        for (const std::uint32_t id : near) {
            const auto o = b.object(id);
            if (!o || !o->building() || !o->entry || o->clan == b.self.clan || o->type == kTypeTeleport) continue;
            const float d = dist2d(pos, *o->entry);
            if (d < best) best = d, lid = id;
        }
        return lid;
    }
    bool go(Behaviour& b, const UnitState& st) { // slot 0x3c
        if (!given) target = pick(b, st.pos);
        const auto o = target == kNoObject ? std::nullopt : b.object(target);
        if (!o) return false;
        return b.walker.set_target(o->entry ? *o->entry : o->pos, b.go_speed());
    }
    bool start(Behaviour& b, const UnitState& st) override { // 10030280
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        arrived = false;
        return go(b, st);
    }
    bool takt(Behaviour& b, const UnitState& st) override {
        if (capture && target != kNoObject) {
            const auto o = b.object(target);
            if (!o || o->clan == b.self.clan) { // captured (or gone)
                if (given) return false;
                target = kNoObject, arrived = false;
                return go(b, st);
            }
        }
        if (!b.walker.idle()) return true;
        if (!arrived) arrived = true;
        return go(b, st);
    }
};

// M_Task_Leave (ORDER_LEAVE 0x14): a produced robot walks out of its plant (base-building.md §3.2: param
// -1, target 0x204), a unit is pushed out of a building's footprint by its build show (§2.3: target 0x201 =
// the building). behaviour-data.md: "finds a free spot at 150 m / 300 m". [inferred] The spot: a random
// direction (away from the building when one is given, within ±90°) at 150 m, 12 tries, then at 300 m;
// the task ends when the walker is idle. The task's own choice of "free" (no other unit there) is not
// traced.
struct LeaveTask : Task {
    using Task::Task;
    bool start(Behaviour& b, const UnitState& st) override {
        b.set_fire_mode(kFireAtWill, kNoObject, 0.5f);
        float base = 0, spread = kTwoPi;
        if (order.target_kind == kTargetBuilding) {
            const Vec3 from = b.object_pos(order.target_id);
            if (from.x != 0 || from.y != 0) base = std::atan2(st.pos.y - from.y, st.pos.x - from.x) - kTwoPi / 4, spread = kTwoPi / 2;
        }
        for (const float d : {150.f, 300.f})
            for (int i = 0; i < 12; ++i) {
                const float a = base + r01(b.rand) * spread;
                if (b.walker.set_target({st.pos.x + d * std::cos(a), st.pos.y + d * std::sin(a), 0}, b.go_speed())) return true;
            }
        return false;
    }
    bool takt(Behaviour& b, const UnitState&) override { return !b.walker.idle(); }
};

// Orders whose tasks are not ported: they cannot start, so the stack drops them.
struct NotImplementedTask : Task {
    using Task::Task;
    bool start(Behaviour&, const UnitState&) override { return false; }
    bool takt(Behaviour&, const UnitState&) override { return false; }
};

std::unique_ptr<Task> make_task(const Order& o) { // MTaskStack::CreateTaskFromOrder (10033a80), reduced
    switch (o.type) {
    case kOrderStop: return std::make_unique<StopTask>(o);
    case kOrderStayGround: return std::make_unique<StayGroundTask>(o);
    case kOrderGo: return std::make_unique<GoTask>(o);
    case kOrderAttack:
        if (o.target_kind == kTargetPlace) return std::make_unique<GoTask>(o); // M_Task_Go (order type stays 3)
        return std::make_unique<AttackTask>(o);
    case kOrderPatrol: return std::make_unique<PatrolTask>(o);
    case kOrderMigrate: return std::make_unique<MigrateTask>(o);
    case kOrderFollow: return std::make_unique<FollowTask>(o);
    case kOrderCapture: return std::make_unique<CaptureTask>(o);
    case kOrderLeave: return std::make_unique<LeaveTask>(o);
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

bool Task::check_limits(const Behaviour& b, const UnitState& st) const {
    if (limits.time > 0 && started_at != 0 && float(st.time - started_at) * 0.001f > limits.time) return false;
    if (limits.place_radius > 0 && dist3d(limits.place, st.pos) > limits.place_radius) return false;
    if (limits.unit != kNoObject && b.alive(limits.unit) && dist3d(b.object_pos(limits.unit), st.pos) > limits.unit_radius)
        return false;
    return true;
}

float Task::interrupt_desire(Behaviour&, const DecisionEvent& e, TaskLimits& l, const UnitState& st) {
    if (e.type <= 2) {
        l.place = st.pos;
        l.place_radius = 1000;
        return 1;
    }
    if (e.type == 3) return 0; // repair: needs a repair building (FUN_100156b0, not modelled)
    return 1;
}

float Task::target_score(const Behaviour&, Vec3 own, const SeenObject& o) const {
    const float d = dist2d(own, o.pos);
    if (d >= 500) return 0;
    return (2 - (o.armour + 1) / (o.rec18 + 1)) * ((o.rec1c * o.rec18 + 10) / (d + 10));
}

Behaviour::Behaviour(Walker& w, RandomSource r)
    : walker(w), rand(std::move(r)), default_stop_(std::make_unique<StopTask>(Order{kOrderStop})) {
    walker.tick_timer = &task_timer;
}

std::optional<SeenObject> Behaviour::object(std::uint32_t lid) const {
    if (!world || lid == kNoObject) return std::nullopt;
    auto o = world->object(lid);
    if (o && o->clan == kNoClan) return std::nullopt;
    return o;
}

Vec3 Behaviour::object_pos(std::uint32_t lid) const {
    const auto o = object(lid);
    return o ? o->pos : Vec3{};
}

bool Behaviour::alive(std::uint32_t lid) const { return object(lid).has_value(); }

std::uint32_t Behaviour::nearest_enemy(Vec3 centre) const {
    if (!world) return kNoObject;
    std::vector<std::uint32_t> near;
    world->objects_near(centre, 3000, near);
    float best = 1e10f;
    std::uint32_t lid = kNoObject;
    for (const std::uint32_t id : near) {
        const auto o = object(id);
        if (!o || o->building() || (o->type & 0x0100e000u) != o->type || o->clan == self.clan) continue;
        const float d = dist2d(centre, o->pos);
        if (d < best && d < 3000 && world->hostile(self.clan, o->clan)) best = d, lid = id;
    }
    return lid;
}

bool Behaviour::give_order(const Order& o, int mode) {
    auto t = make_task(o);
    if (!t->set_target(*this)) return false;
    if (mode == 3) tasks_.clear();
    if (mode == 2) tasks_.push_front(std::move(t));
    else tasks_.push_back(std::move(t));
    if (mode != 1) task_timer.reset(); // the new current task is looked at on the next tick (inferred)
    return true;
}

float Behaviour::task_takt(const UnitState& st) {
    if (!tasks_.empty() && tasks_.front()->started) {
        Task& cur = *tasks_.front();
        if (cur.takt(*this, st) && cur.check_limits(*this, st)) return cur.progress;
        tasks_.pop_front(); // "Task Ended"; report order done
    } else if (tasks_.empty()) {
        default_stop_->takt(*this, st);
        return 0;
    }
    while (!tasks_.empty()) { // StartCurrentTask
        Task& t = *tasks_.front();
        if (t.start(*this, st)) {
            t.started = true;
            t.started_at = st.time;
            return 0;
        }
        tasks_.pop_front(); // "Cannot StartTask"
    }
    return 0;
}

void Behaviour::observe(const UnitState& st) {
    enemies.clear();
    std::vector<std::uint32_t> near;
    world->objects_near(st.pos, sensor_radius, near);
    for (const std::uint32_t id : near) {
        if (id == self.logic_id || (id & kBuildingBit)) continue;
        const auto o = object(id);
        if (o && world->hostile(self.clan, o->clan)) enemies.push_back(id);
    }
}

bool Behaviour::check_targets(const UnitState& st) {
    if (!world) return false;
    float best = 0;
    std::uint32_t lid = kNoObject;
    const Task& cur = current();
    for (const std::uint32_t id : enemies) {
        const auto o = object(id);
        if (!o) continue;
        // (dist2d < 150 m and the time since the last shot only feed the alert state MB+0x3d0.)
        const float s = cur.target_score(*this, st.pos, *o);
        if (s > best) best = s, lid = id;
    }
    if (lid == kNoObject || !(best > 0)) return false;
    return insert_interrupt({0, lid}, st);
}

bool Behaviour::insert_interrupt(const DecisionEvent& e, const UnitState& st) {
    Task& cur = current();
    if (self.animal() && cur.order.type != kOrderMigrate) return false;
    if (self.building() || cur.order.type == kOrderBuild) return false;
    TaskLimits l;
    if (!(cur.interrupt_desire(*this, e, l, st) > 0.3f)) return false;
    if (!interrupt_timer.check_and_rearm(now, rand)) return false;
    interrupt_timer.rearm(now, rand);
    // The interrupt inherits the tighter of each limit of the current task.
    const TaskLimits& c = cur.limits;
    if (l.time == 0) l.time = c.time;
    else if (c.time != 0 && c.time < l.time) l.time = c.time;
    if (l.place_radius == 0 || (c.place_radius != 0 && c.place_radius < l.place_radius))
        l.place = c.place, l.place_radius = c.place_radius;
    if (l.unit == kNoObject || (c.unit != kNoObject && c.unit_radius < l.unit_radius)) l.unit = c.unit, l.unit_radius = c.unit_radius;
    // MTaskStack::CreateInterruptTask (10034510): a unit that cannot move takes no interrupt task.
    if (!self.building() && (walker.max_speed < 0.5f || !can_move)) return false;
    Order o{kOrderAttack};
    if (e.type <= 2) o.target_kind = kTargetLogicId, o.target_id = e.lid;
    else if (e.type == 5) o.target_kind = kTargetPlace, o.place = object_pos(e.lid);
    else return false; // 3 = M_Task_Reload: NOT IMPLEMENTED
    auto t = std::make_unique<AttackTask>(o);
    t->origin = 2;
    if (!t->set_target(*this)) return false;
    t->limits = l;
    tasks_.push_front(std::move(t));
    Task& f = *tasks_.front(); // StartCurrentTask
    if (f.start(*this, st)) f.started = true, f.started_at = st.time;
    else tasks_.pop_front(); // "Cannot StartTask"
    return true;
}

bool Behaviour::on_damage(std::uint32_t attacker) {
    if (!world || self.building() || !alive(attacker)) return false;
    const bool inserted = insert_interrupt({1, attacker}, walker.unit_state());
    if (inserted) task_timer.reset();
    return inserted;
}

void Behaviour::takt(std::uint32_t t, Wizard& wz) {
    now = t;
    if (world && observe_timer.check_and_rearm(now, rand)) observe(walker.unit_state());
    movement_takt(t, wz);
    if (world) fire.takt(*this, now);
}

void Behaviour::movement_takt(std::uint32_t t, Wizard& wz) {
    now = t;
    if (!task_timer.check_and_rearm(now, rand)) {
        if (refresh_timer.check_and_rearm(now, rand)) walker.retry_if_stuck(); // EscapeObstacle: not modelled
        return;
    }
    const UnitState st = walker.unit_state();
    // Decision layer: CheckRepair (not modelled) or CheckTargets insert an interrupt task, which is
    // looked at on the next tick.
    if (decision_timer.check_and_rearm(now, rand) && check_targets(st)) {
        task_timer.reset();
        return;
    }
    if (walker.max_speed < 0.5f || walker.max_speed < walker.min_speed) {
        if (can_move) walker.stop(); // StopAndHold
        can_move = false;
        return;
    }
    can_move = true;
    walker.sync_with_wizard(wz);
    progress_ = task_takt(st);
    walker.retry_if_stuck();
    walker.on_behaviour_takt(wz);
}

} // namespace parkan::ai
