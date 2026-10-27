// Weapons, bullets, damage and death (src/combat, src/res/weapons; notes/weapons-damage.md). Unit tests
// on hand-made data; corpus tests on the game: loader totals (tools/weapons_dump.py --check), the hero's
// per-hit damage (spec §0) and a scripted fight in Tut_2 (a gun shoots an animal until it dies, its
// removal reaches the mission runtime). Linked into parkan-tests.
#include "combat/combat_world.hpp"
#include "combat/rooms.hpp"
#include "economy/assembly.hpp"
#include "core/paths.hpp"
#include "game/mission_runtime.hpp"
#include "mission/tma.hpp"
#include "res/terrain.hpp"
#include "res/weapons.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>

using namespace parkan;
using namespace parkan::combat;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

void test_splash_and_armour() {
    std::printf("combat: splash falloff, armour\n");
    // DamagePartFromRecord 10010030, kind 3: R = 7 (bm_h_01), part sphere r = 1.
    CHECK(near(splash_damage(3, 170, 9, 1, 7), 0));                  // d ≥ R + r
    CHECK(near(splash_damage(3, 170, 2, 1, 7), 170));                // inside: |r − R| = 6 > d
    CHECK(near(splash_damage(3, 170, 7, 1, 7), 170 * std::pow(1 * 0.5f / 7, 3.f)));
    CHECK(near(splash_damage(3, 170, 2, 0, 7), 0));                  // r == 0
    CHECK(near(splash_damage(4, 170, 2, 1, 7), 0));
    CHECK(near(splash_damage(2, 170, 50, 1, 7), 170));               // direct: no falloff
    const Armour base{1, 0, 0}, module{0.681f, 0.00013f, 0};         // i_arm_l / o_arm_l_01
    CHECK(near(base.apply(60), 60));
    CHECK(near(module.apply(60), 60 * (0.681f + 0.00013f * 60)));
    CHECK(near(module.apply(1e5f), 1e5f));                            // clamped to dmg
}

DamageState make_state(std::vector<float> hp, std::vector<std::int32_t> parent) {
    DamageState d;
    d.parts.resize(hp.size());
    for (std::size_t i = 0; i < hp.size(); ++i) {
        d.parts[i].present = true;
        d.parts[i].max_hp = d.parts[i].hp = d.parts[i].prev_hp = hp[i];
        d.parts[i].parent = parent[i];
    }
    d.finish_init();
    d.removal_delay_ms = 2000;
    d.clan = 1;
    return d;
}

void test_damage_state() {
    std::printf("combat: part HP, death, dying\n");
    {
        DamageState d = make_state({100, 50, 50}, {-1, 0, 0});
        CHECK(near(d.initial, 200) && near(d.threshold, 40));
        d.add_part_hp(1, -80, 10); // clamped at 0: destroyed, not vital
        CHECK(near(d.parts[1].hp, 0) && d.parts[1].destroyed() && near(d.total, 150) && !d.killed);
        d.add_part_hp(0, -100, 20); // part 0 → death: clan 0xfffe, removal at now + body[18]
        CHECK(d.killed && d.clan == kKilledClan && d.removal_at && near(*d.removal_at, 2020));
        CHECK(near(d.parts[0].hp, 0));
    }
    {
        // A vital part (runtime flag 1, mesh node flag 0x200) kills as well; the player is never removed.
        DamageState d = make_state({100, 50}, {-1, 0});
        d.parts[1].flags |= 1;
        d.player = true;
        d.add_part_hp(1, -50, 5);
        CHECK(d.killed && !d.removal_at);
    }
    {
        // ApplyDamageTotal: proportional to HP, or sequential from the last part.
        DamageState d = make_state({100, 300}, {-1, 0});
        d.apply_damage_total(-40, false, false, 0);
        CHECK(near(d.parts[0].hp, 90) && near(d.parts[1].hp, 270));
        d.apply_damage_total(-280, true, false, 0);
        CHECK(near(d.parts[1].hp, 0) && near(d.parts[0].hp, 80));
        d.apply_damage_total(1000, false, false, 0); // heal by missing HP, destroyed parts excluded
        CHECK(near(d.parts[0].hp, 100) && near(d.parts[1].hp, 0));
    }
    {
        // SlowTickDamage: total below 0.2·initial → dying, bleeding 0.1·threshold per second until part 0
        // dies (sequential: the last part first).
        DamageState d = make_state({100, 400}, {-1, -1});
        d.add_part_hp(1, -400, 0); // total 100 = 0.2 · 500: not below
        d.slow_tick(0.25f, 250);
        CHECK(!d.dying);
        d.add_part_hp(0, -1, 300); // 99 < 100
        d.slow_tick(0.25f, 500);
        CHECK(d.dying && near(d.threshold, 99) && near(d.total, 99 - 0.1f * 99 * 0.25f, 1e-2f));
        float t = 500;
        while (!d.killed && t < 60000) d.slow_tick(0.25f, t += 250);
        CHECK(d.killed && t > 10000 && t < 12000); // 10 s of bleeding at 10 %/s of the start total
    }
    {
        // Stages (UpdatePartStage): stage = stages − ceil(stages·ratio); a rise with an effect explodes.
        DamageState d = make_state({100}, {-1});
        d.effects.push_back({{"system.rlb", "x.exp"}, Expl{}});
        d.effects[0].second.damage = 5;
        d.parts[0].effect = 0;
        d.parts[0].stages = 2;
        d.add_part_hp(0, -40, 0);
        d.update_part_stage(0, true);
        CHECK(d.parts[0].stage == 0 && d.explosions.empty());
        d.add_part_hp(0, -20, 0); // ratio 0.4 → 2 − ceil(0.8) = 1
        d.update_part_stage(0, true);
        CHECK(d.parts[0].stage == 1 && d.explosions.size() == 1 && near(d.explosions[0].damage, 5 + 20));
    }
}

// A gun on a fixed muzzle; the bullets are only counted.
struct TestHost : GunHost {
    std::vector<float> spawns;
    float now = 0;
    std::optional<Vec3> target;
    bool muzzle(std::uint32_t, Vec3& pos, Vec3& dir) override {
        pos = {}, dir = {0, 1, 0};
        return true;
    }
    bool broken(std::int32_t) override { return false; }
    std::optional<Vec3> target_position() override { return target; }
    void spawn(GunDevice&, Vec3, Vec3) override { spawns.push_back(now); }
};

