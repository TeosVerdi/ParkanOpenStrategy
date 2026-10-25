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
// Port choices (inferred where marked):
// - shields (type 9) keep their six side charges full (absorption is not modelled, combat/damage.hpp), so
//   their need is the idle draw only; the under-powered drain of the charges (FUN_10025a90) is deferred;
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
    // SetPowerUsage (Behavior 10019880): device property 0x500 := u on every type-0x1a device.
    void set_power_usage(float u);
    // GetPowerFactor (Behavior 100198e0): Σ device property 0xe00 over the type-0x1a devices =
    // GetParam(0x300) (GetDevicePropertyById 1002bb40).
    float power_factor(const EnergyHost& host) const;

    // CDevice::GetParam (10021d00): param[f & 0xff], × the part's HP ratio (0x100), × dev+0x4c (0x200).
    static float get_param(const PowerDevice& d, std::uint32_t f, const EnergyHost& host);

private:
    float demand(const PowerDevice& d, const std::vector<GunDevice>& guns, float dt, const EnergyHost& host) const;
    void grant(PowerDevice& d, std::vector<GunDevice>& guns, float dt, float ratio, const EnergyHost& host);
    float pass(std::uint32_t mask, float avail, std::vector<GunDevice>& guns, float dt, const EnergyHost& host);
    bool broken(const PowerDevice& d, const EnergyHost& host) const;
    std::array<float, 6> bucket_{}; // DAT_100486cc
};

} // namespace parkan::combat
