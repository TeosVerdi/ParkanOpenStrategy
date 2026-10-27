// A unit's device energy budget: Control.dll CControlBase::UpdateEnergyBudget (1002d340), run on the slow
// tick after SlowTickDamage, over every device of the control (ctl+0x5bc, CreateDevice 1002d4b0 order).
// Spec: notes/weapons-damage.md §8.
//
// Each device states what it offers (> 0, batteries) or needs (< 0) for the step (slot 5); the sum of the
// offers is the power P. The needs are met category by category (table 1003ccc8 by device type) with
// FUN_1002dca0 (slot 6 gets the share each category received):
//   engines (3) ← P, then 0 ← the rest, sensors | shields (2 | 5), weapons (4), and last the batteries (1)
//   give up what was used: ratio = used / offered.
//
// The force shield (class 9, ctor 10025460) keeps six side charges (+0x98, fractions of its capacity
// param0 · +0x11c, 1.0 at construction); with the deflector (class 0x15, a generic device whose params 0..5
// weigh the sides) it is the unit's shield (CDeviceSet ctl+0x5cc / +0x5d0; notes/weapons-damage.md §4.2).
//
// Port choices (inferred where marked):
// - the shield's +0x11c (0x204 SetProperty 0x11, which also sets the guns' dev+0x180) keeps its ctor value 1
//   (no caller of property 0x11 traced); the per-side change records (+0xb0, network) are not kept;
// - repair (type 0xf) heals through DamageState::apply_damage_total, the replay lock (ctl+0x618) is never
//   set, and the device's "include destroyed parts" byte (+0x94, 0 at construction) is never changed;
// - the slow tick has no jitter (combat_world.hpp).
#pragma once

#include "combat/damage.hpp"
#include "combat/gun.hpp"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace parkan::combat {

// One device of the control as the budget sees it (CDevice fields).
struct PowerDevice {
    std::uint32_t cls = 0;     // the CreateDevice case (the type the device was created with): its class
    std::uint32_t type = 0;    // spec +0x00 (dev+0x48 → +0): the category; a module (INTO) header may change it
    std::int32_t part = -1;    // dev+4: damage part (combat world index), −1 none
    std::uint32_t state = 0;   // dev+0x50: 0 = off (the class constructor's value, or spec +0x18 when ≠ −1)
    float power = 0;           // spec +0x20: idle draw per second
    std::array<float, 16> param{}; // spec +0x2c (dev+0x54)
    float ratio = 1;           // dev+0x4c: the share of its need it last received (a battery's charge)
    float usage = 0;           // type 0x1a dev+0xa4: power usage (device property 0x500, SetPowerUsage)
    std::int32_t gun = -1;     // a gun device: index into the unit's guns (its energy lives there)
    std::array<float, 6> sides{1, 1, 1, 1, 1, 1}; // class 9 +0x98: the side charges, ±y, −x, +x, ±z (0..1)
    ResName effect;            // header +0x6c / +0x8c (slot 9, 10025600): the shield's hit effect
    // The device's state after CDevice::Load: class default, then spec +0x18 unless −1.
    static std::uint32_t initial_state(std::uint32_t cls, std::int32_t spec_state);
};

// 1003ccc8: device type → budget category (0..5).
std::uint32_t energy_category(std::uint32_t type);

// What the devices read from their control.
struct EnergyHost {
    // CControlBase part value 1 (1000dc40): part record +0x14, the HP ratio. 0 = the device is broken.
    std::function<float(std::int32_t part)> part_ratio;
    // The engine's load (100265c0): max|local velocity| (ctl+0x1c8) / max|file body[6..8]| · the current
    // motion state's energy factor (ctl+0x154 = state +0x54).
    float engine_load = 0;
    DamageState* damage = nullptr; // repair
    float now_ms = 0;
};

class EnergyBudget {
public:
    std::vector<PowerDevice> devices;
    float drain_a = 0, drain_b = 0; // ctl+0x5dc / +0x5e0 (0x204 properties 3 / 4): −(b2+b5+b0)/dt, −(b4+b3+b0)/dt

    // UpdateEnergyBudget(dt) with dt in seconds.
    void update(float dt_s, std::vector<GunDevice>& guns, const EnergyHost& host);

