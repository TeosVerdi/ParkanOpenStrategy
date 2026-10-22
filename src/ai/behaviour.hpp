// The unit brain, reduced to movement: MBehaviour (Behavior.dll, iid 0x10) MovementTakt (10005110,
// class slot 56), its task stack MTaskStack (MBehaviour+0x520; Takt 10034a30) and the movement tasks
// M_Task_Stop (10031db0 / 10031e10), M_Task_StayGround (10031ca0), M_Task_Go (1002b3c0..1002b670),
// M_Task_Migrate (1002c9d0 / 1002ca60 / 1002cba0) and M_Task_Patrol (1002d520..). MTimer at 1004c4c0..
// 1004c550. Spec: notes/unit-ai-runtime.md §1, §2.1.
//
// NOT IMPLEMENTED (tasks that cannot start, so the order is dropped as "Cannot StartTask"): Attack (3),
// Follow (0x16) and every non-movement order; Go into a building (target kind 0x201). Also not
// modelled: the decision layer (CheckRepair / CheckTargets interrupt tasks, timer +0x958), fire
// control and fire modes, the threat state, order limits (CheckLimits), EscapeObstacle, the LEAVE
// order for units inside a building, StopAndHold (1003dc90, taken as Stop).
#pragma once

#include "ai/walker.hpp"

#include <array>
#include <deque>
#include <memory>

namespace parkan::ai {

// MTimer (8 bytes): base and jitter in units of 64 ms; fires at most once per `next`.
struct MTimer {
    std::uint32_t next = 0;
    std::uint16_t base = 0, jitter = 0;

    void set_period(float min_s, float rand_s); // SetPeriod 1004c4c0 (also clears next)
    void rearm(std::uint32_t now, const RandomSource& rand); // Rearm 1004c500
    bool check_and_rearm(std::uint32_t now, const RandomSource& rand); // CheckAndRearm 1004c550
    void reset() { next = 0; } // 1004c540: fires on the next check
};

enum OrderType : std::uint32_t {
    kOrderNone = 0,
    kOrderStop = 1,
    kOrderGo = 2,
    kOrderAttack = 3,
    kOrderPatrol = 4,
    kOrderMigrate = 0xf,
    kOrderStayGround = 0x15,
    kOrderFollow = 0x16,
};
enum TargetKind : std::uint32_t { kTargetBuilding = 0x201, kTargetPlace = 0x202, kTargetNone = 0x204 };

// The parts of the 0x13c-byte Order the movement tasks read.
struct Order {
    std::uint32_t type = kOrderNone;
    float param = 0;
    std::uint32_t target_kind = kTargetNone;
    Vec3 place{};
};

// A clan's migration areals (IArealMap CreateMigrationAreals 100220e0 / GetMigrationAreal 100221a0 /
// PickMigrationAreal 10022230): the data.tma mode-0 clan records {x, y, z, size, ?}.
struct ClanMigration {
    std::vector<std::array<float, 5>> areals;
    // The per-clan choice is kept until this timer fires, then rand() % count. Its period is not traced:
    // a zero period (a fresh pick whenever the time moved on) is assumed.
    MTimer timer;
    int current = -1;
    int pick(std::uint32_t now, const RandomSource& rand);
};

class Behaviour;

// MTask: slot 6 StartTask, slot 7 OnBehaviourTakt (false = the task is over).
class Task {
public:
    explicit Task(const Order& o) : order(o) {}
    virtual ~Task() = default;
    virtual bool start(Behaviour&, const UnitState&) { return true; }
    virtual bool takt(Behaviour& b, const UnitState& st) = 0;
    Order order;
    bool started = false;
    float progress = 0; // MBehaviour+0xa38 while current
};

class Behaviour {
public:
    Behaviour(Walker& walker, RandomSource rand);

    // GiveOrder → MakeOrder → order list → CreateTaskFromOrder: mode 1 append, 2 prepend, 3 replace.
    // A task that becomes current here is started on the next task tick (inferred).
    void give_order(const Order& o, int mode);
    std::size_t order_count() const { return tasks_.size(); }
    std::uint32_t current_order() const { return tasks_.empty() ? kOrderNone : tasks_.front()->order.type; }
    float progress() const { return progress_; }

    // MovementTakt, run on every game tick (OnTickMsg → OnTakt) for an AI-moved unit with a wizard.
    void movement_takt(std::uint32_t now, Wizard& wz);

    // MB+0x5fc · Go_SpeedPercent (1).
    float go_speed() const { return walker.max_speed; }

    Walker& walker;
    RandomSource rand;
    ClanMigration* migration = nullptr; // the unit's clan areals (animals)
    MTimer task_timer{0, 7, 7};   // +0x938: 0.45–0.9 s task + path tick
    MTimer refresh_timer{0, 7, 7}; // +0x948: walker refresh between task ticks
    bool can_move = false;         // MB+0xa04 & 0x800
    std::uint32_t now = 0;

private:
    float task_takt(const UnitState& st); // MTaskStack::Takt
    std::deque<std::unique_ptr<Task>> tasks_;
    std::unique_ptr<Task> default_stop_;
    float progress_ = 0;
};

} // namespace parkan::ai
