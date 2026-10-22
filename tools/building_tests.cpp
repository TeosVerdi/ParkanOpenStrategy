// Buildings in combat (notes/buildings.md): src/world/building (CBuilding doors and capture door),
// buildings as combatants (src/combat) and the AI's capture task (src/ai, M_Task_Search). Unit tests on
// a fake world; corpus tests on the game: every placed building loads with its HP, a deterministic
// capture of Tut_2's Large Factory that completes the script's objective 0, and the hangar shot down to
// its removal, which the Tut_2 script reports as a failed mission. Linked into parkan-tests.
#include "ai/behaviour.hpp"
#include "ai/wizard.hpp"
#include "combat/combat_world.hpp"
#include "core/paths.hpp"
#include "game/mission_runtime.hpp"
#include "mission/tma.hpp"
#include "res/landmap.hpp"
#include "res/terrain.hpp"
#include "world/building.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

using namespace parkan;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

// ---- the AI capture task (M_Task_Search, capture mode) on a fake world ----

struct FakeWorld final : ai::Perception {
    std::vector<ai::SeenObject> objs;
    ai::SeenObject* find(std::uint32_t lid) {
        for (auto& o : objs)
            if (o.logic_id == lid) return &o;
        return nullptr;
    }
    std::optional<ai::SeenObject> object(std::uint32_t lid) const override {
        for (const auto& o : objs)
            if (o.logic_id == lid) return o;
        return std::nullopt;
    }
    void objects_near(Vec3 c, float r, std::vector<std::uint32_t>& out) const override {
        for (const auto& o : objs)
            if (ai::dist2d(o.pos, c) < r) out.push_back(o.logic_id);
    }
    bool hostile(std::uint32_t a, std::uint32_t b) const override { return a != b; }
    bool line_clear(Vec3, Vec3, std::uint32_t, std::uint32_t) const override { return true; }
};

// A walkable 1000 m square of 10 × 10 areals (as in tools/ai_tests.cpp).
LandMap big_square() {
    constexpr int n = 10;
    constexpr float size = 100;
    LandMap m;
    for (int ix = 0; ix < n; ++ix)
        for (int iy = 0; iy < n; ++iy) {
            LandAreal a;
            const float x0 = size * float(ix), y0 = size * float(iy);
            a.verts = {{x0, y0, 0}, {x0 + size, y0, 0}, {x0 + size, y0 + size, 0}, {x0, y0 + size, 0}};
            const auto id = [&](int x, int y) { return (x < 0 || y < 0 || x >= n || y >= n) ? -1 : x * n + y; };
            const auto link = [&](int nb, int edge) { return std::array<std::int32_t, 2>{nb, nb < 0 ? -1 : edge}; };
            a.links = {link(id(ix, iy - 1), 2), link(id(ix + 1, iy), 3), link(id(ix, iy + 1), 0), link(id(ix - 1, iy), 1)};
            a.anchor = {x0 + size / 2, y0 + size / 2, 0};
            a.walkable = 1;
            a.min_x = x0, a.max_x = x0 + size, a.min_y = y0, a.max_y = y0 + size;
            m.areals.push_back(a);
        }
    m.cells_x = m.cells_y = n;
    for (int c = 0; c < n * n; ++c) m.cell_first.push_back(std::uint32_t(c)), m.cell_ids.push_back(std::uint16_t(c));
    m.cell_first.push_back(n * n);
    m.set_bounds(0, 0, n * size, n * size);
    return m;
}

// A brain on a kinematic body (the pilot's requests integrated directly).
struct Sim {
    LandMap map = big_square();
    ai::ArealGraph graph = ai::ArealGraph::build(map);
    ai::MsvcRand rng;
    ai::RandomSource rand = [this] { return rng(); };
    ai::Walker walker{map, graph, rand};
    ai::Behaviour brain{walker, rand};
    ai::Wizard wz;
    ai::UnitState st;
    float heading = 0;
    std::array<float, 27> body{};
    std::uint32_t now = 0;