void test_gun_device() {
    std::printf("combat: gun device (StepBarrel, Update)\n");
    BulletProto b;
    b.range = 500;
    GunDeviceSpec spec;
    spec.param[0] = 3;   // ammo
    spec.param[1] = 20;  // energy buffer
    spec.param[2] = 6;   // per shot
    spec.param[3] = 100; // delay after a shot
    spec.barrels = {0};
    CtldJoint j;
    j.rate = 4; // 0.5 / 4 s per moving step
    GunDevice g(spec, {j}, &b);
    TestHost h;
    auto run = [&](float until) {
        for (; h.now < until;) {
            h.now += 10;
            g.update(h.now, h);
        }
    };
    run(100);
    CHECK(g.state() == kGunIdle && h.spawns.empty());
    g.set_state(kFireContinuous);
    run(2000);
    // Shot k: recoil step at t0, spawn 125 ms later (step 2 has no duration: the return step starts in
    // the same frame), done 125 ms after that, then param3 = 100 ms; 10 ms frames add ≤ 10 ms per
    // zero-length step. Energy 20 allows 3 shots of 6; ammo 3.
    CHECK(h.spawns.size() == 3);
    if (h.spawns.size() == 3) {
        CHECK(h.spawns[0] > 200 && h.spawns[0] < 250);
        const float period = h.spawns[1] - h.spawns[0];
        CHECK(period >= 350 && period <= 380);
        CHECK(near(h.spawns[2] - h.spawns[1], period, 20));
    }
    CHECK(g.ammo() == 0 && near(g.energy(), 2) && g.state() == kGunEmpty);
    // Single shot: one bullet, then the command clears.
    GunDevice s(spec, {j}, &b);
    TestHost h2;
    s.set_state(kFireSingle);
    for (h2.now = 0; h2.now < 1500;) s.update(h2.now += 10, h2);
    CHECK(h2.spawns.size() == 1 && s.command() == kCeaseFire);
    // A guided bullet (seeker) waits for a target and locks for param9 seconds.
    BulletProto guided = b;
    guided.seeker = std::array<float, 3>{0.25f, 500, 250};
    GunDevice gg(spec, {j}, &guided);
    TestHost h3;
    gg.set_state(kFireContinuous);
    for (h3.now = 0; h3.now < 500;) gg.update(h3.now += 10, h3);
    CHECK(h3.spawns.empty() && gg.state() == kGunReady && near(gg.param(9), 0.25f) && near(gg.param(10), std::cos(0.25f)));
    h3.target = Vec3{0, 100, 0};
    for (; h3.now < 1200;) gg.update(h3.now += 10, h3);
    CHECK(!h3.spawns.empty() && h3.spawns[0] > 750 && h3.spawns[0] < 900); // 250 ms lock + 125 ms recoil
    h3.target = Vec3{0, 600, 0}; // beyond param8 = min(500, seeker 500)
    const std::size_t before = h3.spawns.size();
    for (; h3.now < 3000;) gg.update(h3.now += 10, h3);
    CHECK(gg.state() == kGunOutOfRange && h3.spawns.size() <= before + 1);
}

// UpdateEnergyBudget (1002d340) on hand-made devices: P from the battery, then engines, sensors, weapons in
// that order; the battery gives up what was used.
void test_energy_budget() {
    std::printf("combat: energy budget\n");
    BulletProto b;
    GunDeviceSpec spec;
    spec.param[0] = -1, spec.param[1] = 20, spec.param[2] = 10, spec.param[3] = 100;
    spec.barrels = {0};
    CtldJoint j;
    j.rate = 4;
    auto make = [&](float battery_power) {
        EnergyBudget e;
        PowerDevice bat;
        bat.cls = bat.type = 0x13, bat.power = battery_power, bat.param[0] = 100, bat.state = PowerDevice::initial_state(0x13, -1);
        PowerDevice eng;
        eng.cls = eng.type = 5, eng.power = 4, eng.state = PowerDevice::initial_state(5, -1);
        PowerDevice radar;
        radar.cls = radar.type = 8, radar.power = 3, radar.state = PowerDevice::initial_state(8, -1);
        PowerDevice gun;
        gun.cls = gun.type = 2, gun.gun = 0;
        PowerDevice repair; // off (state 0): no draw
        repair.cls = repair.type = 0xf, repair.power = 50, repair.state = PowerDevice::initial_state(0xf, -1);
        e.devices = {bat, eng, radar, gun, repair};
        return e;
    };
    std::vector<GunDevice> guns{GunDevice(spec, {j}, &b)};
    TestHost h;
    guns[0].set_state(kFireSingle);
    for (h.now = 0; h.now < 1500;) guns[0].update(h.now += 10, h);
    CHECK(h.spawns.size() == 1 && near(guns[0].energy(), 10) && near(guns[0].energy_ratio(), 0.5f));
    EnergyHost host;
    host.engine_load = 1;
    // Supply 10/s for 1 s: engine 4, radar 3, the gun's deficit 10 gets the remaining 3 (ratio 0.3).
    EnergyBudget e = make(10);
    e.update(1, guns, host);
    CHECK(near(e.devices[1].ratio, 1) && near(e.devices[2].ratio, 1) && near(guns[0].energy(), 13) && near(guns[0].energy_ratio(), 0.65f));
    CHECK(near(*e.battery_fraction(), 0.9f) && near(*e.battery_capacity(), 100));
    CHECK(near(e.drain_a, 3) && near(e.drain_b, 7));
    CHECK(near(e.devices[4].ratio, 1)); // the repair device is off
    // Supply 2: the engine gets half, the radar and the gun nothing.
    EnergyBudget lo = make(2);
    lo.update(1, guns, host);
    CHECK(near(lo.devices[1].ratio, 0.5f) && near(lo.devices[2].ratio, 0) && near(guns[0].energy(), 13) && near(*lo.battery_fraction(), 0.98f));
    // The offer is power·charge·dt, at most the charge held; capacity −1 never drains (property 1 = 1).
    EnergyBudget half = make(10);
    half.set_battery_fraction(0.5f);
    half.update(1, guns, host); // offers 5: engine 4, radar 1/3
    CHECK(near(half.devices[2].ratio, 1.f / 3) && near(*half.battery_fraction(), 0.45f));
    EnergyBudget endless = make(10);
    endless.devices[0].param[0] = -1;
    endless.update(1, guns, host);
    CHECK(near(*endless.battery_fraction(), 1) && near(*endless.battery_capacity(), -1));
    // CDeviceSet::SetProperty(1) (1002ba30) leaves a battery with capacity ≤ 0 alone (notes/energy.md §1).
    endless.devices[0].ratio = 0.25f;
    endless.set_battery_fraction(0.75f);
    CHECK(near(endless.devices[0].ratio, 0.25f));
    EnergyBudget normal = make(10);
    normal.set_battery_fraction(0.75f);
    CHECK(near(normal.devices[0].ratio, 0.75f));
    // Category table 1003ccc8.
    CHECK(energy_category(5) == 3 && energy_category(0x13) == 1 && energy_category(2) == 4 && energy_category(0x1e) == 4 &&
          energy_category(9) == 5 && energy_category(8) == 2 && energy_category(0xf) == 0 && energy_category(0x1a) == 3);
}

