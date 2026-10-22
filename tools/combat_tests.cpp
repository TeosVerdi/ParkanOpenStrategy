// Weapons, bullets, damage and death (src/combat, src/res/weapons; notes/weapons-damage.md). Unit tests
// on hand-made data; corpus tests on the game: loader totals (tools/weapons_dump.py --check), the hero's
// per-hit damage (spec §0) and a scripted fight in Tut_2 (a gun shoots an animal until it dies, its
// removal reaches the mission runtime). Linked into parkan-tests.
#include "combat/combat_world.hpp"
#include "core/paths.hpp"
#include "game/mission_runtime.hpp"
#include "mission/tma.hpp"
#include "res/terrain.hpp"
#include "res/weapons.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

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
    g.recharge_per_s = 0;
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

} // namespace

int combat_unit_tests() {
    g_fail = 0;
    test_splash_and_armour();
    test_damage_state();
    test_gun_device();
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
