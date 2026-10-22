// One computer-controlled unit: the brain (Behaviour + Walker + FireControl, ticked every game tick)
// and the pilot (Wizard, hooked into the motion runtime once per motion step before SelectNextMotion)
// driving a UnitMotion through its two IControl requests. Spec: notes/unit-ai-runtime.md §0, §2.10, §4.
//
// Combat: give the unit a Perception (set_perception) and read fire() after every update(): which
// weapons pull the trigger this tick, at which target, and where the turrets should point.
//
// Per frame the owner calls update(now) and then applies its ground / collision offsets with
// motion().apply_offset(...) (as play.cpp does for the hero); the brain reads the unit's state from
// the motion's output matrix, so offsets applied after update are seen on the next tick.
#pragma once

#include "ai/behaviour.hpp"
#include "control/unit_motion.hpp"

namespace parkan::ai {

struct AiUnitConfig {
    bool can_fly = false;     // BehChassis CanFly
    bool walk_chassis = true; // BehChassis WalkChassis
    bool animal = false;      // profession Type 0x20000000
};

class AiUnit {
public:
    // `motion`, `map` and `graph` must outlive the unit; motion.start() is the owner's. Installs
    // motion.pilot.
    AiUnit(UnitMotion& motion, const LandMap& map, const ArealGraph& graph, RandomSource rand, AiUnitConfig cfg = {});
    AiUnit(const AiUnit&) = delete;
    AiUnit& operator=(const AiUnit&) = delete;
    ~AiUnit();

    // GiveOrder(ORDER_GO, target kind 0x202 = the place, mode 3 replace).
    void give_order_go(Vec3 target);
    // The animals' GiveDefaultOrder (100088f0): ORDER_MIGRATE (target kind 0x204, mode 3) over `clan`'s
    // migration areals (shared by the clan's units; must outlive the unit).
    void set_migration(ClanMigration* clan);
    // false = refused (CreateTaskFromOrder: the task's SetTarget failed).
    bool give_order(const Order& o, int mode) { return brain_.give_order(o, mode); }
    // The world view and the unit's identity (LogicID, clan, ObjectTypeCode, profession flags). Without a
    // Perception the unit has no combat layer (no observer, CheckTargets or fire control).
    void set_perception(const Perception* world, const SelfInfo& self);
    // OnDamageMsg: hit by `attacker` (may insert an interrupt attack on it).
    bool on_damage(std::uint32_t attacker) { return brain_.on_damage(attacker); }
    // The fire control's result of the last update(); weapons / turret groups are configured through
    // fire_control().weapons / .turret_error (FireControl). The trigger is a request: the weapon device
    // still applies its own reload (the AI re-times weapons at every task tick, 10023f30).
    const FireRequest& fire() const { return brain_.fire.request(); }
    FireControl& fire_control() { return brain_.fire; }
    // The aim angles of the fire target seen from `mount` (world), for TurretRig (see aim_angles).
    AimAngles turret_aim(Vec3 mount) const { return aim_angles(motion_->output_matrix(), mount, fire().aim); }

    // One game tick: OnTakt (observer, MovementTakt, fire control), then the motion update (the pilot runs inside it per motion step).
    void update(float now_ms);
    // SetPointAltitude's ground source (optional; see Walker::ground_height).
    void set_ground_height(std::function<float(float, float)> f) { walker_.ground_height = std::move(f); }

    UnitState state() const; // GetUnitState: output-matrix position and forward, last step velocity
    Vec3 position() const { return state().pos; }
    std::uint32_t current_order() const { return brain_.current_order(); }
    bool moving() const { return !walker_.idle(); }

    UnitMotion& motion() { return *motion_; }
    Behaviour& brain() { return brain_; }
    Walker& walker() { return walker_; }
    Wizard& wizard() { return wizard_; }

private:
    UnitMotion* motion_;
    RandomSource rand_; // one generator shared by the walker and the brain (the CRT's rand())
    Walker walker_;
    Behaviour brain_;
    Wizard wizard_;
    std::uint32_t now_ = 0;
};

} // namespace parkan::ai