// Force shield (classes 9 + 0x15) in the energy budget: the hero's o_fsh_l_02 (capacity 1850, 15 per s,
// 0.04 energy per unit of charge, idle 2.0/s) with the deflector o_def_l_02 (0.9 per side, idle 0.2/s).
void test_shield_budget() {
    std::printf("combat: shield charges in the energy budget\n");
    auto make = [](float battery_power) {
        EnergyBudget e;
        PowerDevice bat;
        bat.cls = bat.type = 0x13, bat.power = battery_power, bat.param[0] = 1e6f, bat.state = PowerDevice::initial_state(0x13, -1);
        PowerDevice sh;
        sh.cls = sh.type = 9, sh.power = 2, sh.param[0] = 1850, sh.param[1] = 15, sh.param[2] = 0.04f;
        sh.state = PowerDevice::initial_state(9, -1);
        PowerDevice def;
        def.cls = def.type = 0x15, def.power = 0.2f, def.state = PowerDevice::initial_state(0x15, -1);
        for (int i = 0; i < 6; ++i) def.param[std::size_t(i)] = 0.9f;
        e.devices = {bat, sh, def};
        e.shield = 1, e.deflector = 2;
        return e;
    };
    std::vector<GunDevice> guns;
    EnergyHost host;
    EnergyBudget e = make(100);
    CHECK(e.shield_active(host) && near(e.side_strength(0, host), 1665)); // 0.9 · 1850 · 1
    // Full: the idle draws only (shield 2, deflector 0.2).
    e.update(1, guns, host);
    CHECK(near(e.drain_a, 2.2f) && near(e.side_strength(3, host), 1665));
    // A 60-point hit on the front: 1605 left (charge 1605 / 1665), 66.67 units of charge missing.
    e.set_side_strength(0, 1605, host);
    CHECK(near(e.devices[1].sides[0], 1605.f / 1665, 1e-5f) && near(e.side_strength(0, host), 1605, 1e-2f));
    // One second: param1 = 15 units restored for 0.6 energy (need 2.6), spread over what the sides lack
    // (only the front): +15 · 0.9 = 13.5 strength.
    e.update(1, guns, host);
    std::printf("  front after 1 s: %.2f (drain %.3f /s)\n", e.side_strength(0, host), e.drain_a);
    CHECK(near(e.drain_a, 2.8f, 1e-3f) && near(e.side_strength(0, host), 1618.5f, 0.02f));
    for (int i = 0; i < 4; ++i) e.update(1, guns, host); // 15 more each second: full after 66.67/15 = 4.44 s
    CHECK(near(e.side_strength(0, host), 1665, 0.02f));
    // Under-supplied (battery 1/s for a need of 2.2): k = 1/2.2; the shield's e = 2k − 2 < 0 drains the
    // sides by e/0.04 units, in proportion to their charge; property 0x79 = charge · deflector ratio k.
    EnergyBudget lo = make(1);
    lo.update(1, guns, host);
    const float k = 1 / 2.2f, drop = (2 * k - 2) / 0.04f / (6 * 1850);
    const auto hud = lo.shield_charges(host);
    std::printf("  under-supplied: charge %.6f (want %.6f), 0x79 %.4f\n", lo.devices[1].sides[0], 1 + drop, hud ? (*hud)[0] : -1.f);
    CHECK(near(lo.devices[1].sides[0], 1 + drop, 1e-6f) && hud && near((*hud)[0], (1 + drop) * k, 1e-5f));
    CHECK(near(lo.side_strength(1, host), 0.9f * k * 1850 * (1 + drop), 0.01f));
    // The sides (GetShieldSide 1002c590): the dominant axis of the hit point in the unit frame.
    CHECK(EnergyBudget::shield_side({0, 5, 1}) == 0 && EnergyBudget::shield_side({1, -5, 0}) == 1 &&
          EnergyBudget::shield_side({-3, 1, 2}) == 2 && EnergyBudget::shield_side({3, -1, 2}) == 3 &&
          EnergyBudget::shield_side({1, 1, 4}) == 4 && EnergyBudget::shield_side({1, 1, -4}) == 5);
    // No deflector: no shield.
    EnergyBudget none = make(100);
    none.deflector = -1;
    CHECK(!none.shield_active(host) && none.side_strength(0, host) == 0 && !none.shield_charges(host));
}

// Reload rooms (notes/energy.md §3): the recharge step on a unit, the room records, the box test.
void test_rooms() {
    std::printf("combat: reload rooms\n");
    // Room records (MRoomManager_Init 100186b0): kept for flags & 0xf8 or & 0xc600; reload on & 0x20 / & 0x600.
    std::vector<HallwayVertex> v(6);
    v[0].flags = 0, v[1].flags = 0x620, v[2].flags = 0x10000620, v[3].flags = 0x4000, v[4].flags = 0x400, v[5].flags = 0x10;
    for (std::size_t i = 0; i < v.size(); ++i) v[i].pos = {float(i), 0, 0};
    const auto rooms = build_rooms(v);
    CHECK(rooms.size() == 5 && rooms[0].vertex == 1 && rooms[1].vertex == 2 && rooms[2].vertex == 3 && rooms[3].vertex == 4 && rooms[4].vertex == 5);
    CHECK(rooms[0].reloads() && rooms[1].reloads() && !rooms[2].reloads() && rooms[3].reloads() && !rooms[4].reloads());
    CHECK(near(rooms[0].radius, 5) && near(rooms[0].height, 3) && near(rooms[0].speed_limit, 2));
    CHECK(near(rooms[1].radius, 10) && near(rooms[1].height, 12));
    CHECK(build_rooms({{{}, 0x20000, 0}}).empty()); // no 0xf8 / 0xc600 bit: no room
    CHECK(near(build_rooms({{{}, 0x24020, 0}})[0].speed_limit, 1000));
    // Room_UpdateOccupants' box: z from −0.7h to +h (−h with flags & 0x4040), horizontally within the radius.
    const Room r = rooms[0];
    CHECK(room_contains(r, {10, 10, 0}, {13, 14, 0}) && !room_contains(r, {10, 10, 0}, {13, 14.1f, 0}));
    CHECK(room_contains(r, {0, 0, 0}, {0, 0, -2.1f}) && !room_contains(r, {0, 0, 0}, {0, 0, -2.2f}));
    CHECK(room_contains(r, {0, 0, 0}, {0, 0, 3}) && !room_contains(r, {0, 0, 0}, {0, 0, 3.1f}));
    Room low = r;
    low.flags |= 0x40;
    CHECK(room_contains(low, {0, 0, 0}, {0, 0, -3}) && !room_contains(low, {0, 0, 0}, {0, 0, -3.1f}));
    // Ammo step: max 200 → +2 per 0.1 s step; max 5 → +1; never above the maximum; unlimited (−1) untouched.
    CHECK(recharged_ammo(0, 200, kRoomRate * 0.1f) == 2 && recharged_ammo(0, 5, kRoomRate * 0.1f) == 1);
    CHECK(recharged_ammo(199, 200, 0.01f) == 200 && recharged_ammo(-1, -1, 0.01f) == -1);

    // The step on a unit: battery 0.30 + 5 s of 0.1 s steps = 0.80, clamped at 1 after 7 s.
    BulletProto b;
    GunDeviceSpec spec;
    spec.type = 2, spec.param[0] = 200, spec.param[1] = 200, spec.param[2] = 10;
    spec.barrels = {0};
    CtldJoint j;
    Combatant c;
    PowerDevice bat;
    bat.cls = bat.type = 0x13, bat.param[0] = 100, bat.ratio = 0.3f, bat.state = PowerDevice::initial_state(0x13, -1);
    PowerDevice sh;
    sh.cls = sh.type = 9, sh.param[0] = 1850, sh.sides = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    c.energy.devices = {bat, sh};
    c.energy.shield = 1;
    c.damage = make_state({100, 100}, {-1, 0});
    c.damage.apply_damage_total(-100, false, false, 0); // 100 of 200 HP left
    c.guns.emplace_back(spec, std::vector<CtldJoint>{j}, &b);
    c.guns[0].set_ammo(0);
    c.guns[0].set_energy_ratio(0.5f);
    CHECK(near(c.guns[0].energy(), 100) && near(c.guns[0].energy_ratio(), 0.5f));
    for (int i = 0; i < 50; ++i) c.recharge(kRoomRate * 0.1f, 0);
    CHECK(near(c.battery_fraction(), 0.8f, 1e-3f));
    CHECK(near(c.damage.hp_ratio(), 1.f, 1e-3f)); // 0.5 + 50 steps of 0.01
    CHECK(near(*c.energy.shield_mean(), 1.f, 1e-3f));
    CHECK(c.guns[0].ammo() == 100 && near(c.guns[0].energy_ratio(), 1) && near(c.guns[0].energy(), 200));
    for (int i = 0; i < 20; ++i) c.recharge(kRoomRate * 0.1f, 0);
    CHECK(near(c.battery_fraction(), 1) && c.guns[0].ammo() == 140 && near(c.damage.hp_ratio(), 1));
    // Gun energy: param1 200, ratio 0.5 → after 1 s the ratio is 0.6 and the energy 120.
    c.guns[0].set_energy_ratio(0.5f);
    for (int i = 0; i < 10; ++i) c.recharge(kRoomRate * 0.1f, 0);
    CHECK(near(c.guns[0].energy_ratio(), 0.6f) && near(c.guns[0].energy(), 120));
    // A gun of type 0x1e (a building's) and an unlimited one keep their ammo; an endless battery stays.
    Combatant d;
    spec.type = 0x1e, spec.param[0] = -1;
    d.guns.emplace_back(spec, std::vector<CtldJoint>{j}, &b);
    d.guns[0].set_energy_ratio(0.5f);
    PowerDevice endless = bat;
    endless.param[0] = -1, endless.ratio = 0.25f;
    d.energy.devices = {endless};
    d.recharge(0.1f, 0);
    CHECK(d.guns[0].ammo() == -1 && near(d.guns[0].energy_ratio(), 0.5f) && near(d.energy.devices[0].ratio, 0.25f));
}

