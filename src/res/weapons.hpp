// Weapon data: the CTLD weapon devices (type 2 / 0x1e), the bullet prototypes they name (objects.rlb
// BULL → weapon.rlb CTLD body + NDPR + EXPL) and the NDPR damage parts every object carries.
// Spec: notes/weapons-damage.md §1 (layouts), notes/formats-wea-cpt-ndp.md "NDPR";
// reference parser: tools/weapons_dump.py (same corpus totals expected).
#pragma once

#include "res/ctld.hpp"
#include "res/fx.hpp"
#include "res/library.hpp"

#include <array>
#include <optional>
#include <utility>

namespace parkan {


// CDevice::Load (10021d50) for a weapon device (CGunDevice::Load 10029650): the 0xb0-byte header.
struct GunDeviceSpec {
    std::uint32_t type = 2;    // +0x00: 2 gun, 0x1e building gun (same class)
    std::int32_t id = 0;       // +0x04
    std::uint32_t flags = 0;   // +0x08: 0x2000000 salvo (all barrels step together)
    std::int32_t event = -1;   // +0x0c: passed to FUN_100221e0 at the recoil step (sound/event, inferred)
    std::int32_t part = -1;    // +0x18: mesh part whose destruction breaks the device (inferred)
    std::array<float, 16> param{}; // +0x2c: 0 ammo (−1 ∞), 1 energy buffer, 2 energy/shot, 3 delay ms, 4 muzzle delay ms
    ResName bullet;            // +0x6c / +0x8c: objects.rlb BULL
    std::vector<std::uint32_t> barrels; // joint refs (channels)
    std::string module;        // unit-module text (e.g. "i_c05_b"), cp1251 as stored
    static constexpr std::uint32_t kSalvo = 0x2000000;
};
bool is_weapon_device(const CtldDevice& d);
GunDeviceSpec decode_gun_device(const CtldDevice& d);

// NDPR: one damage record per mesh node, 4 + 76·n bytes (strict).
struct NdprPart {
    std::uint32_t flags = 0; // 0x70 vital (projectiles), 0x20/0x40 left/right locomotion, 0x1 scenery
    float hit_points = 0;
    float density = 0;
    ResName effect;          // destruction effect (.exp); present when both names are non-empty
    bool has_effect() const { return !effect.first.empty() && !effect.second.empty(); }
};
std::vector<NdprPart> decode_ndpr(std::span<const std::uint8_t> data);

// EXPL: decode_expl / ExplosionDef in res/fx.hpp (the one EXPL decoder).
using Expl = ExplosionDef;

// objects.rlb BULL: 5 × (lib[32], entry[32]) = msh, wea, cpt, ndp, ctl.
struct BullRecord {
    std::array<ResName, 5> pairs;
    const ResName& mesh() const { return pairs[0]; }
    const ResName& ndpr() const { return pairs[3]; }
    const ResName& ctld() const { return pairs[4]; }
};
BullRecord decode_bull(std::span<const std::uint8_t> data);

// A bullet prototype resolved down to its data (weapons_dump.py Game.bullet).
struct BulletProto {
    std::string name;
    BullRecord record;
    float speed = 0;              // body[7] m/s
    float range = 0;              // body[22] m
    std::array<float, 3> turn{};  // body[9..11] max homing turn rates (rad/s)
    std::int32_t removal_ms = 0;  // body[18] (int): delay between "dead" and removal
    std::int32_t physics = 0;     // body[21] (int): 0 free flight, 3 ballistic (gravity)
    std::vector<NdprPart> parts;
    std::optional<Expl> expl;     // part 0's effect: the hit explosion
    // The bullet CTLD's situation lists (RunMotionEvents 10002800): situation 0 starts effects (event
    // type 4: tracer, engine, muzzle flash; their sound commands are the gun's sound), situation 4 (range
    // exhausted) may name a substitute explosion (event 0x1b).
    std::vector<ResName> spawn_effects;
    // Per spawn effect: its event's three CTPT points (raw +0x10/+0x14/+0x18, the bullet's own .cpt,
    // space 1 = the point's part frame at scale 1). fx::contact_frame turns them into the effect's
    // local frame (bullets: ×0.03, X backwards).
    struct SpawnPoints {
        std::array<Vec3, 3> pos{}, vec{};
    };
    std::vector<SpawnPoints> spawn_points;
    std::optional<Expl> range_expl;
    ResName range_expl_name;
    std::optional<std::array<float, 3>> seeker; // device 0x11 params: cone half-angle (rad), range (m), lock (ms)
    float hit_points() const { return parts.empty() ? 0.f : parts[0].hit_points; } // NDPR part 0
    // Damage of one hit with multiplier `mult` (spec §0): mult·EXPL.damage + bullet HP (× mult).
    float hit_damage(float mult = 1) const { return mult * ((expl ? expl->damage : 0.f) + hit_points()); }
};
// Throws FormatError when any piece is missing or malformed.
BulletProto load_bullet(AssetLibrary& lib, std::string_view name);

// Corpus scan of every NRes .rlb in the game folder (weapons_dump.py --check).
struct WeaponCorpus {
    std::size_t weapon_devices = 0; // CTLD devices of type 2 / 0x1e
    std::size_t bullets = 0;        // BULL records that resolve
    std::size_t bull_records = 0;
    std::size_t expl_entries = 0;   // weapon.rlb EXPL entries
    std::size_t ndpr_entries = 0, ndpr_parts = 0;
    std::vector<std::string> unresolved; // BULL records no weapon uses whose data is missing
};
// Throws FormatError when a weapon device's bullet does not resolve or any record is malformed.
WeaponCorpus scan_weapon_corpus(AssetLibrary& lib);

} // namespace parkan
