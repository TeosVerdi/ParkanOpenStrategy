#include "combat/energy.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::combat {

namespace {

// 1003ccc8 (32 entries).
constexpr std::array<std::uint8_t, 32> kCategory{0, 4, 4, 0, 2, 3, 0, 0, 2, 5, 5, 0, 0, 0, 1, 0,
                                                 3, 2, 0, 1, 3, 5, 4, 0, 4, 0, 3, 5, 0, 0, 4, 0};

// FUN_10010b10(flag 0): Σ (max − hp) over the parts with hp > 0.
float missing_hp(const DamageState& d) {
    float m = 0;
    for (const auto& p : d.parts)
        if (p.present && p.hp > 0) m += p.max_hp - p.hp;
    return m;
}

} // namespace

std::uint32_t energy_category(std::uint32_t type) { return type < kCategory.size() ? kCategory[type] : 0; }

std::uint32_t PowerDevice::initial_state(std::uint32_t cls, std::int32_t spec_state) {
    if (spec_state != -1) return std::uint32_t(spec_state); // CDevice::Load: dev+0x50 = spec +0x18
    switch (cls) {
    case 1: case 4: return 0x200;           // CTurretDevice 10027050, CCameraDevice 100233d0
    case 2: case 0x1e: case 0xf: case 0x1b: return 0; // CGunDevice (the fire command), repair 10022ae0, CDevice
    case 9: case 0x11: case 0x13: return 0x20; // shield 10025460, seeker 10024760, battery 10022980
    case 0xa: return 0x2000;                // 10026470
    default: return 5;                      // CGenericDevice 10020800 (engines, type 8, 0x15, 0x1a, …)
    }
}

bool EnergyBudget::broken(const PowerDevice& d, const EnergyHost& host) const {
    // CDevice slot 2 (10021820): the part's ratio is 0.
    return d.part >= 0 && host.part_ratio && host.part_ratio(d.part) == 0;
}

float EnergyBudget::get_param(const PowerDevice& d, std::uint32_t f, const EnergyHost& host) {
    const std::size_t i = f & 0xff;
    float v = i < d.param.size() ? d.param[i] : 0.f;
    if ((f & 0x100) && d.part >= 0 && host.part_ratio) v *= host.part_ratio(d.part);
    if (f & 0x200) v *= d.ratio;
    return v;
}

// Device slot 5 (vtable +0x14).
float EnergyBudget::demand(const PowerDevice& d, const std::vector<GunDevice>& guns, float dt, const EnergyHost& host) const {
    const bool off = broken(d, host);
    if (d.gun >= 0) return guns[std::size_t(d.gun)].energy_deficit(dt, off); // 10029a40
    if (d.state == 0 || off) return 0;
    switch (d.cls) {
    case 0x13: { // battery 100229a0: offers power·charge·health·dt, at most what it holds
        const float x = host.part_ratio && d.part >= 0 ? host.part_ratio(d.part) : 1.f;
        const float t = d.power * d.ratio * x * dt;
        if (get_param(d, 0, host) <= 0) return t; // capacity ≤ 0 (−1: an endless battery)
        return std::min(t, get_param(d, 0x200, host) * x);
    }
    case 5: // engine 100265c0
        return -(host.engine_load * d.power * dt);
    case 0x1a: // 1002e540: building power consumer
        return -((d.power + d.usage) * dt);
    case 0xf: { // repair 10022b20: idle draw + param1 per HP of min(missing HP, param0·health·dt)
        const float base = -(dt * d.power);
        const float h = get_param(d, 0x100, host) * dt;
        const float m = host.damage ? missing_hp(*host.damage) : 0.f;
        return base - get_param(d, 1, host) * (m < h ? m : h);
    }
    case 0xa: { // 100264b0: param4 per second more in state 0x1000
        float v = -(dt * d.power);
        if (d.state == 0x1000) v -= get_param(d, 4, host) * dt;
        return v;
    }
    default: // CDevice 10021860 (also the shield's need with full charges: 10025700, see the file comment)
        return -(dt * d.power);
    }
}