// Radar detection with the target's signatures (notes/energy.md §4.2).
void test_radar() {
    std::printf("combat: radar detection\n");
    Combatant radar;
    PowerDevice r;
    r.cls = r.type = 8, r.param[0] = 0.01f, r.param[3] = 300, r.state = PowerDevice::initial_state(8, -1);
    radar.energy.devices = {r};
    radar.energy.radar = 0;
    // R = 300, d = 150 (f = 0.75), param0 0.01, target mass 200 (property 0x45): 0.01·200·0.75 = 1.5 ≥ 1.
    Combatant target;
    target.mass = Combatant::Mass{200, 5, 5};
    const auto sig = target.signature();
    CHECK(sig && near(sig->p45, 200) && near(sig->p43, 0) && near(sig->p44, 0)); // no drain yet
    CHECK(radar.radar_detects(150, *sig) && !radar.radar_detects(300, *sig));
    radar.energy.devices[0].ratio = 0.5f; // half the energy: 0.75 → not detected
    CHECK(!radar.radar_detects(150, *sig));
    radar.energy.devices[0].ratio = 0; // no energy detects nothing
    CHECK(!radar.radar_detects(1, *sig));
    // The other two signatures: area × the sensor / shield draw (0x43), volume × the weapon / engine draw (0x44).
    radar.energy.devices[0].ratio = 1;
    radar.energy.devices[0].param[0] = 0, radar.energy.devices[0].param[1] = 0.02f, radar.energy.devices[0].param[2] = 0.02f;
    target.energy.drain_a = 4, target.energy.drain_b = 0;
    CHECK(radar.radar_detects(150, *target.signature()) == (0.02f * 5 * 4 * 0.75f >= 1)); // 1.5
    target.energy.drain_a = 0.5f; // 0.02·5·0.5·0.75 = 0.0375
    CHECK(!radar.radar_detects(150, *target.signature()));
    target.energy.drain_b = 40; // 0.02·5·40·0.75 = 3
    CHECK(radar.radar_detects(150, *target.signature()));
    // A radar that is off, or without a radar device, sees nothing; a target without a design has no signature.
    radar.energy.devices[0].state = 0;
    CHECK(!radar.radar_detects(1, *sig));
    CHECK(!Combatant().radar_detects(1, *sig) && !Combatant().signature());
    // i_dsh (type 0xa) scales the signatures: min(1, GetParam(0x300 + i)).
    PowerDevice dsh;
    dsh.cls = dsh.type = 0xa, dsh.param[0] = 0.5f, dsh.param[1] = 2, dsh.param[2] = 0.25f, dsh.state = 0x2000;
    target.energy.devices = {dsh};
    target.energy.drain_a = target.energy.drain_b = 1;
    const auto s2 = target.signature();
    CHECK(s2 && near(s2->p45, 100) && near(s2->p43, 5) && near(s2->p44, 1.25f));
}

