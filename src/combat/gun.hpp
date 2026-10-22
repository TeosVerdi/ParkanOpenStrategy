// A weapon device: Control.dll CGunDevice (CTLD device type 2 / 0x1e; ctor 100294c0, Load 10029650,
// SetState 1002a100, Update 10029ca0, StepBarrel 1002a190) driven by CControlBase::UpdateDevices
// (1002d260). Spec: notes/weapons-damage.md §1.2, §2.1, §2.3, §2.4.
//
// Port choices (inferred where marked):
// - energy: refilled at `recharge_per_s` (∞ by default = always full; the battery budget
//   UpdateEnergyBudget is not ported — spec §7.3);
// - the barrel channels interpolate linearly from `from` to `to` over the device's [start, end]
//   (CDevice::ApplyJoints not read; inferred);
// - GetParam's 0x100 / 0x200 scaling (part health, energy ratio) is not applied;
// - the readiness value (HUD, dev+0x17c) is not kept.
#pragma once

#include "control/turret.hpp"
#include "res/weapons.hpp"

#include <cmath>
#include <optional>

namespace parkan::combat {

// SetState values (dev+0x50; ManManager MCMD_STATE CIS_*).
enum GunCommand : std::uint32_t {
    kCeaseFire = 0,       // CIS_SWITCHOFF
    kFireContinuous = 0x100, // CIS_CONTINUEFIGHT (LMB held)
    kFireSingle = 0x200,  // one shot, then back to 0 (the AI's trigger)
    kFireGroup = 0x400,
    kGunReset = 0x1000,   // on selection: state 2, lock timer reset
};

// dev+0x11c.
enum GunState : std::uint32_t {
    kGunIdle = 0, kGunLocking = 1, kGunReady = 2, kGunFiring = 3, kGunInterval = 4, kGunEmpty = 5,
    kGunNoEnergy = 6, kGunOutOfRange = 7, kGunNotAimed = 8,
};

// What the device asks of its owner.
class GunHost {
public:
    virtual ~GunHost() = default;
    // GetContactPoint(joint.point_b, space 2): the barrel's muzzle and direction (normalised) in the world.
    virtual bool muzzle(std::uint32_t joint, Vec3& pos, Vec3& dir) = 0;
    // CDevice slot 2: the device's mesh part is destroyed.
    virtual bool broken(std::int32_t part) = 0;
    // GetTargetPosition (1002a8c0): the target pushed by the turret / SetTarget, if any.
    virtual std::optional<Vec3> target_position() = 0;
    // StepBarrel step 2: create the bullet at `pos` flying along `dir` (before sight convergence).
    virtual void spawn(class GunDevice& gun, Vec3 pos, Vec3 dir) = 0;
    // FUN_100221e0(hdr+0xc) at the recoil step (inferred: a sound or event index).
    virtual void recoil(GunDevice&, std::uint32_t /*barrel*/) {}
};

class GunDevice {
public:
    // `joints` are the CTLD joints of the device's CTLD (barrel refs index them); `bullet` may be null
    // (the device then steps its barrels without firing).
    GunDevice(GunDeviceSpec spec, std::vector<CtldJoint> joints, const BulletProto* bullet);

    const GunDeviceSpec& spec() const { return spec_; }
    const BulletProto* bullet() const { return bullet_; }
    const std::vector<CtldJoint>& joints() const { return joints_; }

    // SetState (1002a100).
    void set_state(std::uint32_t s);
    std::uint32_t command() const { return command_; }

    // UpdateDevices for this device up to `now_ms`: every due step (barrel step or state decision) runs.
    void update(float now_ms, GunHost& host);

    // GetParam (CDevice 10021d00) without the 0x100/0x200 scaling; params 8..10 are filled from the
    // bullet at construction (ReloadBulletInfo 100296f0).
    float param(std::size_t i) const { return i < param_.size() ? param_[i] : 0.f; }

    GunState state() const { return state_; }
    std::int32_t ammo() const { return ammo_; }     // −1 = unlimited
    float energy() const { return energy_; }
    std::size_t barrel() const { return cur_; }
    std::size_t barrel_count() const { return channels_.size(); }
    std::uint32_t barrel_joint(std::size_t b) const { return channels_[b].joint; }
    std::uint32_t shots() const { return shots_; }   // bullets spawned (port statistics)
    float lock_timer() const { return lock_; }       // dev+0x170: seconds left to lock on (HUD brackets)
    bool enabled = true;                               // dev+0x118
    float recharge_per_s = INFINITY;                   // port (see the file comment)
    float damage_mult = 1;                             // dev+0x180 → bullet property 0xb4

    // The keyframe time of barrel `b`'s joint at `now_ms` (recoil animation); nullopt when the joint
    // has no mesh output.
    std::optional<std::pair<std::int32_t, float>> barrel_frame(std::size_t b, float now_ms) const;

private:
    struct Channel {
        std::uint32_t joint = 0;
        float cur = 0, from = 0, to = 0;
        std::uint32_t counter = 0; // dev+0x12c: StepBarrel's 4-step counter
    };
    void tick(float dt, GunHost& host);
    bool step_barrel(std::size_t b, GunHost& host);
    void extend_move_end(std::size_t b);
    void apply_joints(float now);
    bool skip_joint(std::size_t b) const;

    GunDeviceSpec spec_;
    std::vector<CtldJoint> joints_;
    const BulletProto* bullet_ = nullptr;
    std::array<float, 16> param_{};
    std::vector<Channel> channels_;
    std::size_t cur_ = 0;               // dev+0x128
    GunState state_ = kGunIdle;         // dev+0x11c
    std::uint32_t command_ = kCeaseFire; // dev+0x50
    float energy_ = 0;                  // dev+0x120
    std::int32_t ammo_ = -1;            // dev+0x124
    float lock_ = -1;                   // dev+0x170
    float start_ = 0, end_ = 0;         // dev+0xc / +0x10
    bool started_ = false;              // dev+0x2c
    std::uint32_t shots_ = 0;
};

} // namespace parkan::combat
