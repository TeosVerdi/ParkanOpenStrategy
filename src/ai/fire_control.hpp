// The unit's fire control: MBehaviour+0x35c (ctor 10023e80). Tasks request a fire mode through
// 10023f30 ({mode, LogicID, value} at FC+0x28, which also re-times every weapon, 1001b650); OnTakt runs
// 10023ff0 every game tick for an AI-fired unit (MB+0xa04 & 0x40): it picks the target (modes 1–4,
// 100254b0 / 100255a0 / 100256e0), points the turret groups at it (IDeviceSet slot 0x40 + device state
// 0x400 "AI point"), casts the line of fire and pulls each weapon's trigger (device state 0x200) when
// its aim, range and timer allow. Spec: notes/unit-ai-runtime.md §4.5–§4.7.
//
// The port stops at the decision: FireRequest says which weapons fire this tick, at what, and where
// the turrets should point; the app spawns the shots (weapons / damage are not this module's).
//
// Not modelled: the manual-channel override (FC+0x5c / +0x60 → 10025a00), the aim-part choice on the
// target (10025830: the app's aim point is the target position), the weapon property 6 > 10000 skip
// (10024d18), the bunker branch (own unit standing in a building on Go/Attack/Search: threshold 0) and
// the replay records (MB+0x908).
#pragma once

#include "ai/mtimer.hpp"
#include "ai/perception.hpp"

#include <vector>

namespace parkan::ai {

enum FireModeKind : std::uint32_t {
    kFireHold = 0,      // no target
    kFireTarget = 1,    // FC+0x2c
    kFireAtWill = 2,    // nearest seen enemy within 500 m (100254b0)
    kFireStrongest = 3, // highest (armour + weapon) / (d + 10) (100255a0)
    kFireWeakest = 4,   // highest 1 / ((d + 10)(armour + weapon + 10)) (100256e0)
};

// FC+0x28: what the current task asks for. Idle and moving tasks use {2, −1, 0.5}; an attack that is
// fighting uses {1, target, 0.5}. `value` is stored but has no reader in 10023ff0.
struct FireMode {
    std::uint32_t mode = kFireAtWill;
    std::uint32_t target = kNoObject;
    float value = 0.5f;
};

// One weapon record (0x58 bytes of the parts summary MB+0x5fc) as the fire control reads it. The app
// fills the data fields from its weapon parts; the defaults are the port's (no weapon data) and are
// *inferred*: an unlimited-rate turret gun with a 0..150..200 m envelope.
struct Weapon {
    int group = 0;            // −1 = fixed on the chassis (MB+0x660), ≥ 0 = turret group (MB+0x658 index)
    float rate = 0;           // +0x28: > 2 → period 30/(rate·F) + U(0, 30/(rate·F)) s, else 0.5/F + U(0, 1.5/F)
    std::uint32_t flags = 0;  // +0x34: 0x10 → range factor 1.1, 0x8 → 1.0 (no envelope)
    float height = 0;         // +0x14: range factor · (1 − Δz/height); 0 = no height term (port)
    float near = 0, opt = 150, far = 200; // +0x44 / +0x48 / +0x4c: the range envelope (m)
    // The weapon device's aim state (IDeviceSet slot 0x28): 0 aimed → 0, 1 slewing with progress v →
    // (1 − v)·π, otherwise π. Set by the app before the tick.
    float aim_error = 0;
    MTimer timer;             // +0: re-timed by every fire-mode request
};

// The result of one fire-control pass (valid until the next takt).
struct FireRequest {
    std::uint32_t target = kNoObject; // FC+0x50, −1 = nothing to shoot at
    Vec3 aim{};               // the target's position (turret device target for state 0x400)
    bool track = false;       // turret groups should track `aim` (set when a target exists and a weapon sits on a turret)
    float distance = 0;       // 3-D, unit → target
    bool line_clear = false;  // the ray and the friendly-in-line check
    std::uint32_t trigger = 0; // bit i: weapons[i] fires this tick (device state 0x200); 0 between gated passes
};

// Turret aiming for the app's TurretRig: the angles of `aim` seen from `mount` (world) in the unit's
// body frame (placement forward = column 1). yaw > 0 = to the right (clockwise from above, the ANGLE_X
// direction), pitch > 0 = up. Control's state 0x400 turns them into channel times t = angle/range +
// default per joint (AngleAboutAxis 10026e20) and slews at the joint rate; the sign of yaw against the
// joint's invert flag is *inferred*.
struct AimAngles {
    float yaw = 0, pitch = 0;
};
AimAngles aim_angles(const Mat4& placement, Vec3 mount, Vec3 aim);

// FUN_1001b9f0: the weapon's range factor at 3-D distance `d` with the unit `dz` metres above the target.
float range_factor(const Weapon& w, float d, float dz);

class Behaviour;

class FireControl {
public:
    // 10023f30: store the task's request (unless the manual channel overrides it) and re-time every
    // weapon (1001b650: SetPeriod clears `next`, so each weapon may fire at its next pass).
    void set_mode(const FireMode& m);
    // 10023ff0, called every game tick.
    void takt(Behaviour& b, std::uint32_t now);

    const FireMode& mode() const { return mode_; }
    const FireRequest& request() const { return out_; }
    std::uint32_t target() const { return current_; }

    std::vector<Weapon> weapons{Weapon{}};
    std::vector<float> turret_error; // per turret group: turret device slot 0x28 → 0 / (1 − v)·π / π
    float freq_factor = 1;           // BehDifficulty Fire_FreqFactor (no sheet loaded: 1)
    float muzzle_height = 2;         // the ray start above the unit origin (port; 1001cd00 not traced)
    bool overridden = false;         // FC+0x5c / +0x60: mode requests ignored (manual fire not modelled)
    MTimer gate{0, 1, 1};            // FC+0x38: 64–128 ms between passes
    MTimer retarget{0, 31, 15};      // FC+0x40: 1.98–2.94 s between target choices
    MTimer hold{0, 15, 0};           // FC+0x48: reset at each choice (no reader found)

private:
    std::uint32_t choose(Behaviour& b, Vec3 own) const;
    FireMode mode_{};
    std::uint32_t current_ = kNoObject; // FC+0x50
    FireRequest out_{};
};

} // namespace parkan::ai
