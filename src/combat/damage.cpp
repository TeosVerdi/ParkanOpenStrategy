#include "combat/damage.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::combat {

float splash_damage(std::uint32_t kind, float dmg, float d, float r, float R) {
    if (kind == 4) return 0;
    if (kind != 3) return dmg;
    if (r == 0 || d >= R + r) return 0;
    if (d < std::abs(r - R)) return dmg;
    const float f = (R + r - d) * 0.5f / R;
    return dmg * f * f * f;
}

float Armour::apply(float dmg) const { return std::clamp(dmg * (a + b * dmg), 0.f, dmg); }

std::size_t DamageState::resolve(std::size_t t) const {
    while (t < parts.size() && parts[t].alias >= 0) t = std::size_t(parts[t].alias);
    return t;
}

void DamageState::finish_init() {
    total = initial = 0;
    for (const auto& p : parts)
        if (p.present) total += p.hp;
    initial = total;
    threshold = initial * threshold_factor;
}

void DamageState::add_part_hp(std::size_t i, float delta, float now) {
    if (delta == 0 || i >= parts.size() || !parts[i].present) return;
    DamagePart& p = parts[i];
    const float old = p.hp;
    p.hp = std::clamp(p.hp + delta, 0.f, p.max_hp);
    if (p.hp == old) return;
    total = std::max(0.f, total + (p.hp - old));
    changed = true;
    p.ratio = p.max_hp > 0 ? p.hp / p.max_hp : 0.f; // also sent to the mesh as part value 0x200
    if (p.hp != 0) {
        p.flags &= ~0x10u;
        return;
    }
    const std::uint32_t was = p.flags;
    p.flags |= 0x10;
    if ((i == root || (was & 1)) && clan != kKilledClan) {
        clan = kKilledClan;
        killed = true;
        killed_at = now;
        // Class 3 (the player's robot) stops here (10010f30: the rest is under `class != 3`): no removal
        // time, it stays in collision, part 0 is not emptied nor its stage updated here (the hit's own
        // UpdatePartStage in OnDamageRecord still runs). State flag 0x10000000 (no auto-removal) is not
        // modelled.
        if (player) return;
        removal_at = now + float(removal_delay_ms);
        add_part_hp(root, -total, now);
        update_part_stage(root, true);
    }
}

void DamageState::apply_damage_total(float amount, bool sequential, bool include_destroyed, float now) {
    if (amount == 0) return;
    float avail, eff;
    if (amount <= 0) {
        avail = -total;
        eff = std::max(amount, avail);
    } else {
        if (include_destroyed) {
            avail = initial - total;
        } else {
            avail = 0;
            for (const auto& p : parts)
                if (p.present && !p.destroyed()) avail += p.max_hp - p.hp;
        }
        eff = std::min(amount, avail);
    }
    if (avail == 0) return;
    if (!sequential) {
        const float k = eff / avail;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!parts[i].present) continue;
            if (amount <= 0) add_part_hp(i, -(k * parts[i].hp), now);
            else if (include_destroyed || !parts[i].destroyed()) add_part_hp(i, (parts[i].max_hp - parts[i].hp) * k, now);
        }
    } else if (amount <= 0) {
        float rest = amount;
        for (std::size_t i = parts.size(); i-- > 0 && rest < 0;) {
            if (!parts[i].present) continue;
            const float take = std::min(-rest, parts[i].hp);
            add_part_hp(i, -take, now);
            rest += take;
        }
    } else {
        float rest = amount;
        for (std::size_t i = 0; i < parts.size() && rest > 0; ++i) {
            if (!parts[i].present || (!include_destroyed && parts[i].destroyed())) continue;
            const float give = std::min(rest, parts[i].max_hp - parts[i].hp);
            add_part_hp(i, give, now);
            rest -= give;
        }
    }
    propagate_destruction(now);
}

void DamageState::propagate_destruction(float now) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
        DamagePart& p = parts[i];
        if (!p.present || p.destroyed() || p.parent < 0 || !parts[std::size_t(p.parent)].destroyed()) continue;
        if (p.hp > 0) add_part_hp(i, -p.hp, now);
        else p.flags |= 0x10;
    }
}

void DamageState::update_part_stage(std::size_t i, bool explode) {
    if (i >= parts.size() || !parts[i].present) return;
    DamagePart& p = parts[i];
    const DamagePart* parent = p.parent >= 0 ? &parts[std::size_t(p.parent)] : nullptr;
    if ((p.flags & 4) && parent) p.ratio = parent->ratio;
    std::uint32_t st;
    if ((p.flags & 8) && parent) st = parent->stage;
    else st = p.stages - std::uint32_t(std::ceil(float(p.stages) * p.ratio));
    st = std::min(st, p.stages);
    if (st != p.stage) {
        if (explode && st > p.stage && p.effect >= 0)
            explosions.push_back({std::int32_t(i), p.effect, mult * effects[std::size_t(p.effect)].second.damage + p.prev_hp - p.hp});
        p.stage = st;
    }
    p.prev_hp = p.hp;
}

void DamageState::damage_part_from_record(std::size_t i, std::int32_t self, const DamageRecord& rec, Vec3 centre, float radius,
                                          float now) {
    if (i >= parts.size() || !parts[i].present) return;
    const DamagePart& p = parts[i];
    if (p.destroyed() || (rec.source == self && rec.source_part == std::int32_t(i))) return;
    if (p.parent >= 0 && parts[std::size_t(p.parent)].destroyed()) return;
    float dmg = rec.damage;
    if (rec.kind != 2) {
        const Vec3 d = rec.pos - centre;
        dmg = splash_damage(rec.kind, dmg, std::sqrt(dot(d, d)), radius, rec.radius);
    }
    if (armour) dmg = armour->apply(dmg);
    if (dmg != 0) add_part_hp(i, -dmg, now);
}

void DamageState::slow_tick(float dt_s, float now) {
    if (invulnerable || !changed) return;
    changed = false;
    float sum = 0, count = 0;
    for (const auto& p : parts)
        if (p.present && (p.ndpr_flags & 0x70)) sum += p.ratio, count += 1;
    const bool to_die = count > 1 ? sum <= count * 0.5f - 1 : total < threshold;
    if (to_die) {
        if (!dying) {
            dying = true; // RunMotionEvents(situation 6) is not modelled
            threshold = total;
        }
        apply_damage_total(-(0.1f * threshold * dt_s), true, false, now);
    } else if (dying) {
        dying = false; // situation 7
    }
}

} // namespace parkan::combat
