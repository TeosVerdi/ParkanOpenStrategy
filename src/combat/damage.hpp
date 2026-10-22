// Hit points, damage records and death: Control.dll CControlBase's damage-part table (NDPR parts at
// ctl+0x558, totals +0x58c/+0x590/+0x594) and the functions that change it. Spec:
// notes/weapons-damage.md §3.3 (records), §4 (damage), §5 (death); notes/formats-wea-cpt-ndp.md "NDPR".
//
// Not modelled: force shields (AbsorbByShield 1000ff00: every shield-side test is "no shield"),
// debris (DetachPartDebris), the replay lock (ctl+0x618), break-off states (runtime flags 0x40..0x100).
#pragma once

#include "core/math.hpp"
#include "res/weapons.hpp"

#include <optional>

namespace parkan::combat {

inline constexpr std::uint32_t kKilledClan = 0xFFFE; // DETID_KILLED: GetClan after death

// The damage record (0x48 bytes, pushed by UpdatePartStage 10011220, consumed by ProcessDamageQueue
// 10012ce0). Object references are indices into the combat world's units; -1 = none (bullets, statics).
struct DamageRecord {
    std::uint32_t kind = 1;       // *EXPL: 1 effect only, 2 direct, 3 splash, 4 splash with 0 part damage
    std::int32_t source = -1, source_part = -1; // the exploding object (skipped by its own record)
    std::int32_t target = -1, target_part = -1; // the face hit (kind 2 damages only this part)
    std::int32_t attacker = -1;   // ctl+0x54c: the shooter
    Vec3 pos{};                   // +0x1c
    float damage = 0;             // +0x34
    float radius = 0;             // +0x38
};

// DamagePartFromRecord's splash term (10010030) for a part sphere at distance d with radius r and a
// record of radius R: kind 3 → 0 when r == 0 or d ≥ R + r, full when one sphere contains the other
// (d < |r − R|), else dmg·((R + r − d)·0.5/R)³; kind 4 → 0; other kinds → dmg.
float splash_damage(std::uint32_t kind, float dmg, float d, float r, float R);

// Armour device (type 0x1b, ctl+0x5b4): dmg' = clamp(dmg·(a + b·dmg), 0, dmg). a = param1, b = param2,
// m = param0 (mass per part area, not modelled).
struct Armour {
    float a = 1, b = 0, m = 0;
    float apply(float dmg) const;
};

// One runtime part (0x2c bytes, FUN_1000b240 / BindDamageParts 1000f940).
struct DamagePart {
    bool present = false;      // has an NDPR record; component roots are merged into their mount (alias)
    std::int32_t alias = -1;   // hits on this transform count on that part
    std::int32_t parent = -1;  // +0x04 (resolved through aliases)
    std::uint32_t ndpr_flags = 0;
    std::uint32_t flags = 0;   // +0x28: 1 vital (mesh node 0x200), 2 (0x100), 4 ratio from parent (0x400),
                               //        8 stage from parent (0x1 & !0x80), 0x10 destroyed
    float max_hp = 0;          // GetPartValue(3) = hitPoints × mult × scale
    float hp = 0, prev_hp = 0; // +0x08 / +0x0c
    float ratio = 1;           // +0x14
    std::uint32_t stage = 0;   // +0x18
    std::uint32_t stages = 1;  // +0x1c: IAniMesh slot 0x48 (port: present damage states − 1, min 1; inferred)
    std::int32_t effect = -1;  // into DamageState::effects
    Vec3 sphere_centre{};      // the mesh part's bounding sphere (IAniMesh +0xc, 2), node-local
    float sphere_radius = 0;
    bool destroyed() const { return (flags & 0x10) != 0; }
};

// A part's damage stage rose and it has an effect: UpdatePartStage's explosion. The caller places it
// (part matrix) and turns it into a DamageRecord.
struct PartExplosion {
    std::int32_t part = -1;
    std::int32_t effect = -1;
    float damage = 0; // mult·EXPL.damage + previous HP − HP
};

class DamageState {
public:
    std::vector<DamagePart> parts;
    std::size_t root = 0;                        // part 0 of the object (the chassis root node)
    std::vector<std::pair<ResName, Expl>> effects; // distinct NDPR effects
    float total = 0, initial = 0, threshold = 0; // +0x590 / +0x58c / +0x594
    float threshold_factor = 0.2f;               // 100063c5: 0.2 units, 0.3 class-10 statics
    float mult = 1;                              // ctl+0x660 (property 0xb4)
    std::uint32_t clan = 0;                      // ctl+0x550
    bool invulnerable = false;                   // ctl+0x5b0 (property 0xa2)
    bool player = false;                         // class 3 (the hero): never removed
    std::int32_t removal_delay_ms = 0;           // body[18]
    std::optional<float> removal_at;             // ctl+0x59c
    bool killed = false;                         // part 0 / a vital part reached 0 (clan := 0xfffe)
    float killed_at = 0;
    bool dying = false;                          // +0x386: the slow tick's dying state
    bool changed = false;                        // +0x598
    std::optional<Armour> armour;
    std::vector<PartExplosion> explosions;       // pushed by update_part_stage, drained by the owner

    // Sums the totals after `parts` is filled (max HP set, hp = max).
    void finish_init();

    // AddPartHP (10010f30): clamp to [0, max], update the total; at 0 the part is destroyed, and part 0
    // (`root`) or a vital (flag 1) part kills the object: clan 0xfffe, removal at now + body[18] (not for the
    // player), part 0 emptied and its stage updated (the death explosion).
    void add_part_hp(std::size_t part, float delta, float now_ms);
    // ApplyDamageTotal (10010ba0): damage spread over the parts by their HP (healing by their missing
    // HP); `sequential` takes damage from the last part backwards and heals from the first.
    void apply_damage_total(float amount, bool sequential, bool include_destroyed, float now_ms);
    // PropagatePartDestruction (10011130), port reading: the parts under a destroyed part are destroyed.
    void propagate_destruction(float now_ms);
    // UpdatePartStage (10011220): stage = stages − ceil(stages·ratio); a rise with an effect pushes a
    // PartExplosion when `explode`.
    void update_part_stage(std::size_t part, bool explode);
    // DamagePartFromRecord (10010030) with the part's world sphere.
    void damage_part_from_record(std::size_t part, std::int32_t self, const DamageRecord& rec, Vec3 centre, float radius,
                                 float now_ms);
    // SlowTickDamage (10012a40), `dt_s` seconds since the last call: the vital-parts / threshold test
    // starts the dying state, which bleeds 0.1·threshold per second (100424b0) until part 0 dies.
    void slow_tick(float dt_s, float now_ms);

    std::size_t resolve(std::size_t transform) const; // alias → part
    float hp_ratio() const { return initial > 0 ? total / initial : 0.f; }
};

} // namespace parkan::combat
