// One computer-controlled unit: the brain (Behaviour + Walker, ticked every game tick) and the pilot
// (Wizard, hooked into the motion runtime once per motion step before SelectNextMotion) driving a
// UnitMotion through its two IControl requests. Spec: notes/unit-ai-runtime.md §0, §2.10.
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
    void give_order(const Order& o, int mode) { brain_.give_order(o, mode); }

    // One game tick: MovementTakt, then the motion update (the pilot runs inside it per motion step).
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