    // CDeviceSet::GetProperty (1002b410) over the batteries (type 0x13):
    // 1 = Σ GetParam(0x200) / Σ GetParam(0) (1.0 when a battery has capacity < 0; nullopt without
    // capacity) — unit property 0x73, the HUD energy gauge (GetPropertyImpl 1000e3d1);
    std::optional<float> battery_fraction() const;
    // 2 = Σ capacity (nullopt when the sum is 0; a battery with capacity < 0 returns that capacity).
    std::optional<float> battery_capacity() const;
    // SetProperty(1, f) (1002ba30): every battery's charge := f (the economy distributor's refill).
    void set_battery_fraction(float f);
    // 0x204 GetProperty(7) / SetProperty(7, v) (the room's shield recharge, Room_RechargeLifeAndShield 10018100): the
    // mean side charge; the setter spreads the difference over the sides (FUN_10025a90). nullopt without a shield.
    std::optional<float> shield_mean() const;
    void set_shield_mean(float v);
    // The i_dsh signature factors of unit properties 0x45 / 0x43 / 0x44 (unit-stats.md §3), i = 0 / 1 / 2: the
    // product over the type-0xa devices of min(1, GetParam(0x300 + i)), × min(1, GetParam(3)) in state 0x1000
    // [the second factor: inferred reading]; 1 without such a device.
    float signature_factor(std::size_t i, const EnergyHost& host) const;
    // SetPowerUsage (Behavior 10019880): device property 0x500 := u on every type-0x1a device.
    void set_power_usage(float u);
    // GetPowerFactor (Behavior 100198e0): Σ device property 0xe00 over the type-0x1a devices =
    // GetParam(0x300) (GetDevicePropertyById 1002bb40).
    float power_factor(const EnergyHost& host) const;

    // CDevice::GetParam (10021d00): param[f & 0xff], × the part's HP ratio (0x100), × dev+0x4c (0x200).
    static float get_param(const PowerDevice& d, std::uint32_t f, const EnergyHost& host);

    // The shield (CDeviceSet +0x5c0 = ctl+0x5cc, class 9) and the deflector (+0x5c4 = ctl+0x5d0, class 0x15):
    // CreateDevice keeps the last one of each class [inferred: as for the armour]. −1 = none.
    std::int32_t shield = -1, deflector = -1;
    std::int32_t radar = -1; // ctl+0x5d4 (CDeviceSet +0x5c8): the last class-8 device created (1002d5b8)
    float shield_mult = 1; // shield +0x11c
    // IsShieldActive (1002c500), the device half: both exist, are on (dev+0x50 ≠ 0) and not broken.
    bool shield_active(const EnergyHost& host) const;
    // GetShieldSideStrength (1002ca30): deflector GetParam(0x300 | side) · shield GetParam(0) · charge(side);
    // 0 without both devices.
    float side_strength(std::size_t side, const EnergyHost& host) const;
    // DrainShieldSide (1002ca80): side `side`'s strength becomes `strength` (FUN_10025be0, clamped to the
    // capacity); nothing when the deflector's weight of that side is ≤ 0.
    void set_side_strength(std::size_t side, float strength, const EnergyHost& host);
    // Unit property 0x79 (0x204 slot 0x2c, 1002c430): charge(side) · the deflector's part HP ratio · its
    // dev+0x4c, the HUD's shield sides (CShieldDisplay_Draw); nullopt without both devices.
    std::optional<std::array<float, 6>> shield_charges(const EnergyHost& host) const;
    // GetShieldSide's side choice (1002c590) for `d` = hit point − shield centre in the unit frame: 0 +y
    // (front), 1 −y, 2 −x, 3 +x, 4 +z, 5 −z, by the dominant axis (ties to the lower side).
    static std::size_t shield_side(Vec3 d);

private:
    float demand(const PowerDevice& d, const std::vector<GunDevice>& guns, float dt, const EnergyHost& host) const;
    void grant(PowerDevice& d, std::vector<GunDevice>& guns, float dt, float ratio, const EnergyHost& host);
    float pass(std::uint32_t mask, float avail, std::vector<GunDevice>& guns, float dt, const EnergyHost& host);
    bool broken(const PowerDevice& d, const EnergyHost& host) const;
    float shield_capacity(const PowerDevice& d) const { return d.param[0] * shield_mult; } // GetParam(0), 100255c0
    static void spread_charge(PowerDevice& d, float x); // FUN_10025a90
    std::array<float, 6> bucket_{}; // DAT_100486cc
};

} // namespace parkan::combat
