// The path follower between the brain and the pilot: MWalker (Behavior.dll, at MBehaviour+0xb8; ctor
// 10036290). SetTarget (1003bad0) plans an areal route and a corridor, RefineCorridorPoint (10038e00)
// turns corridor points into legs, EmitTrajectoryPoints (1003a480) times them, OnBehaviourTakt
// (1003d280) keeps 3 timed points queued, PushPointsToWizard (1003d960) / SendHoldPosition (1003d7f0)
// feed the CWizard, SyncWithWizard (1003c5c0) drops what it consumed and re-plans off route.
// Spec: notes/unit-ai-runtime.md §2.2–§2.7.
//
// Not modelled (no buildings yet): building legs (1003fde0), hallway locks (LockNextPath / UnLockOne),
// obstacle detours in RefineCorridorPoint (corridor points become legs as they are), EscapeObstacle
// (1003dde0), the flyer leg subdivision every 20 m and its turn-in-place point, the UseWizard = 0
// internal mover.
#pragma once

#include "ai/pathfind.hpp"
#include "ai/wizard.hpp"

#include <deque>
#include <functional>

namespace parkan::ai {

struct MTimer;

// The 0x48-byte unit-state / trajectory-point record (GetUnitState 10014bd0).
struct UnitState {
    Vec3 pos{};
    Vec3 vel{};     // world m/s of the last mover step (control property 0x27)
    Vec3 dir{0, 1}; // forward axis, normalised
    std::uint32_t time = 0;
};

struct TrajectoryPoint {
    Vec3 pos{}, vel{}, heading{};
    std::uint32_t time = 0;
    std::uint32_t areal = 0;
    bool linear_heading = false; // +0x44
};

class Walker {
public:
    Walker(const LandMap& map, const ArealGraph& graph, RandomSource rand);

    // Chassis / unit facts (BehChassis CanFly / WalkChassis, profession Type 0x20000000).
    bool can_fly = false, walk_chassis = true, animal = false;
    // Parts summary MB+0x614 / +0x618: runtime max forward speed body[7] and min speed body[4].
    float max_speed = 0, min_speed = 0;
    // GetUnitState, supplied by the owner.
    std::function<UnitState()> unit_state;
    // Ground height for SetPointAltitude (10040f20: z = ground + 15, animals + 30 + r01·50); when
    // unset the point keeps the previous point's z (ground units ignore z: flag nibble 3 on body z).
    std::function<float(float x, float y)> ground_height;
    // MBehaviour+0x938, the 0.45–0.9 s task/path timer: SetTarget and Stop reset it.
    MTimer* tick_timer = nullptr;

    // SetTarget(target, {speed, −1, 5.0}): clamps the cruise speed, plans; false when the target is not
    // on a walkable areal or no route exists.
    bool set_target(Vec3 target, float speed);
    void stop();                                  // MWalker::Stop
    bool idle() const { return corridor_.empty() && legs_.empty() && points_.empty(); } // IsIdle 1003dd80
    void retry_if_stuck();                        // RetryIfStuck 1003f3a0
    void sync_with_wizard(Wizard& wz);
    void on_behaviour_takt(Wizard& wz);

    Vec3 target() const { return target_; }
    float cruise_speed() const { return cruise_; }
    int off_route_count() const { return off_route_; }
    int replans() const { return replans_; }
    const std::vector<std::uint32_t>& route() const { return route_; } // the last areal path
    const std::deque<CorridorPoint>& corridor() const { return corridor_; }
    const std::deque<CorridorPoint>& legs() const { return legs_; }
    const std::deque<TrajectoryPoint>& points() const { return points_; }

private:
    bool plan_global_path();
    bool refine_corridor_point();
    bool emit_trajectory_points(const TrajectoryPoint& prev, float end_v, float v0);
    void push_points_to_wizard(Wizard& wz);
    void send_hold_position(Wizard& wz);
    void set_point_altitude(Vec3& p, float fallback_z);
    void clear();

    const LandMap* map_;
    const ArealGraph* graph_;
    RandomSource rand_;
    int off_route_ = 0;                 // W+0x2c
    bool hold_request_ = false;         // W+0xb4
    Vec3 target_{};                     // W+0xb8
    float cruise_ = 0;                  // W+0xdc
    TrajectoryPoint last_reached_{};    // W+0x6c
    std::deque<CorridorPoint> corridor_, legs_; // W+0xe8, W+0xfc
    std::deque<TrajectoryPoint> points_;        // W+0x110
    std::vector<std::uint32_t> route_;
    int replans_ = 0;
};

} // namespace parkan::ai