    Sim(FakeWorld* world, std::uint32_t lid, std::uint32_t clan, std::uint32_t size_class, Vec3 pos) {
        body[6] = body[8] = 1, body[7] = 10, body[11] = 2;
        walker.max_speed = 10, walker.min_speed = 0;
        st.pos = pos;
        walker.unit_state = [this] {
            ai::UnitState s = st;
            s.dir = {-std::sin(heading), std::cos(heading), 0};
            s.time = now;
            return s;
        };
        brain.world = world;
        brain.self.logic_id = lid, brain.self.clan = clan, brain.self.type = 0x01008000, brain.self.size_class = size_class;
    }
    void step(std::uint32_t dt = 33) {
        now += dt;
        wz.set_tick_time(now);
        brain.takt(now, wz);
        Mat4 m = Mat4::translation(st.pos);
        const float c = std::cos(heading), s = std::sin(heading);
        m.at(0, 0) = c, m.at(1, 0) = s, m.at(0, 1) = -s, m.at(1, 1) = c;
        if (auto out = wz.takt(now, float(dt), m, body)) {
            heading += out->yaw * body[11] * float(dt) * 0.001f;
            st.pos = st.pos + Vec3{-std::sin(heading), std::cos(heading), 0} * (out->linear.y * float(dt) * 0.001f);
        }
    }
};

ai::SeenObject building_at(std::uint32_t lid, std::uint32_t clan, std::uint32_t type, Vec3 pos, Vec3 entry) {
    ai::SeenObject o;
    o.logic_id = lid, o.clan = clan, o.type = type, o.pos = pos, o.radius = 40, o.entry = entry;
    return o;
}

void test_capture_task() {
    std::printf("buildings: AI capture task (M_Task_Search 10030110 / 10030300)\n");
    FakeWorld world;
    world.objs.push_back(building_at(0x80000001, 3, 0x80000010, {600, 600, 0}, {600, 560, 0}));  // a neutral plant
    world.objs.push_back(building_at(0x80000002, 1, 0x80000010, {300, 800, 0}, {300, 760, 0}));  // own plant
    world.objs.push_back(building_at(0x80000003, 3, 0x80000200, {800, 200, 0}, {800, 160, 0}));  // a teleport
    ai::Order capture;
    capture.type = ai::kOrderCapture, capture.target_kind = ai::kTargetBuilding;
    {
        Sim big(&world, 7, 1, 3, {200, 200, 0}); // size class 3 (M): refused
        capture.target_id = 0x80000001;
        CHECK(!big.brain.give_order(capture, 1));
    }
    Sim s(&world, 7, 1, 2, {200, 200, 0});
    capture.target_id = 0x80000002; // own clan
    CHECK(!s.brain.give_order(capture, 1));
    capture.target_id = 0x80000003; // teleport
    CHECK(!s.brain.give_order(capture, 1));
    capture.target_id = 7; // not a building
    CHECK(!s.brain.give_order(capture, 1));
    ai::Order minerals = capture;
    minerals.target_kind = ai::kTargetMask, minerals.mask = 0x10000000; // the explorer's search: not ported
    CHECK(!s.brain.give_order(minerals, 1));
    capture.target_id = 0x80000001;
    CHECK(s.brain.give_order(capture, 1));
    CHECK(s.brain.current_order() == ai::kOrderCapture);
    // The unit walks to the capture volume; the building's capture door (here: a stand-in that needs
    // 5 s inside 5 m of the entry, the plant's lift time) makes it the unit's.
    std::uint32_t inside_since = 0, captured_at = 0, done_at = 0;
    for (int i = 0; i < 6000 && !done_at; ++i) {
        s.step();
        const bool inside = ai::dist2d(s.st.pos, {600, 560, 0}) < 5;
        if (!inside) inside_since = 0;
        else if (!inside_since) inside_since = s.now;
        if (inside_since && !captured_at && s.now - inside_since >= 5000) {
            captured_at = s.now;
            world.find(0x80000001)->clan = 1;
        }
        if (captured_at && s.brain.current_order() != ai::kOrderCapture) done_at = s.now;
    }
    std::printf("  reached the entry, captured at %u ms, task over at %u ms; unit at (%.1f, %.1f)\n", captured_at, done_at, s.st.pos.x, s.st.pos.y);
    CHECK(captured_at > 0 && done_at >= captured_at && done_at - captured_at < 2000);
    // By type mask 0x80000000: the nearest building of another clan that is not a teleport.
    world.find(0x80000001)->clan = 3;
    Sim t(&world, 8, 1, 1, {700, 700, 0});
    ai::Order search = capture;
    search.target_kind = ai::kTargetMask, search.mask = 0x80000000, search.target_id = ai::kNoObject;
    CHECK(t.brain.give_order(search, 1));
    for (int i = 0; i < 3000; ++i) t.step();
    std::printf("  search: unit at (%.1f, %.1f)\n", t.st.pos.x, t.st.pos.y);
    CHECK(ai::dist2d(t.st.pos, {600, 560, 0}) < 30);
    CHECK(t.brain.current_order() == ai::kOrderCapture); // still searching (nothing captures here)
}