// Device slot 6 (vtable +0x18).
void EnergyBudget::grant(PowerDevice& d, std::vector<GunDevice>& guns, float dt, float ratio, const EnergyHost& host) {
    const bool off = broken(d, host);
    if (d.gun >= 0) {
        guns[std::size_t(d.gun)].add_energy(dt, ratio, off); // 10029a90
        return;
    }
    if (d.state == 0 || off) return;
    switch (d.cls) {
    case 0x13: { // 10022a60: the charge falls by what was used of the offer
        const float cap = get_param(d, 0, host);
        if (cap < 0) return;
        const float x = host.part_ratio && d.part >= 0 ? host.part_ratio(d.part) : 1.f;
        const float used = demand(d, guns, dt, host) * ratio;
        if (cap * x == 0) return; // port guard: the original divides by zero (no data has capacity 0)
        d.ratio -= used / (cap * x);
        return;
    }
    case 0xf: { // 10022bb0
        d.ratio = ratio;
        const float e = -(demand(d, guns, dt, host) * ratio) - dt * d.power;
        if (e < 0 || !host.damage) return;
        const float p1 = get_param(d, 1, host);
        float amount;
        if (p1 > 0) amount = e / p1;
        else {
            const float h = get_param(d, 0x100, host) * dt, m = missing_hp(*host.damage);
            amount = m < h ? m : h;
        }
        host.damage->apply_damage_total(amount, true, false, host.now_ms); // ApplyDamageTotal 10010ba0
        return;
    }
    case 0xa: { // 10026510: switches itself to 0x2000 when under-supplied in state 0x1000
        d.ratio = ratio;
        const float e = -(demand(d, guns, dt, host) * ratio) - dt * d.power;
        if (e < 0 && d.state == 0x1000) d.state = 0x2000;
        return;
    }
    default: // CDevice 10021890 (engines, 0x1a, shields: see the file comment)
        d.ratio = ratio;
        return;
    }
}

float EnergyBudget::pass(std::uint32_t mask, float avail, std::vector<GunDevice>& guns, float dt, const EnergyHost& host) {
    // FUN_1002dca0: D = −Σ the masked buckets; ratio = min(1, avail / D); return avail − ratio·D.
    float need = 0;
    for (std::uint32_t c = 0; c < 6; ++c)
        if (mask & (1u << c)) need -= bucket_[c];
    if (need == 0) return avail;
    float ratio = avail / need;
    if (1.f < ratio) ratio = 1;
    const float used = ratio * need;
    for (std::uint32_t c = 0; c < 6; ++c)
        if (mask & (1u << c)) bucket_[c] *= ratio;
    for (auto& d : devices)
        if (mask & (1u << energy_category(d.type))) grant(d, guns, dt, ratio, host);
    return avail - used;
}

void EnergyBudget::update(float dt, std::vector<GunDevice>& guns, const EnergyHost& host) {
    // UpdateEnergyBudget (1002d340).
    bucket_.fill(0);
    float offered = 0;
    for (const auto& d : devices) {
        const float v = demand(d, guns, dt, host);
        if (v > 0) offered += v;
        bucket_[energy_category(d.type)] += v;
    }
    float r = pass(1u << 3, offered, guns, dt, host);  // engines
    r = pass(1u << 0, r, guns, dt, host);               // misc (repair, …)
    r = pass(1u << 2 | 1u << 5, r, guns, dt, host);     // sensors, shields
    r = pass(1u << 4, r, guns, dt, host);               // weapons, turrets
    pass(1u << 1, r - offered, guns, dt, host);         // batteries: −(what was used)
    const float k = dt > 0.0001f ? 1.f / dt : 0.f;      // 1003bc8c
    drain_a = -((bucket_[2] + bucket_[5] + bucket_[0]) * k);
    drain_b = -((bucket_[4] + bucket_[3] + bucket_[0]) * k);
}

std::optional<float> EnergyBudget::battery_fraction() const {
    // GetProperty(1): GetParam(0x200) / GetParam(0) (dev+0x4c is not scaled by the part's health here).
    float charge = 0, cap = 0;
    for (const auto& d : devices) {
        if (d.type != 0x13) continue;
        if (d.param[0] < 0) return 1.f;
        charge += d.param[0] * d.ratio;
        cap += d.param[0];
    }
    if (cap > 0) return charge / cap;
    return std::nullopt;
}

std::optional<float> EnergyBudget::battery_capacity() const {
    float cap = 0;
    for (const auto& d : devices) {
        if (d.type != 0x13) continue;
        if (d.param[0] < 0) return d.param[0];
        cap += d.param[0];
    }
    if (cap > 0) return cap;
    return std::nullopt;
}

void EnergyBudget::set_battery_fraction(float f) {
    for (auto& d : devices)
        if (d.type == 0x13) d.ratio = f;
}

void EnergyBudget::set_power_usage(float u) {
    for (auto& d : devices)
        if (d.type == 0x1a) d.usage = u;
}

float EnergyBudget::power_factor(const EnergyHost& host) const {
    float pf = 0;
    for (const auto& d : devices)
        if (d.type == 0x1a) pf += get_param(d, 0x300, host);
    return pf;
}

} // namespace parkan::combat
