// The unit brain: MBehaviour (Behavior.dll, iid 0x10) MovementTakt (10005110, class slot 56), its task
// stack MTaskStack (MBehaviour+0x520; Takt 10034a30), the decision layer at MB+0x5b8 (CheckTargets
// 10017e70, InsertInterrupt 100179c0, MTaskStack::CreateInterruptTask 10034510), the observer at
// MB+0x4a8 (100234e0 / 10023240) and the tasks M_Task_Stop (10031db0 / 10031e10), M_Task_StayGround
// (10031ca0..), M_Task_Go (1002b3c0..1002b670), M_Task_Attack (10026a30..10028310), M_Task_Follow
// (1002acd0..1002ae20), M_Task_Migrate (1002c640..1002cba0), M_Task_Patrol (1002d250..) and the capture
// mode of M_Task_Search (10030110..10030300, notes/buildings.md §3.6). The fire
// control (MB+0x35c) is ai/fire_control.hpp. Spec: notes/unit-ai-runtime.md §1, §2.1, §4.
//
// The economy tasks (Transport 6, Build 7, Mine 0xa, Construct 0xc, Research 0xe/0x10, ShowUpgrade 0x12,
// Upgrade 0x18) run in src/economy (economy::Economy, reached through its EconomyControl); M_Task_Leave
// (0x14) is here.
// NOT IMPLEMENTED (tasks that cannot start, so the order is dropped as "Cannot StartTask"): every other
// order; Go into a building (target kind 0x201); patrols around a building or a unit. Also not modelled:
// CheckRepair (10017d50: always false, there is no damage summary), the repair interrupt (event 3), the
// threat / alert state MB+0x3d0 (10018ac0, 10017c70, 10019a10: animation states), the clan state gates
// (IArealMap slot 0x28(clan) == 3 / 0), the call for help on damage (1000c260), EscapeObstacle, the LEAVE
// order for units inside a building, StopAndHold (1003dc90, taken as Stop), and every building-hallway
// branch of the attack task (target or attacker standing on a hallway vertex, ruins).
#pragma once

#include "ai/fire_control.hpp"
#include "ai/mtimer.hpp"
#include "ai/perception.hpp"
#include "ai/walker.hpp"

#include <array>
#include <deque>
#include <memory>