// ---- corpus ----

std::vector<std::string> all_missions(const std::filesystem::path& game) {
    std::vector<std::string> missions;
    for (int c = 0; c < 6; ++c)
        for (int m = 1; m < 10; ++m) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "MISSIONS/CAMPAIGN/CAMPAIGN.%02d/Mission.%02d", c, m);
            if (resolve_path(game, buf)) missions.push_back(buf);
        }
    for (const char* f : {"MISSIONS/Single.01", "MISSIONS/Single.02", "MISSIONS/Multi.01", "MISSIONS/Multi.02", "MISSIONS/Multi.03",
                          "MISSIONS/Multi.04", "MISSIONS/Multi.05", "MISSIONS/Multi.06", "MISSIONS/Autodemo.00"})
        if (resolve_path(game, f)) missions.push_back(f);
    return missions;
}

TerrainGround load_ground(const std::filesystem::path& game, const mission::Tma& tma, std::unique_ptr<Terrain>& keep) {
    std::string map_dir = tma.land_path;
    if (auto cut = map_dir.find_last_of("\\/"); cut != std::string::npos) map_dir.resize(cut);
    keep = std::make_unique<Terrain>(decode_terrain(read_file(*resolve_path(game, map_dir + "/Land.msh"))));
    return TerrainGround(*keep);
}

void test_corpus_hp(AssetLibrary& lib, const std::filesystem::path& game, const TerrainGround& ground) {
    std::printf("buildings corpus: every placed building (data.tma kind 0) as a combatant\n");
    const auto missions = all_missions(game);
    CHECK(missions.size() == 29);
    std::size_t placed = 0, loaded = 0, capturable = 0, doors = 0, pose_mismatch = 0;
    std::map<std::string, float> hp_by_design;
    for (const auto& m : missions) {
        const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, m), "data.tma")));
        combat::CombatWorld world(lib, ground, nullptr);
        for (const auto& o : tma.objects) {
            if (o.kind != 0) continue;
            ++placed;
            auto body = LiveUnit::load_building(lib, o);
            if (!body) {
                std::printf("  %s: %s does not load\n", m.c_str(), o.resource.c_str());
                continue;
            }
            ++loaded;
            Building b(std::move(body));
            capturable += b.can_be_captured();
            for (const auto& it : b.items()) doors += it.type == 0xc;
            // The building's pose (no door moved) equals the static pose of its render mesh at the placement.
            const LiveUnit& u = b.body();
            const Mat4 place = tma_placement(o);
            for (std::size_t t = 1; t < u.posed.size() && t < u.model.mesh.transforms.size(); ++t) {
                if (b.moving_transform(t)) continue;
                const Mat4 want = place * u.model.mesh.transforms[t];
                float err = 0;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) err = std::max(err, std::abs(want.at(r, c) - u.posed[t].at(r, c)));
                if (err > 1e-3f) ++pose_mismatch;
            }
            const std::size_t ci = world.add_unit(b.body(), o.identity, o.ai_index, false);
            const combat::Combatant& c = world.unit(ci);
            CHECK(c.building() && c.damage.initial > 0 && c.damage.removal_delay_ms == 1000);
            hp_by_design[o.resource] = c.damage.initial;
        }
    }
    std::printf("  %zu placed, %zu loaded, %zu with a capture door, %zu doors (type 0xc), %zu posed transforms off\n", placed, loaded,
                capturable, doors, pose_mismatch);
    for (const auto& [design, hp] : hp_by_design) std::printf("    %-40s %9.0f HP\n", design.c_str(), hp);
    CHECK(placed == 167 && loaded == placed);
    CHECK(pose_mismatch == 0);
    // Tut_2 (sums of the NDPR hit points of the FORT agent and its mesh components, scale 1).
    CHECK(hp_by_design["UNITS\\BUILDS\\PLANT\\lplant01.dat"] == 573003);
    CHECK(hp_by_design["UNITS\\BUILDS\\HANGAR\\shang01.dat"] == 98002);
    CHECK(hp_by_design["UNITS\\BUILDS\\GENER\\gener01.dat"] == 2464001);
}