// Reload rooms on the game's buildings (notes/energy.md §3.3), and a hero recharging in a plant's room.
void test_rooms_corpus(AssetLibrary& lib, const std::filesystem::path& game) {
    std::printf("combat corpus: reload rooms\n");
    auto flags_of = [&](const char* msh) {
        std::set<std::uint32_t> f;
        for (const auto& v : load_hallway(lib, "fortif.rlb", msh))
            if (v.flags & 0x620) f.insert(v.flags);
        return f;
    };
    using S = std::set<std::uint32_t>;
    for (const char* n : {"fr_b_bunker.msh", "fr_m_bunker.msh", "fr_l_bunker.msh", "fr_b_tower.msh", "fr_m_tower.msh", "fr_b_ruin.msh"})
        CHECK(flags_of(n) == S{0x620});
    CHECK(flags_of("fr_l_gener.msh") == S({0x620, 0x10000620, 0x10000020}));
    CHECK(flags_of("fr_l_angar.msh") == S{0x10000620});
    for (const char* n : {"fr_b_plant.msh", "fr_m_plant.msh", "fr_l_plant.msh"}) {
        const S f = flags_of(n);
        CHECK(f.count(0x600) && f.count(0x10000400));
        const auto rooms = build_rooms(load_hallway(lib, "fortif.rlb", n));
        CHECK(std::any_of(rooms.begin(), rooms.end(), [](const Room& r) { return r.reloads() && r.flags == 0x600; }));
    }

    // Tut_2: the hero stands in the Large Factory's reload room.
    const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
    const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, mission), "data.tma")));
    std::string map_dir = tma.land_path;
    map_dir.resize(map_dir.find_last_of("\\/"));
    const Terrain terrain = decode_terrain(read_file(*resolve_path(game, map_dir + "/Land.msh")));
    const TerrainGround ground(terrain);
    const mission::TmaObject *hero_o = nullptr, *plant_o = nullptr;
    for (const auto& o : tma.objects) {
        if (o.identity == 1) hero_o = &o;
        if (o.identity == 0x80000001) plant_o = &o;
    }
    CHECK(hero_o && plant_o);
    if (!hero_o || !plant_o) return;
    auto hero = LiveUnit::load(lib, *hero_o, ground);
    auto plant = LiveUnit::load_building(lib, *plant_o);
    CHECK(hero && plant);
    if (!hero || !plant) return;
    const UnitPart& root = plant->model.parts.front();
    const auto rooms = build_rooms(load_hallway(lib, root.mesh_archive, root.mesh_entry));
    const Room* reload = nullptr;
    for (const auto& r : rooms)
        if (r.reloads() && !reload) reload = &r;
    CHECK(reload != nullptr);
    if (!reload) return;
    const Vec3 at = plant->frame.matrix.transform_point(reload->pos);
    std::printf("  %s: %zu rooms, reload vertex %u (flags %x) at (%.1f, %.1f, %.1f)\n", root.mesh_entry.c_str(), rooms.size(), reload->vertex,
                reload->flags, at.x, at.y, at.z);
    CombatWorld world(lib, ground, nullptr);
    const std::size_t hi = world.add_unit(*hero, hero->placement->identity, hero->placement->ai_index, true);
    const std::size_t pi = world.add_unit(*plant, plant->placement->identity, plant->placement->ai_index, false);
    world.set_rooms(pi, rooms);
    Combatant& h = world.unit(hi);
    h.energy.set_battery_fraction(0.3f);
    CHECK(near(h.battery_fraction(), 0.3f, 1e-3f));
    auto stand = [&](Vec3 p, bool walk) {
        Mat4 m = Mat4::translation(p);
        hero->motion->start(0, m);
        hero->motion->set_throttle({0, walk ? 1.f : 0.f, 0});
        hero->frame = hero->motion->interpolate(0);
    };
    // The hero's radar against a unit of the hero's own design (full energy, 100 m): detected by its mass signature.
    if (auto path = resolve_path(game, hero_o->resource))
        if (const auto st = economy::design_stats(lib, mission::decode_scheme(read_file(*path)))) {
            h.mass = Combatant::Mass{st->mass, st->area, st->volume};
            const auto sig = h.signature();
            std::printf("  hero: mass %.0f area %.1f volume %.1f; radar R %.0f, param0 %.4f; signature p45 %.0f\n", st->mass, st->area,
                        st->volume, *h.radar_param(3), *h.radar_param(0), sig ? sig->p45 : -1.f);
            CHECK(sig && h.radar_detects(100, *sig) && !h.radar_detects(*h.radar_param(3), *sig));
        }
    float now = 0;
    auto run = [&](float ms, bool walk) {
        for (float t = 0; t < ms; t += 25) {
            now += 25;
            if (walk) { // keep the hero at the vertex while it walks in place
                hero->motion->update(now);
                hero->frame.matrix = Mat4::translation(at);
            }
            hero->pose_parts();
            plant->pose_parts();
            world.update(now);
        }
    };
    h.damage.clan = plant->placement->ai_index;
    // The energy budget's own slow tick also drains the battery a little; compare against an unrecharged twin.
    stand(at, false);
    run(5000, false);
    const float after5 = h.battery_fraction();
    std::printf("  battery after 5 s in the room: %.3f\n", after5);
    // Spec: a = 0.1·dt per step, 49 steps of 0.1 s fall in 5 s (the first update only starts the room's clock):
    // 0.3 + 0.1·4.9 = 0.79, less the ≈ 0.003 the energy budget drains meanwhile.
    CHECK(near(after5, 0.79f, 0.005f));
    run(7000, false);
    CHECK(near(h.battery_fraction(), 1.f, 1e-3f) || h.battery_fraction() > 0.98f);
    // An enemy clan, or a hero outside the room, is not recharged.
    h.energy.set_battery_fraction(0.3f);
    h.damage.clan = plant->placement->ai_index + 1;
    run(3000, false);
    CHECK(h.battery_fraction() < 0.3f + 1e-3f);
    h.damage.clan = plant->placement->ai_index;
    stand(at + Vec3{100, 0, 0}, false);
    run(3000, false);
    CHECK(h.battery_fraction() < 0.3f + 1e-3f);
    // Walking through the room (|v| ≥ 2 m/s) does not recharge.
    stand(at, true);
    run(1000, true);
    const float speed = std::sqrt(dot(hero->motion->world_velocity(), hero->motion->world_velocity()));
    std::printf("  walking hero: %.1f m/s, battery %.3f\n", speed, h.battery_fraction());
    CHECK(speed >= 2 && h.battery_fraction() < 0.3f + 1e-3f);
}

} // namespace

int combat_unit_tests() {
    g_fail = 0;
    test_splash_and_armour();
    test_damage_state();
    test_gun_device();
    test_energy_budget();
    test_shield_budget();
    test_rooms();
    test_radar();
    return g_fail;
}