namespace parkan::ai {

enum OrderType : std::uint32_t {
    kOrderNone = 0,
    kOrderStop = 1,
    kOrderGo = 2,
    kOrderAttack = 3,
    kOrderPatrol = 4,
    kOrderSearch = 5,
    kOrderBuild = 7,
    kOrderMigrate = 0xf,
    kOrderCapture = 0x11, // ORDER_ROBOT_CAPTURE: M_Task_Search ("захват")
    kOrderLeave = 0x14, // M_Task_Leave (walk out of a plant / a building's footprint)
    kOrderStayGround = 0x15,
    kOrderFollow = 0x16,
};
// 0x201 = by LogicID (a unit, or a building when bit 31 is set), 0x202 = a place, 0x204 = none.
enum TargetKind : std::uint32_t { kTargetBuilding = 0x201, kTargetPlace = 0x202, kTargetMask = 0x203, kTargetNone = 0x204 };
inline constexpr std::uint32_t kTypeTeleport = 0x80000200;
inline constexpr std::uint32_t kTargetLogicId = kTargetBuilding;

// The task limits (Order+0x110, 7 dwords; MTask::SetLimits 100014c0 copies them to task+0x2c, ai.dll
// 100043a0 initialises them to {0, …, 0, −1, 0}). MTask::CheckLimits (10001660) ends the task when one
// is exceeded ("Time Limit expired" / "Place Limit expired" / "Unit Limit expired").
struct TaskLimits {
    float time = 0;                 // s since the task started (+0x48); 0 = none
    Vec3 place{};                   // centre of the place limit
    float place_radius = 0;         // 3-D distance from `place`; 0 = none
    std::uint32_t unit = kNoObject; // a unit the task must stay near (−1 = none)
    float unit_radius = 0;          // 3-D distance from that unit (checked while it exists)
};

// The parts of the 0x13c-byte Order the tasks read. The script's fSuccess / fSurvive / fTime /
// fIndependence (Order+0x12c..+0x138) have no reader in the ported tasks.
struct Order {
    std::uint32_t type = kOrderNone;
    float param = 0;
    std::uint32_t target_kind = kTargetNone;
    Vec3 place{};                        // kind 0x202
    std::uint32_t target_id = kNoObject; // kind 0x201 (Order+0x10)
    std::uint32_t mask = 0;              // kind 0x203: an ObjectTypeCode mask (0x80000000 buildings)
    TaskLimits limits{};
};

// A decision-layer event (the record CreateInterruptTask 10034510 reads): type 0 = an enemy was seen
// (CheckTargets), 1 = the unit was hit by `lid` (OnDamageMsg 100064b0 → 10018060), 2 = attack `lid`,
// 3 = go repair (CheckRepair), 5 = attack the place of `lid`.
struct DecisionEvent {
    std::uint32_t type = 0;
    std::uint32_t lid = kNoObject;
};

// A clan's migration areals (IArealMap CreateMigrationAreals 100220e0 / GetMigrationAreal 100221a0 /
// PickMigrationAreal 10022230): the data.tma mode-0 clan records {x, y, z, inner, outer} (the radii as
// M_Task_Migrate's slot 12 reads them, 1002c640).
struct ClanMigration {
    std::vector<std::array<float, 5>> areals;
    // The per-clan choice is kept until this timer fires, then rand() % count. Its period is not traced:
    // a zero period (a fresh pick whenever the time moved on) is assumed.
    MTimer timer;
    int current = -1;
    int pick(std::uint32_t now, const RandomSource& rand);
};

class Behaviour;

// Who the unit is (IBehaviour GetLogicID slot 0x30 / GetClan slot 0x44 / GetCreationParam slot 0x38) and
// the profession facts the combat code reads.
struct SelfInfo {
    std::uint32_t logic_id = kNoObject;
    std::uint32_t clan = kNoClan;
    std::uint32_t type = 0;     // ObjectTypeCode
    bool can_targeting = true;  // BehProfession CanTargeting (+0xc): the fire threshold 0.45 (else 0.85)
    bool has_weapons = true;    // QI(0x204) slot 4 property 5 != 0: interrupt attacks, Follow's slot 12
    std::uint32_t size_class = 0; // property 0x201: 1 T, 2 S, 3 M, 4 L (only < 3 capture buildings)
    bool animal() const { return type == kTypeAnimal; }
    bool building() const { return logic_id != kNoObject && (logic_id & kBuildingBit) != 0; }
};

// MTask: slot 3 SetTarget, 6 StartTask, 7 OnBehaviourTakt (false = the task is over), 9 CheckLimits,
// 12 interrupt desire, 13 target score.
class Task {
public:
    explicit Task(const Order& o) : order(o), limits(o.limits) {}
    virtual ~Task() = default;
    virtual bool set_target(Behaviour&) { return true; } // false: the order is refused (CreateTaskFromOrder)
    virtual bool start(Behaviour&, const UnitState&) { return true; }
    virtual bool takt(Behaviour& b, const UnitState& st) = 0;
    bool check_limits(const Behaviour& b, const UnitState& st) const; // 10001660
    // Slot 12: how much the task lets `e` interrupt it (> 0.3 = yes); may set `l`, the limits of the
    // interrupt task. Base 100018a0: events 0..2 → 1.0 with a place limit of 1000 m around the unit.
    virtual float interrupt_desire(Behaviour& b, const DecisionEvent& e, TaskLimits& l, const UnitState& st);
    // Slot 13: CheckTargets' score of a seen enemy (0 = ignore). Base 10001010.
    virtual float target_score(const Behaviour& b, Vec3 own, const SeenObject& enemy) const;
    Order order;
    TaskLimits limits;            // +0x2c
    int origin = 1;               // +0x54: 1 = from an order, 2 = an interrupt task
    std::uint32_t started_at = 0; // +0x48
    bool started = false;
    float progress = 0; // MBehaviour+0xa38 while current
};

class Behaviour {
public:
    Behaviour(Walker& walker, RandomSource rand);