void test_tut2_capture(AssetLibrary& lib, const std::filesystem::path& game) {
    std::printf("buildings corpus: Tut_2, the hero captures the Large Factory (objective 0)\n");
    const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
    const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, mission), "data.tma")));
    std::unique_ptr<Terrain> terrain;
    const TerrainGround ground = load_ground(game, tma, terrain);
    const mission::TmaObject *hero_o = nullptr, *plant_o = nullptr;
    for (const auto& o : tma.objects) {
        if (o.identity == 1) hero_o = &o;
        if (o.identity == 0x80000001) plant_o = &o;
    }
    CHECK(hero_o && plant_o);
    if (!hero_o || !plant_o) return;
    auto hero = LiveUnit::load(lib, *hero_o, ground);
    Building plant(LiveUnit::load_building(lib, *plant_o), 0x80000010);
    CHECK(hero && plant.can_be_captured());
    if (!hero || !plant.can_be_captured()) return;
    // fr_b_plant: the capture door is CTLD device 7 (type 0xd) over joints 4 and 3 (nodes 25, 24: the lift,
    // frames 1..3 at rate 0.2 → 5000 ms).
    const auto& spheres = plant.rest_capture_spheres();
    CHECK(spheres.size() == 2);
    Vec3 entry{};
    for (const auto& s : spheres) entry = entry + s.centre * (1.f / float(spheres.size()));
    std::printf("  capture volume centre (%.1f, %.1f, %.1f), r %.1f / %.1f\n", entry.x, entry.y, entry.z, spheres[0].radius, spheres[1].radius);
    auto place_hero = [&](float x, float y) {
        Mat4 m = Mat4::translation({x, y, 0});
        if (auto g = ground.probe({x, y, 2000}, 4000)) m.at(2, 3) = g->z;
        hero->motion->start(0, m);
        hero->frame = hero->motion->interpolate(0);
    };
    auto contact = [&](bool alive) {
        return std::vector<Building::Contact>{{0, hero->sphere_world(), hero->sphere_radius, alive}};
    };
    // Outside the volume (20 m off): nothing opens.
    place_hero(entry.x + 20, entry.y);
    float now = 0;
    for (int i = 0; i < 100; ++i) CHECK(!plant.update(now += 33, contact(true)));
    CHECK(plant.capture_door()->state == 0);
    // A dead object inside: the lift opens (state 2), then closes when it finds no live capturer (3 → 4 → 0).
    place_hero(entry.x, entry.y);
    std::optional<std::int32_t> got;
    for (int i = 0; i < 200 && !got; ++i) got = plant.update(now += 33, contact(false));
    CHECK(!got && plant.capture_door()->state == 3);
    for (int i = 0; i < 400; ++i) plant.update(now += 33, {});
    CHECK(plant.capture_door()->state == 0);
    // The hero inside and alive: the callback fires when the lift is down.
    const float start = now;
    float fired = -1;
    for (int i = 0; i < 400 && fired < 0; ++i) {
        now += 33;
        if (auto c = plant.update(now, contact(true))) {
            CHECK(*c == 0);
            fired = now;
        }
    }
    std::printf("  captured %.0f ms after entering (state %d)\n", fired - start, plant.capture_door()->state);
    CHECK(fired - start >= 5000 && fired - start <= 5100);
    CHECK(plant.capture_door()->state == 1);
    // While the hero stays the door stays open and nothing fires again; once it leaves the door closes.
    for (int i = 0; i < 100; ++i) CHECK(!plant.update(now += 33, contact(true)));
    place_hero(entry.x + 30, entry.y);
    plant.update(now += 33, contact(true));
    CHECK(plant.capture_door()->state == 3);
    // iron3d's part: the old owner's Fort_Captured, the owner change; the player's script completes objective 0.
    auto rt = game::MissionRuntime::load(game, mission);
    std::uint32_t t = 0;
    for (; t < 3000; t += 33) rt->update(t);
    CHECK(rt->objectives().size() >= 1 && rt->objectives()[0] == 0);
    rt->object_event(0x80000001, 7);
    rt->object_captured(0x80000001, 0);
    CHECK(rt->world().find(0x80000001)->clan == 0);
    for (std::uint32_t end = t + 3000; t < end; t += 33) rt->update(t);
    CHECK(rt->objectives()[0] == 1);
    bool complete = false;
    for (const auto& e : rt->events()) complete |= e.type == game::MissionEvent::Type::ObjectiveComplete && e.id == 0;
    CHECK(complete);
}