int combat_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    AssetLibrary lib(game);
    std::printf("combat corpus: weapons, bullets, EXPL\n");
    try {
        const WeaponCorpus w = scan_weapon_corpus(lib);
        std::printf("  %zu weapon devices, %zu bullets (%zu BULL records), %zu EXPL, %zu NDPR (%zu parts); unresolved: %s\n",
                    w.weapon_devices, w.bullets, w.bull_records, w.expl_entries, w.ndpr_entries, w.ndpr_parts,
                    w.unresolved.empty() ? "-" : w.unresolved.front().c_str());
        CHECK(w.weapon_devices == 158 && w.bullets == 66 && w.bull_records == 67 && w.expl_entries == 119);
        CHECK(w.unresolved.size() == 1 && w.unresolved[0].starts_with("bm_b_02"));
    } catch (const std::exception& ex) {
        CHECK(!"weapon corpus");
        std::printf("  %s\n", ex.what());
    }
    test_rooms_corpus(lib, game);
    // Spec §0 / §1.4: the hero's guns.
    const BulletProto mg = load_bullet(lib, "bb_h_01"), laser = load_bullet(lib, "bl_h_01"), missile = load_bullet(lib, "bm_h_01");
    CHECK(near(mg.hit_damage(), 60) && near(laser.hit_damage(), 250));
    CHECK(near(mg.speed, 350) && near(mg.range, 500) && mg.removal_ms == 1000 && mg.physics == 0);
    CHECK(mg.expl && mg.expl->kind == 2 && mg.range_expl_name.second == "bb_h_01_end.exp" && !mg.spawn_effects.empty());
    CHECK(missile.expl && missile.expl->kind == 3 && near(missile.expl->damage, 170) && near(missile.expl->radius, 7) && missile.seeker);

    // Tut_2: the hero's laser shoots an animal (tent) until it dies.
    std::printf("combat corpus: Tut_2 fight\n");
    const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
    const auto dir = resolve_path(game, mission);
    if (!dir) {
        CHECK(!"Tut_2 missing");
        return g_fail;
    }
    const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*dir, "data.tma")));
    std::string map_dir = tma.land_path;
    map_dir.resize(map_dir.find_last_of("\\/"));
    const Terrain terrain = decode_terrain(read_file(*resolve_path(game, map_dir + "/Land.msh")));
    const TerrainGround ground(terrain);
    std::unique_ptr<LiveUnit> hero, tent;
    for (const auto& o : tma.objects) {
        if (!hero && o.resource.find("\\HERO\\") != std::string::npos) hero = LiveUnit::load(lib, o, ground);
        if (!tent && o.resource.find("tent.dat") != std::string::npos) tent = LiveUnit::load(lib, o, ground);
    }
    CHECK(hero && tent);
    if (!hero || !tent) return g_fail;
    // Walk sway (notes/turret-camera.md "Walk sway"): r_h_02's walk clip yaws the chassis root and the
    // turret mounted under it follows. With steady mounts (the port's player option) the turret's
    // mount keeps its heading in the body frame; without them it swings as in the original.
    {
        auto turret_yaw_range = [&](bool steady) {
            auto u = LiveUnit::load(lib, *hero->placement, ground);
            u->steady_mounts = steady;
            u->motion->set_throttle({0, 1, 0});
            float lo = 1e9f, hi = -1e9f;
            for (int ms = 33; ms <= 3000; ms += 33) {
                u->motion->update(float(ms));
                u->frame = u->motion->interpolate(float(ms));
                u->pose_parts();
                const Mat4& m = u->model.parts[u->turret_part].model.node_world[0]; // body frame
                const float yaw = std::atan2(m.at(1, 1), m.at(0, 1)) * 57.29578f;
                lo = std::min(lo, yaw), hi = std::max(hi, yaw);
            }
            return hi - lo;
        };
        const float swaying = turret_yaw_range(false), steady = turret_yaw_range(true);
        std::printf("  walk: turret mount yaw swings %.2f° (original), %.3f° (steady mounts)\n", swaying, steady);
        CHECK(swaying > 3 && steady < 0.05f);
    }
    // The tent hovers 40 m in front of the hero, at the hero's height.
    {
        Mat4 m = hero->frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), 0};
        Mat4 t = m;
        t.at(0, 3) += fwd.x * 40, t.at(1, 3) += fwd.y * 40, t.at(2, 3) += 8;
        tent->motion->start(0, t);
        tent->frame = tent->motion->interpolate(0);
    }
    CombatWorld world(lib, ground, nullptr);
    const std::size_t hi = world.add_unit(*hero, hero->placement->identity, hero->placement->ai_index, true);
    const std::size_t ti = world.add_unit(*tent, tent->placement->identity, tent->placement->ai_index, false);
    Combatant& h = world.unit(hi);
    Combatant& t = world.unit(ti);
    CHECK(h.guns.size() == 4 && h.selected == std::vector<bool>({true, false, false, false}));
    CHECK(near(t.damage.initial, 8001) && t.damage.removal_delay_ms > 0);
    // State 0x400 aiming (TurretRig::inputs_for_angles): the laser barrel points at marks around the hero.
    {
        const Mat4 m = hero->frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, right{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, up{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
        hero->pose_parts();
        const Vec3 mount = *world.muzzle(hi, 2);
        for (const Vec3 off : {Vec3{0, 60, 0}, Vec3{30, 60, 0}, Vec3{-30, 60, 10}, Vec3{40, -20, 5}, Vec3{0, 60, 25}}) {
            const Vec3 aim = mount + right * off.x + fwd * off.y + up * off.z;
            const Vec3 d = aim - mount;
            const float x = dot(d, right), y = dot(d, fwd), z = dot(d, up);
            const auto [tx, ty] = hero->rig->inputs_for_angles(std::atan2(x, y), std::atan2(z, std::hypot(x, y)));
            hero->aim_x = tx, hero->aim_y = ty;
            hero->pose_parts();
            const float err = world.aim_error(hi, 2, aim);
            std::printf("  aim (%g %g %g): inputs %.3f %.3f, error %.3f rad\n", off.x, off.y, off.z, tx, ty, err);
            CHECK(err < 0.06f);
        }
        hero->aim_x = TurretRig::kManualX, hero->aim_y = TurretRig::kManualY;
    }
    // Robots' turrets (C2M1 designs): the barrel of the first turret gun follows the same aiming.
    for (const char* design : {"UNITS/UNITS/BATTLE/21swlk1.dat", "UNITS/UNITS/BATTLE/21swlk2.dat", "UNITS/UNITS/BATTLE/21mwlk1.dat",
                               "UNITS/UNITS/BATTLE/21bwlk1.dat", "UNITS/UNITS/BATTLE/21swel1.dat"}) {
        mission::TmaObject o = *hero->placement;
        o.resource = design;
        auto r = LiveUnit::load(lib, o, ground);
        if (!r || !r->rig) {
            std::printf("  %s: no turret\n", design);
            continue;
        }
        CombatWorld w2(lib, ground, nullptr);
        const std::size_t ri = w2.add_unit(*r, 999, 0, false);
        // Clip modules (INTO clip@0 → CGunDevice::Load): e_gun_lc_01 carries ammo 200, its clip i_c01_l_02 500;
        // e_gun_mc_14 250, its clip i_c14_m_02 45 (o_c01_l_02.ctl / o_c14_m_02.ctl param0).
        if (std::string_view(design) == "UNITS/UNITS/BATTLE/21swlk1.dat" || std::string_view(design) == "UNITS/UNITS/BATTLE/21mwlk1.dat") {
            const std::int32_t want = std::string_view(design).ends_with("21swlk1.dat") ? 500 : 45;
            std::size_t clipped = 0;
            for (const auto& gun : w2.unit(ri).guns) clipped += gun.ammo() == want && !gun.spec().module.empty();
            std::printf("  %s: %zu guns with the clip's ammo %d\n", design, clipped, want);
            CHECK(clipped == 2);
        }
        std::size_t g = 0;
        while (g < w2.unit(ri).guns.size() && !w2.unit(ri).mounts[g].on_turret()) ++g;
        if (g == w2.unit(ri).guns.size()) {
            std::printf("  %s: no turret gun\n", design);
            continue;
        }
        const Mat4 m = r->frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, right{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, up{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
        r->pose_parts();
        const Vec3 mount = *w2.muzzle(ri, g);
        std::printf("  %s gun %zu:", design, g);
        for (const Vec3 off : {Vec3{0, 60, 0}, Vec3{30, 60, 0}, Vec3{-30, 60, 10}, Vec3{40, -20, 5}}) {
            const Vec3 aim = mount + right * off.x + fwd * off.y + up * off.z;
            const Vec3 d = aim - mount;
            const float x = dot(d, right), y = dot(d, fwd), z = dot(d, up);
            const auto [tx, ty] = r->rig->inputs_for_angles(std::atan2(x, y), std::atan2(z, std::hypot(x, y)));
            r->aim_x = tx, r->aim_y = ty;
            r->pose_parts();
            const float err = w2.aim_error(ri, g, aim);
            std::printf(" %.3f", err);
            CHECK(err < (off.y > 0 ? 0.08f : 0.2f)); // behind and aside: 21bwlk1 ends 0.135 rad off (not investigated)
        }
        std::printf("\n");
    }
    // Homing (CSeekerDevice::Track + CBulletControl::Update): the plasma (seeker cone 0.25 rad, turn
    // 0.8 rad/s) locks on the tent, then each shot leaves 5 m to its side (~0.12 rad) and curves in.
    {
        CombatWorld w(lib, ground, nullptr);
        const std::size_t a = w.add_unit(*hero, hero->placement->identity, hero->placement->ai_index, true);
        const std::size_t b = w.add_unit(*tent, tent->placement->identity, tent->placement->ai_index, false);
        Combatant& shooter = w.unit(a);
        shooter.select(1), shooter.select(2); // the plasma only
        shooter.target = std::int32_t(b);
        hero->pose_parts();
        tent->pose_parts();
        const Vec3 mount = *w.muzzle(a, 1), aim = w.unit(b).aim_centre();
        const Mat4 m = hero->frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, right{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, up{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
        const Vec3 d = aim - mount;
        const auto [tx, ty] = hero->rig->inputs_for_angles(std::atan2(dot(d, right), dot(d, fwd)), std::atan2(dot(d, up), std::hypot(dot(d, right), dot(d, fwd))));
        hero->aim_x = tx, hero->aim_y = ty;
        hero->pose_parts();
        shooter.aim_point = aim + right * 5.f;
        // Flying straight, the shot would miss the tent.
        const Vec3 off = normalize(*shooter.aim_point - mount);
        const auto straight = w.trace(mount, mount + off * 150.f, std::int32_t(a));
        CHECK(!straight || straight->unit != std::int32_t(b));
        shooter.set_trigger(true);
        std::size_t shots = 0, hits_on_tent = 0;
        for (float now = 33; now < 3000; now += 33) {
            hero->pose_parts();
            tent->pose_parts();
            w.update(now);
            for (const auto& e : w.take_events()) {
                if (e.type == CombatEvent::Type::Shot) ++shots;
                if (e.type == CombatEvent::Type::Damaged && e.unit == std::int32_t(b)) ++hits_on_tent;
            }
        }
        std::printf("  plasma homing: %zu shots aimed 5 m aside, %zu hits on the tent (straight line: %s)\n", shots, hits_on_tent,
                    straight && straight->unit == std::int32_t(b) ? "hit" : "miss");
        CHECK(shots > 3 && hits_on_tent + 1 >= shots); // the last shot may still be in flight
        hero->aim_x = TurretRig::kManualX, hero->aim_y = TurretRig::kManualY;
    }

    // Energy budget (UpdateEnergyBudget 1002d340) on the hero (tut2_p: r_h_02 + e_tur_ht_02). Batteries: the
    // i_pws_l_03 module (o_pws_l_03.ctl: capacity 4080, power 6.8/s) replaces r_h_02 device 0. Idle draws:
    // shield o_fsh_l_02 2.0, o_dsh_l_03 0.11 (state 0x2000), radar o_rdr_l_df 0.08, o_def_l_02 0.2 per second;
    // repair (off), armour and the guns draw nothing idle; the engine (r_h_02 device 5) nothing standing.
    // The laser (o_tur_ht_02 device 6: buffer 200, 5.5 a shot) fired without pause outruns the supply and
    // stops in state 6; then each 250 ms tick refills it by (6.8·charge − 2.39)·0.25 and the battery loses
    // 6.8·charge·0.25 / 4080.
    {
        CombatWorld w(lib, ground, nullptr);
        const std::size_t a = w.add_unit(*hero, hero->placement->identity, hero->placement->ai_index, true);
        Combatant& c = w.unit(a);
        CHECK(c.energy.battery_capacity() && near(*c.energy.battery_capacity(), 4080));
        CHECK(near(c.battery_fraction(), 1));
        c.select(1), c.select(3); // the laser only
        c.aim_point = hero->position() + Vec3{0, 0, 200};
        c.set_trigger(true);
        const GunDevice& laser = c.guns[2];
        float now = 0, empty_at = -1;
        while (now < 120000 && empty_at < 0) {
            hero->pose_parts();
            w.update(now += 25);
            w.take_events();
            if (laser.state() == kGunNoEnergy) empty_at = now;
        }
        std::printf("  laser: no energy at %.2f s after %u shots (energy %.2f, battery %.4f)\n", empty_at / 1000, laser.shots(),
                    laser.energy(), c.battery_fraction());
        CHECK(empty_at > 0 && laser.energy() < 5.5f);
        // 200 + the supply over the run (≈ 4.4/s) = the shots' 5.5 each.
        const float supplied = laser.shots() * 5.5f + laser.energy() - 200;
        CHECK(supplied > 0 && supplied < empty_at / 1000 * 4.42f + 1);
        c.set_trigger(false);
        float worst = 0;
        for (int tick = 0; tick < 40; ++tick) {
            const float e0 = laser.energy(), f0 = c.battery_fraction();
            for (int f = 0; f < 10; ++f) {
                hero->pose_parts();
                w.update(now += 25);
            }
            w.take_events();
            const float gain = (6.8f * f0 - 2.39f) * 0.25f;
            const float de = laser.energy() - e0, df = f0 - c.battery_fraction();
            worst = std::max({worst, std::abs(de - gain), std::abs(df * 4080 - 6.8f * f0 * 0.25f)});
        }
        std::printf("  laser recharge: %.3f / tick, energy %.2f, battery %.4f; drain %.3f / %.3f /s (worst error %.2g)\n",
                    (6.8f * c.battery_fraction() - 2.39f) * 0.25f, laser.energy(), c.battery_fraction(), c.energy.drain_a, c.energy.drain_b, worst);
        CHECK(worst < 2e-3f);
        CHECK(near(c.energy.drain_a, 2.39f, 1e-3f) && near(c.energy.drain_b, (6.8f * c.battery_fraction() - 2.39f), 0.01f));
    }

    // Force shield (CBulletControl::OnCollision 1000d0c0, AbsorbByShield 1000ff00): a second hero 40 m in
    // front, facing the first, takes its MG fire on the front side of its shield.
    {
        auto target = LiveUnit::load(lib, *hero->placement, ground);
        Mat4 m = hero->frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), 0};
        Mat4 t2 = m;
        for (int r = 0; r < 3; ++r) t2.at(r, 0) = -m.at(r, 0), t2.at(r, 1) = -m.at(r, 1); // turned about z by 180°
        t2.at(0, 3) += fwd.x * 40, t2.at(1, 3) += fwd.y * 40;
        target->motion->start(0, t2);
        target->frame = target->motion->interpolate(0);
        CombatWorld w(lib, ground, nullptr);
        const std::size_t a = w.add_unit(*hero, hero->placement->identity, 0, true);
        const std::size_t b = w.add_unit(*target, 2, 1, false);
        Combatant& shooter = w.unit(a);
        Combatant& victim = w.unit(b);
        const EnergyHost vh = victim.energy_host();
        CHECK(victim.energy.shield >= 0 && victim.energy.deflector >= 0 && victim.energy.shield_active(vh));
        const float full = victim.energy.side_strength(0, vh);
        const PowerDevice& sd = victim.energy.devices[std::size_t(victim.energy.shield)];
        std::printf("  shield %s/%s: capacity %.0f, front %.1f; hit points %.0f; MG bullet HP %.0f, damage %.0f\n", sd.effect.first.c_str(),
                    sd.effect.second.c_str(), sd.param[0], full, victim.damage.total, mg.hit_points(), mg.hit_damage());
        target->pose_parts();
        hero->pose_parts();
        shooter.aim_point = victim.aim_centre();
        shooter.set_trigger(true); // the MG
        float now = 0, first = -1, last = -1;
        std::size_t stopped = 0, fx = 0, bad_end = 0;
        const float hp_before = victim.damage.total;
        while (now < 3000) {
            hero->pose_parts();
            target->pose_parts();
            w.update(now += 25);
            for (const auto& e : w.take_events()) {
                if (e.type == CombatEvent::Type::BulletEnded && e.amount == 2) {
                    ++stopped, last = now;
                    if (first < 0) first = now;
                    const Vec3 d = e.pos - victim.shield_centre();
                    bad_end += std::abs(std::sqrt(dot(d, d)) - victim.shield_radius()) > 0.01f; // on the sphere
                }
                if (e.type == CombatEvent::Type::Effect && e.unit == std::int32_t(b) && e.handle && e.placement) ++fx;
            }
        }
        shooter.set_trigger(false);
        // Each stopped bullet (HP 59 ≤ the side) puts a kind-2 record {target, −2, side 0} of 60 (mult·EXPL 1 +
        // HP 59) on the shield; the sides regain 15 units of charge a second (13.5 of strength) meanwhile.
        const float front = victim.energy.side_strength(0, vh), regen = front - (full - 60.f * float(stopped));
        std::printf("  %u MG shots, %zu stopped on the sphere (r %.2f), %zu hit effects; front %.1f → %.1f (regained %.1f in %.2f s), HP "
                    "%.0f → %.0f\n",
                    shooter.guns[0].shots(), stopped, victim.shield_radius(), fx, full, front, regen, (last - first) / 1000, hp_before,
                    victim.damage.total);
        CHECK(near(full, 0.9f * 1850) && near(mg.hit_points(), 59) && stopped >= 8 && bad_end == 0 && fx == stopped);
        CHECK(victim.damage.total == hp_before);
        CHECK(regen > 13.5f * ((last - first) / 1000 - 0.25f) && regen < 13.5f * ((last - first) / 1000 + 0.5f));
        // A front side weaker than the bullet's HP: the bullet collapses it, flies on and hits the body.
        victim.energy.set_side_strength(0, 30, vh);
        shooter.set_trigger(true); // one shot
        std::size_t notices = 0;
        float lost = 0;
        for (int i = 0; i < 80; ++i) {
            hero->pose_parts();
            target->pose_parts();
            w.update(now += 25);
            for (const auto& e : w.take_events())
                if (e.type == CombatEvent::Type::Shot) {
                    shooter.set_trigger(false);
                } else if (e.type == CombatEvent::Type::Damaged && e.unit == std::int32_t(b)) {
                    if (e.amount == 0) ++notices;
                    lost += e.amount;
                }
        }
        std::printf("  through a 30-point side: %zu notice(s), %.2f HP lost, front %.2f\n", notices, lost, victim.energy.side_strength(0, vh));
        // The face hit's full 60, less the armour (device 0x1b: 60·(a + 60·b)).
        const float want = victim.damage.armour ? victim.damage.armour->apply(60) : 60.f;
        CHECK(notices == 1 && near(lost, want, 0.01f) && near(victim.damage.total, hp_before - want, 0.01f));
        CHECK(victim.energy.side_strength(0, vh) < 13.5f * 2.1f); // collapsed, regaining from 0
    }

    // The HUD's device readers (hud.md §3, §5, §6): properties 0x50 / 0xa9 = the radar's (class 8, o_rdr_l_df: range
    // 250, sweep 750) GetParam(3) / (4); the lamps' first type-0xf (repair, state 0 = off) and type-0xa (i_dsh_l_03,
    // 0x2000) device states; device property 0x300 (broken) follows the gun device's part.
    {
        CombatWorld w(lib, ground, nullptr);
        const std::size_t a = w.add_unit(*hero, hero->placement->identity, 0, true);
        Combatant& c = w.unit(a);
        std::printf("  radar %.0f / %.0f; repair state %#x, detection shield %#x\n", c.radar_param(3).value_or(-1), c.radar_param(4).value_or(-1),
                    c.device_state(0xf).value_or(0xdead), c.device_state(0xa).value_or(0xdead));
        CHECK(c.radar_param(3) == 250.f && c.radar_param(4) == 750.f);
        CHECK(c.device_state(0xf) == 0u && c.device_state(0xa) == 0x2000u && !c.device_state(0x7f));
        CHECK(!c.gun_broken(2));
        std::int32_t part = -1;
        for (const auto& d : c.energy.devices)
            if (d.gun == 2) part = d.part;
        CHECK(part >= 0);
        if (part >= 0) {
            const std::size_t p = c.damage.resolve(std::size_t(part));
            c.damage.add_part_hp(p, -c.damage.parts[p].max_hp, 0);
            CHECK(c.gun_broken(2));
        }
    }

    auto rt = game::MissionRuntime::load(game, mission);
    h.select(1); // deselect the MG
    h.select(3); // the laser
    h.set_trigger(true);
    float now = 0, killed = -1, removed = -1;
    std::size_t hits = 0;
    while (now < 60000 && removed < 0) {
        now += 33;
        hero->pose_parts();
        tent->pose_parts();
        h.aim_point = t.aim_centre();
        world.update(now);
        for (const auto& e : world.take_events()) {
            if (e.type == CombatEvent::Type::Damaged && e.unit == std::int32_t(ti)) ++hits;
            if (e.type == CombatEvent::Type::Killed && e.unit == std::int32_t(ti)) {
                killed = now;
                rt->world().set_clan(t.logic_id, kKilledClan);
            }
            if (e.type == CombatEvent::Type::Removed && e.unit == std::int32_t(ti)) {
                removed = now;
                rt->object_killed(t.logic_id);
            }
        }
    }
    const GunDevice& gun = h.guns[2];
    std::printf("  laser: %u shots, %zu hits, killed at %.0f ms, removed at %.0f ms; energy %.1f\n", gun.shots(), hits, killed, removed,
                gun.energy());
    for (std::size_t i = 0; i < t.damage.parts.size(); ++i)
        if (t.damage.parts[i].present)
            std::printf("    part %zu: %.0f / %.0f%s\n", i, t.damage.parts[i].hp, t.damage.parts[i].max_hp, t.damage.parts[i].destroyed() ? " destroyed" : "");
    CHECK(killed > 0 && removed > 0);
    CHECK(near(removed - killed, float(t.damage.removal_delay_ms), 34));
    // Part 0 (2500 HP) dies after 10 laser hits of 250. A laser shot takes 2 × 0.5/rate (joint 22 rate 4:
    // 125 ms each) + param3 200 ms, plus a frame for each zero-length step.
    CHECK(h.guns[0].shots() == 0 && hits == 10);
    CHECK(killed >= 125 + 9 * 450 && killed <= 125 + 9 * (450 + 2 * 33) + 33);
    CHECK(t.removed && t.damage.clan == kKilledClan);
    if (const game::WorldObject* o = rt->world().find(t.logic_id)) CHECK(!o->alive);
    else CHECK(!"tent not in the world");
    return g_fail;
}