    // GiveOrder → MakeOrder → order list → CreateTaskFromOrder: mode 1 append, 2 prepend, 3 replace.
    // false = refused (the task's SetTarget failed, e.g. an attack on an own-clan unit). A task that
    // becomes current here is started on the next task tick (inferred).
    bool give_order(const Order& o, int mode);
    std::size_t order_count() const { return tasks_.size(); }
    std::uint32_t current_order() const { return tasks_.empty() ? kOrderNone : tasks_.front()->order.type; }
    const Task* current_task() const { return tasks_.empty() ? nullptr : tasks_.front().get(); }
    float progress() const { return progress_; }

    // OnTakt (10004c40) for an AI unit, every game tick: the observer (100234e0), MovementTakt (slot 56;
    // a building — LogicID bit 31 — runs slot 57, building_takt, instead), then the fire control (10023ff0).
    // Without a Perception only MovementTakt runs (no combat layer).
    void takt(std::uint32_t now, Wizard& wz);
    // Slot 57 (100054a0), a building's tick: its task stack every 64–128 ms (the Stop task asks the fire
    // control for fire at will).
    void building_takt(std::uint32_t now);
    // MovementTakt alone.
    void movement_takt(std::uint32_t now, Wizard& wz);
    // OnDamageMsg (100064b0): hit by `attacker` → InsertInterrupt({1, attacker}). True if an
    // interrupt attack was inserted.
    bool on_damage(std::uint32_t attacker);

    // MB+0x5fc · Go_SpeedPercent (1).
    float go_speed() const { return walker.max_speed; }
    // FUN_10023f30 on MB+0x35c.
    void set_fire_mode(std::uint32_t mode, std::uint32_t target, float value) { fire.set_mode({mode, target, value}); }
    // FUN_10014a40 / FUN_100149b0: an object's position, {0, 0, 0} when it does not exist.
    Vec3 object_pos(std::uint32_t lid) const;
    // FUN_10014b70: the object exists and has a clan.
    bool alive(std::uint32_t lid) const;
    std::optional<SeenObject> object(std::uint32_t lid) const;
    // 10001120: the nearest hostile transport / builder / warbot (type & 0x0100e000 == type) within
    // 3000 m (2-D) of `centre`, −1 if none.
    std::uint32_t nearest_enemy(Vec3 centre) const;

    Walker& walker;
    RandomSource rand;
    const Perception* world = nullptr; // nullptr: the unit sees nothing (no combat layer)
    SelfInfo self;
    FireControl fire;                   // MB+0x35c
    ClanMigration* migration = nullptr; // the unit's clan areals (animals)
    MTimer task_timer{0, 7, 7};        // +0x938: 0.45–0.9 s task + path tick
    MTimer building_timer{0, 1, 1};    // +0x930 (ctor 1000398f): a building's task tick, 64–128 ms
    MTimer refresh_timer{0, 7, 7};     // +0x948: walker refresh between task ticks
    MTimer decision_timer{0, 15, 15};  // +0x958: 0.96–1.92 s CheckRepair / CheckTargets
    MTimer interrupt_timer{0, 31, 46}; // decision part +0x28 (MTimer(2, 3), ctor at 1000387b): 2–4.9 s
    MTimer observe_timer{0, 7, 23};    // observer +0x28 (10023120): 0.45–1.9 s sensor scan
    float sensor_radius = 300;         // observer +0x38 (MB+0x74 slot 4; not traced, assumed)
    std::vector<std::uint32_t> enemies; // observer +0x50 (MB+0x4f8): hostile units the sensor sees
    std::uint32_t last_fire = 0;       // MB+0xb4: the last time a weapon fired (set by the fire control)
    bool can_move = false;             // MB+0xa04 & 0x800
    std::uint32_t now = 0;

private:
    friend class FireControl;
    float task_takt(const UnitState& st); // MTaskStack::Takt
    bool check_targets(const UnitState& st); // 10017e70
    bool insert_interrupt(const DecisionEvent& e, const UnitState& st); // 100179c0
    void observe(const UnitState& st); // 10023240
    Task& current() { return tasks_.empty() ? *default_stop_ : *tasks_.front(); }
    std::deque<std::unique_ptr<Task>> tasks_;
    std::unique_ptr<Task> default_stop_;
    float progress_ = 0;
};

} // namespace parkan::ai