void test_tut2_destroy(AssetLibrary& lib, const std::filesystem::path& game) {
    std::printf("buildings corpus: Tut_2, the hero shoots the hangar down (spec §2)\n");
    const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
    const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, mission), "data.tma")));
    std::unique_ptr<Terrain> terrain;
    const TerrainGround ground = load_ground(game, tma, terrain);
    const mission::TmaObject *hero_o = nullptr, *hangar_o = nullptr;
    for (const auto& o : tma.objects) {
        if (o.identity == 1) hero_o = &o;
        if (o.identity == 0x80000003) hangar_o = &o;
    }
    if (!hero_o || !hangar_o) {
        CHECK(!"Tut_2 objects missing");
        return;
    }
    auto hero = LiveUnit::load(lib, *hero_o, ground);
    Building hangar(LiveUnit::load_building(lib, *hangar_o), 0x80000040);
    Mat4 m = Mat4::translation({1230, 780, 0});
    if (auto g = ground.probe({1230, 780, 2000}, 4000)) m.at(2, 3) = g->z;
    hero->motion->start(0, m);
    hero->frame = hero->motion->interpolate(0);
    combat::CombatWorld world(lib, ground, nullptr);
    const std::size_t hi = world.add_unit(*hero, 1, 0, true);
    const std::size_t bi = world.add_unit(hangar.body(), 0x80000003, 2, false);
    combat::Combatant& h = world.unit(hi);
    combat::Combatant& b = world.unit(bi);
    CHECK(b.building() && b.damage.initial == 98002);
    h.select(1), h.select(3); // the laser alone
    h.guns[2].damage_mult = 20; // property 0xb4: 5000 per hit
    h.set_trigger(true);
    auto rt = game::MissionRuntime::load(game, mission);
    float now = 0, killed = -1, removed = -1;
    std::size_t hits = 0, part_effects = 0;
    while (now < 120000 && removed < 0) {
        now += 33;
        hero->pose_parts();
        h.aim_point = b.aim_centre();
        world.update(now);
        for (const auto& e : world.take_events()) {
            if (e.type == combat::CombatEvent::Type::Damaged && e.unit == std::int32_t(bi)) ++hits;
            if (e.type == combat::CombatEvent::Type::Effect && e.unit == std::int32_t(bi)) ++part_effects;
            if (e.type == combat::CombatEvent::Type::Killed && e.unit == std::int32_t(bi)) {
                killed = now;
                rt->world().set_clan(0x80000003, combat::kKilledClan);
            }
            if (e.type == combat::CombatEvent::Type::Removed && e.unit == std::int32_t(bi)) {
                removed = now;
                rt->object_killed(0x80000003);
            }
        }
        rt->update(std::uint32_t(now));
    }
    std::size_t destroyed = 0;
    for (const auto& p : b.damage.parts) destroyed += p.present && p.destroyed();
    std::printf("  %zu hits, %zu part explosion effects, killed at %.0f ms, removed at %.0f ms, %zu parts destroyed\n", hits,
                part_effects, killed, removed, destroyed);
    CHECK(killed > 0 && removed > 0);
    CHECK(std::abs(removed - killed - 1000) <= 34); // body[18] = 1000 ms
    CHECK(part_effects > 0 && b.removed && b.damage.clan == combat::kKilledClan);
    for (std::uint32_t t = std::uint32_t(now); t < std::uint32_t(now) + 3000; t += 33) rt->update(t);
    CHECK(!rt->world().find(0x80000003)->alive);
    CHECK(rt->mission_over() && !rt->mission_complete()); // tut2_pl2: GetObjectClan(0x80000003) == ERROR → failed
}

} // namespace

int building_unit_tests() {
    g_fail = 0;
    test_capture_task();
    return g_fail;
}

int building_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    AssetLibrary lib(game);
    {
        const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
        const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, mission), "data.tma")));
        std::unique_ptr<Terrain> terrain;
        const TerrainGround ground = load_ground(game, tma, terrain);
        test_corpus_hp(lib, game, ground);
    }
    test_tut2_capture(lib, game);
    test_tut2_destroy(lib, game);
    return g_fail;
}
