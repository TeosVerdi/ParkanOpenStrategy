// Economy and base-building tests (src/economy, the SuperAI's economy handlers): the distributor's three
// regimes, the mine and its quirks, construction and research on hand-made data and on the shipped
// schemes / trees, the builder's build sequence with a refused placement, the upgrade that keeps the
// LogicID, and a headless corpus run in which the AI clans build, research and produce. Linked into
// parkan-tests.
#include "core/paths.hpp"
#include "economy/assembly.hpp"
#include "economy/economy.hpp"
#include "game/mission_runtime.hpp"
#include "res/library.hpp"

#include <cmath>
#include <functional>
#include <cstdio>
#include <map>

using namespace parkan;
using namespace parkan::economy;
using game::kNoObject;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// A unit the app would simulate: stands still and takes every order.
struct FakeControl : game::UnitControl {
    Vec3 pos{};
    std::vector<std::pair<game::UnitOrder, int>> orders;
    bool give_order(const game::UnitOrder& o, int mode) override {
        orders.push_back({o, mode});
        return true;
    }
    std::uint32_t current_order() const override { return 0; }
    Vec3 position() const override { return pos; }
};

EconomyData test_data() {
    EconomyData d;
    d.professions["prof_generator"] = {.power_out = 10};
    d.professions["prof_mine"] = {.ore_offboard = 1, .use_power = 1};
    d.professions["prof_storage"] = {.ore_offboard = 1, .use_power = 1};
    d.professions["prof_plant"] = {.use_power = 4, .use_ore = 5};
    d.professions["prof_institute"] = {.use_power = 3, .use_ore = 1};
    d.build_table.levels[game::kTypePlant] = {"UNITS\\BUILDS\\PLANT\\splant01.dat", "UNITS\\BUILDS\\PLANT\\mplant01.dat"};
    return d;
}

mission::Scheme scheme(std::uint32_t type, std::string root, std::vector<std::string> parts = {}) {
    mission::Scheme s;
    s.flags = type;
    s.root.lib = "objects.rlb", s.root.entry = std::move(root), s.root.one = 1;
    for (auto& p : parts) {
        mission::SchemeNode n;
        n.lib = "objects.rlb", n.entry = p, n.one = 1;
        s.root.children.push_back(n);
    }
    return s;
}

// A tree of items named after their models: {model, researchPower, researchOre, buildPower, buildOre, state, prereqs}.
struct ItemSpec {
    const char* model;
    float rp, ro, bp, bo;
    std::uint8_t state;
    std::vector<std::uint32_t> pre;
};
Research tree(const std::vector<ItemSpec>& specs) {
    mission::ResearchTree t;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        mission::ResearchItem it;
        it.research_power = specs[i].rp, it.research_ore = specs[i].ro;
        it.build_power = specs[i].bp, it.build_ore = specs[i].bo;
        it.kind = std::uint32_t(i);
        it.initial_state = specs[i].state;
        it.prerequisites = specs[i].pre;
        t.items.push_back(it);
        t.models.push_back(specs[i].model);
        t.model_ids.push_back(std::uint32_t(i) << 16);
    }
    for (std::size_t i = 0; i < t.items.size(); ++i)
        for (auto p : t.items[i].prerequisites) t.items[p].unlocks.push_back(std::uint32_t(i));
    return Research(std::move(t));
}

// The app's legs for a FakeControl: straight to the target at 15 m/s.
struct TestMover : Mover {
    FakeControl* c = nullptr;
    Vec3 target{};
    bool moving = false;
    bool go_to(Vec3 t) override {
        target = t, moving = true;
        return true;
    }
    bool idle() const override { return !moving; }
    void step(float dt) {
        if (!moving) return;
        const float dx = target.x - c->pos.x, dy = target.y - c->pos.y, l = std::hypot(dx, dy), v = 15.f * dt;
        if (l <= v) c->pos.x = target.x, c->pos.y = target.y, moving = false;
        else c->pos.x += dx * v / l, c->pos.y += dy * v / l;
    }
};

game::WorldObject object(std::uint32_t lid, std::uint32_t clan, std::uint32_t type, Vec3 p, std::string res = {}) {
    game::WorldObject o;
    o.logic_id = lid, o.clan = clan, o.type = type, o.placed = p, o.resource = std::move(res);
    return o;
}

// --- the distributor -------------------------------------------------------------------------------

void test_distributor() {
    std::printf("economy: distributor regimes\n");
    std::map<std::uint32_t, EcoState> st;
    Distributor d([&](std::uint32_t lid) { return st.count(lid) ? &st[lid] : nullptr; });
    // A generator + a plant: G = 10·0.2 = 2 ≥ W = 50·(1 − 0.98) = 1 → filled in one step, Take = W / G.
    st[1] = {.type = game::kTypeGenerator, .battery_cap = 100, .battery_frac = 0.3f, .power_out = 10};
    st[2] = {.type = game::kTypePlant, .battery_cap = 50, .battery_frac = 0.98f};
    d.add_equal_share(1), d.add_equal_share(2);
    CHECK(near(d.find(1)->share, 0.5f) && near(d.find(2)->share, 0.5f));
    d.step(0.2f);
    CHECK(near(d.summary().power_available, 2) && near(d.summary().power_usage, 1));
    CHECK(near(st[2].battery_frac, 1.f) && near(st[1].battery_frac, 1.f)); // the generator is kept full
    // G ≤ Ws: two consumers with shares 0.7 / 0.3 and no free share: each gets want · s · G / Ws.
    std::map<std::uint32_t, EcoState>().swap(st);
    Distributor e([&](std::uint32_t lid) { return st.count(lid) ? &st[lid] : nullptr; });
    st[1] = {.type = game::kTypeGenerator, .power_out = 10};
    st[2] = {.type = game::kTypePlant, .battery_cap = 100, .battery_frac = 0};
    st[3] = {.type = game::kTypeInstitute, .battery_cap = 100, .battery_frac = 0};
    e.set_share(1, 0, 1);
    e.set_share(2, 0.7f, 1);
    e.set_share(3, 0.3f, 1);
    CHECK(near(e.find(2)->share, 0.7f) && near(e.find(3)->share, 0.3f));
    e.step(1.0f); // G = 10, W = 200, Ws = 100·0.7 + 100·0.3 = 100: A = 0.1, B = 0
    CHECK(near(st[2].battery_frac * 100, 100 * 0.7f * 10 / 100));
    CHECK(near(st[3].battery_frac * 100, 100 * 0.3f * 10 / 100));
    // Ws < G < W: shares in full, the rest pro rata. Shares 0.5 / 0.5 on two wanters of 100 and one of 100 at 0.
    std::map<std::uint32_t, EcoState>().swap(st);
    Distributor f([&](std::uint32_t lid) { return st.count(lid) ? &st[lid] : nullptr; });
    st[1] = {.type = game::kTypeGenerator, .power_out = 150};
    st[2] = {.type = game::kTypePlant, .battery_cap = 100, .battery_frac = 0};
    st[3] = {.type = game::kTypePlant, .battery_cap = 100, .battery_frac = 0};
    f.set_share(1, 0, 1), f.set_share(2, 1, 1), f.set_share(3, 0, 1);
    f.step(1.0f); // W = 200, Ws = 100, G = 150 → A = 1, B = 0.5
    CHECK(near(st[2].battery_frac, 1.f) && near(st[3].battery_frac, 0.5f));
    // Ore: a storage gives min(ore, rate · dt) · Take; a wanter's buffer is (max − ore).
    std::map<std::uint32_t, EcoState>().swap(st);
    Distributor g([&](std::uint32_t lid) { return st.count(lid) ? &st[lid] : nullptr; });
    st[1] = {.type = game::kTypeStorage, .ore = 1000, .ore_max = 4000, .ore_offboard = 1};
    st[2] = {.type = game::kTypePlant, .ore = 0, .ore_max = 0.1f};
    g.add_equal_share(1), g.add_equal_share(2);
    g.step(1.0f); // G = 1, W = 0.1 → everyone full, Take = 0.1
    CHECK(near(st[2].ore, 0.1f) && near(st[1].ore, 1000 - 0.1f, 1e-3f));
    CHECK(near(g.summary().ore_in_buffer, 1000) && near(g.summary().ore_available, 1));
    // Normalisation: AddEqualShare → 1/n; Remove renormalises the rest.
    Distributor h;
    h.add_equal_share(1), h.add_equal_share(2), h.add_equal_share(3);
    float sum = 0;
    for (auto& r : h.records()) sum += r.share;
    CHECK(near(sum, 1.f));
    h.remove(2);
    CHECK(near(h.find(1)->share + h.find(3)->share, 1.f));
    // Takt: the first call primes; then a step every 192–255 ms.
    Distributor k;
    ai::RandomSource rnd = [] { return 0x80; };
    CHECK(!k.takt(1000, rnd));
    CHECK(!k.takt(1150, rnd));
    CHECK(k.takt(1300, rnd));
}

// --- the mine ----------------------------------------------------------------------------------------

void test_mine() {
    std::printf("economy: mine over a 300-ore lode\n");
    game::World w;
    w.add(object(0x80000001, 0, game::kTypeMine, {100, 100, 0}));
    Economy e(w, test_data(), SchemeCache(""), {{{100, 300, 0}, 0, 0x10001000, 300}, {{100, 900, 0}, 0, 0x10001000, 5000}});
    e.set_object_params(0x80000001, {.current_ore = 0.f, .maximum_ore = 500.f});
    e.start([](std::uint32_t) { return false; }); // no distributor: the mine keeps what it mines
    CHECK(e.current_order(0x80000001) == kOrderMine);
    CHECK(e.lodes()[0].flag == 1 && e.lodes()[1].flag == 0); // only the lode within 250 m is claimed
    CHECK(near(e.state(0x80000001)->ore_left, 300));
    std::uint32_t t = 0;
    for (; t < 60000 && e.current_order(0x80000001) == kOrderMine; t += 33) e.update(t);
    const EcoState* s = e.state(0x80000001);
    // The task ends on the takt whose room (left − stored, before the takt) is no more than its rate r: the
    // room shrinks by 2r a takt and crosses 0 at 150 mined, so it stops between 150 + r/2 and 150 + 1.5r
    // (r = 50/s · 0.45–0.9 s ≤ 45).
    CHECK(e.current_order(0x80000001) == 0);
    CHECK(s->ore >= 150 && s->ore <= 218 && near(s->ore + s->ore_left, 300, 0.5f));
    std::printf("  mined %.1f in %.1f s (left %.1f)\n", s->ore, t / 1000.0, s->ore_left);

    // The re-assert quirk: ore taken from a running mine comes back on its next takt; a full mine stops depleting.
    game::World w2;
    w2.add(object(0x80000001, 0, game::kTypeMine, {0, 0, 0}));
    Economy f(w2, test_data(), SchemeCache(""), {{{0, 0, 0}, 0, 0x10001000, 1e6f}});
    f.set_object_params(0x80000001, {.current_ore = 0.f, .maximum_ore = 500.f});
    f.start([](std::uint32_t) { return false; });
    for (std::uint32_t u = 0; u < 20000; u += 33) f.update(u);
    EcoState* m = f.state(0x80000001);
    CHECK(near(m->ore, 500)); // 50/s: full after 10 s
    const float left = m->ore_left;
    m->ore = 0; // a transport empties it
    for (std::uint32_t u = 20000; u < 22000; u += 33) f.update(u);
    CHECK(near(m->ore, 500));       // restored at once
    CHECK(near(m->ore_left, left)); // and the lode no longer depletes
}

// --- research ----------------------------------------------------------------------------------------

void test_research() {
    std::printf("economy: research of a 100-ore item at pf 1\n");
    Research r = tree({{"base", 0, 0, 0, 0, 7, {}}, {"gun", 0, 100, 1, 10, 5, {0}}, {"turret", 0, 50, 1, 10, 4, {1}}});
    CHECK(r.find_item("GUN") == 1 && r.find_item("none") == -1);
    // MarkResearched makes every present item whose prerequisites are present + researched available.
    Research r2 = r;
    CHECK(r2.mark_researched(1));
    CHECK((r2.state(1) & 3) == 3 && (r2.state(2) & kResAvailable));
    // CalcFullResearchCost of a scheme with the locked turret: turret + its unresearched prerequisite gun.
    const auto c = r.full_research_cost(scheme(game::kTypeWarbot, "base", {"turret"}));
    CHECK(c && near(c->ore, 150));
    CHECK(r.tech_array(scheme(game::kTypeWarbot, "base", {"turret"})).size() == 2);
    CHECK(!r.full_research_cost(scheme(game::kTypeWarbot, "base", {"missing"})));

    game::World w;
    w.clans.resize(1);
    w.add(object(0x80000001, 0, game::kTypeInstitute, {0, 0, 0}));
    w.add(object(0x80000002, 0, game::kTypeStorage, {300, 0, 0}));
    Economy e(w, test_data(), SchemeCache(""), {});
    e.set_research(0, r);
    e.set_object_params(0x80000002, {.current_ore = 4000.f, .maximum_ore = 4000.f});
    e.start([](std::uint32_t) { return true; });
    std::vector<EconomyEvent> ev;
    e.set_event_sink([&](const EconomyEvent& x) { ev.push_back(x); });
    CHECK(e.order_research(0, 1));
    CHECK(!e.order_research(0, 1)); // already queued
    // cancel_order (control slot 0xa0(orderIndex)): a queued item leaves the lab, the other stays.
    CHECK(e.order_research(0, 2));
    CHECK(e.cancel_order(0x80000001, kOrderResearch, 2) && !e.cancel_order(0x80000001, kOrderResearch, 2));
    CHECK((e.research_orders(0) == std::vector<std::int32_t>{1}));
    std::uint32_t done = 0;
    for (std::uint32_t t = 0; t < 200000 && !done; t += 33) {
        e.update(t);
        for (auto& x : ev)
            if (x.type == EconomyEvent::Type::ResearchDone && x.item == 1) done = t;
    }
    std::printf("  item researched after %.1f s\n", done / 1000.0);
    CHECK(done > 95000 && done < 110000); // ≈ ore / (pf · Use_Ore) = 100 s
    CHECK(e.research(0)->state(1) & kResResearched);
    CHECK(e.research(0)->state(2) & kResAvailable); // its unlock
    // FreeTechnoNum: the next item costs nothing (2 s at FreeResearchTime).
    e.set_object_params(0x80000001, {.free_techno = 1, .free_research_time = 2.f});
    ev.clear();
    CHECK(e.order_research(0, 2));
    std::uint32_t t0 = done + 33, d2 = 0;
    for (std::uint32_t t = t0; t < t0 + 10000 && !d2; t += 33) {
        e.update(t);
        for (auto& x : ev)
            if (x.type == EconomyEvent::Type::ResearchDone && x.item == 2) d2 = t - t0;
    }
    CHECK(d2 > 1500 && d2 < 4000);
}

// --- construction ----------------------------------------------------------------------------------

void test_construct() {
    std::printf("economy: free bot on a size-4 plant\n");
    game::World w;
    w.clans.resize(1);
    w.add(object(0x80000001, 0, game::kTypePlant, {500, 500, 10}, "UNITS\\BUILDS\\PLANT\\b_plant1.dat"));
    SchemeCache sc("");
    sc.put("UNITS\\BUILDS\\PLANT\\b_plant1.dat", scheme(game::kTypePlant, "fr_b_plant"));
    sc.put("UNITS\\UNITS\\AI\\small.dat", scheme(game::kTypeWarbot, "R_L_01", {"gun"}));
    sc.put("UNITS\\UNITS\\AI\\huge.dat", scheme(game::kTypeWarbot, "R_B_01", {"gun"}));
    Economy e(w, test_data(), std::move(sc), {});
    e.set_research(0, tree({{"R_L_01", 0, 0, 10, 40, 7, {}}, {"gun", 0, 0, 5, 20, 5, {}}, {"R_B_01", 0, 0, 1, 1, 7, {}}}));
    std::vector<std::uint32_t> brains{kNoObject, kNoObject};
    e.set_brains([&](std::uint32_t) { return &brains; });
    e.set_object_params(0x80000001, {.free_bots = 1});
    std::vector<EconomyEvent> ev;
    e.set_event_sink([&](const EconomyEvent& x) { ev.push_back(x); });
    e.start([](std::uint32_t) { return true; });
    e.wrap_controls();
    CHECK(e.building_size(0x80000001) == 4);
    CHECK(e.order_construct(0x80000001, "UNITS\\UNITS\\AI\\small.dat"));
    std::uint32_t produced = 0, unit = kNoObject;
    for (std::uint32_t t = 0; t < 60000 && !produced; t += 33) {
        e.update(t);
        if (t == 5000) {
            const auto p = e.plant(0x80000001);
            CHECK(p && p->busy && near(p->time_cost, 20) && p->progress > 0.2f && p->progress < 0.3f);
            CHECK(std::count(brains.begin(), brains.end(), 0u) == 1); // reserved for the build
        }
        for (auto& x : ev)
            if (x.type == EconomyEvent::Type::UnitProduced) produced = t, unit = x.object;
    }
    std::printf("  robot produced after %.1f s, LogicID %u\n", produced / 1000.0, unit);
    CHECK(produced >= 20000 && produced < 21500); // free-bot table: plant 4 × robot 2 = 20 s
    const game::WorldObject* u = w.live(unit);
    CHECK(u && u->clan == 0 && u->type == game::kTypeWarbot && u->size_class == 2 && near(u->placed.z, 25));
    CHECK(std::count(brains.begin(), brains.end(), unit) == 1); // the robot holds a brain (flag 8)
    CHECK(e.plant(0x80000001)->free_bots == 0);
    // Without free bots the research tree prices it: an unresearched part refuses the order at its start.
    ev.clear();
    CHECK(e.order_construct(0x80000001, "UNITS\\UNITS\\AI\\small.dat"));
    for (std::uint32_t t = 60000; t < 62000; t += 33) e.update(t);
    bool failed = false;
    for (auto& x : ev) failed = failed || (x.type == EconomyEvent::Type::TaskFailed && x.code == kOrderConstruct && x.item == 0x102);
    CHECK(failed);
    e.research(0)->mark_researched(1);
    // A robot larger than the plant's size class is refused too (this plant is 4: R_B_ = 4 passes).
    CHECK(e.order_construct(0x80000001, "UNITS\\UNITS\\AI\\huge.dat"));
    for (std::uint32_t t = 62000; t < 64000; t += 33) e.update(t);
    const auto p = e.plant(0x80000001);
    CHECK(p && p->busy && near(p->ore_cost, 21) && near(p->power_cost, 6)); // Σ build costs: root 1/1 + gun 20/5
}

// --- builder: build sequence with a refused placement, upgrade keeping the LogicID ------------------

void test_builder() {
    std::printf("economy: builder, placement refusal, build show, upgrade\n");
    game::World w;
    w.clans.resize(1);
    w.add(object(1, 0, game::kTypeBuilder, {0, 0, 0}));
    w.add(object(0x80000001, 0, game::kTypeStorage, {400, 0, 0}));
    SchemeCache sc("");
    sc.put("UNITS\\BUILDS\\PLANT\\splant01.dat", scheme(game::kTypePlant, "fr_l_plant"));
    sc.put("UNITS\\BUILDS\\PLANT\\mplant01.dat", scheme(game::kTypePlant, "fr_m_plant"));
    Economy e(w, test_data(), std::move(sc), {});
    e.set_research(0, tree({{"fr_l_plant", 0, 0, 6, 300, 7, {}}, {"fr_m_plant", 0, 0, 17, 650, 7, {}}}));
    e.set_object_params(1, {.current_ore = 0.f, .maximum_ore = 2000.f});
    e.set_object_params(0x80000001, {.current_ore = 4000.f, .maximum_ore = 4000.f});
    std::vector<EconomyEvent> ev;
    e.set_event_sink([&](const EconomyEvent& x) { ev.push_back(x); });
    FakeControl builder;
    w.attach(1, &builder);
    e.start([](std::uint32_t) { return false; });
    e.wrap_controls();
    const auto has = [&](EconomyEvent::Type t) {
        for (auto& x : ev)
            if (x.type == t) return &x;
        return static_cast<EconomyEvent*>(nullptr);
    };
    // Too close to the storage: the bounding spheres (35 + 60 + 4 m) intersect → notify 0x103.
    CHECK(!e.placement_ok(game::kTypePlant, {450, 0, 0}) && e.placement_ok(game::kTypePlant, {0, 600, 0}));
    CHECK(e.order_build(1, game::kTypePlant, {450, 0, 0}));
    std::uint32_t t = 0;
    for (; t < 200000 && !has(EconomyEvent::Type::TaskFailed); t += 33) e.update(t);
    const EconomyEvent* f = has(EconomyEvent::Type::TaskFailed);
    CHECK(f && f->code == kOrderBuild && f->item == 0x103);
    CHECK(near(e.state(1)->ore, 300) && near(e.state(0x80000001)->ore, 3700)); // it loaded the cost at the storage
    // A free site: the builder already carries the ore (≥ Building_Cost): straight to the site.
    ev.clear();
    CHECK(e.order_build(1, game::kTypePlant, {0, 600, 0}, 0.5f));
    CHECK(e.builder(1)->order == kOrderBuild);
    std::uint32_t placed = 0, built = 0, plant = kNoObject;
    for (; t < 400000 && !built; t += 33) {
        e.update(t);
        if (auto* p = has(EconomyEvent::Type::BuildingPlaced); p && !placed) placed = t, plant = p->object;
        if (has(EconomyEvent::Type::BuildingBuilt)) built = t;
        // The show's steps: hidden (flags 8) from 0 s, the build footprint (flags 1) from 30 s.
        if (placed && !built && t - placed > 6000 && t - placed < 6040) CHECK(e.showing(plant) && e.footprint(plant) == 0);
        if (placed && !built && t - placed > 32000 && t - placed < 32040) CHECK(e.showing(plant) && e.footprint(plant) == 2);
    }
    std::printf("  placed at %.1f s, built %.1f s later (LogicID %08x)\n", placed / 1000.0, (built - placed) / 1000.0, plant);
    // MLogicIDMap (ArealMap 1002b4d0 / 1002b2f0): registering 0x80000001 sets the building counter to 2, the
    // allocation increments it first → 0x80000003.
    CHECK(plant == 0x80000003 && w.live(plant) && w.live(plant)->type == game::kTypePlant);
    CHECK(built - placed > 40000 && built - placed < 43000); // the 41 s show
    CHECK(near(e.state(1)->ore, 0) && e.footprint(plant) == 1);
    // Upgrade: level 0 → 1 (BuildDat.lst), same LogicID, 50 s, the ore kept.
    e.state(plant)->ore = 12;
    CHECK(e.can_upgrade(1, plant));
    ev.clear();
    CHECK(e.order_upgrade(1, plant));
    std::uint32_t up = 0;
    const std::uint32_t t0 = t;
    for (; t < t0 + 120000 && !up; t += 33) {
        e.update(t);
        if (auto* u = has(EconomyEvent::Type::BuildingUpgraded)) {
            up = t;
            CHECK(u->object == plant);
        }
    }
    std::printf("  upgraded after %.1f s\n", (up - t0) / 1000.0);
    // The builder (no Mover: 10 m/s on a timer) walks 525 m to the stand point, then the 50 s SphereDown.
    CHECK(up - t0 > 102000 && up - t0 < 106000);
    const game::WorldObject* nb = w.live(plant);
    CHECK(nb && nb->resource == "UNITS\\BUILDS\\PLANT\\mplant01.dat" && e.level(plant) == 1 && e.building_size(plant) == 3);
    CHECK(near(e.state(plant)->ore, 12));
    CHECK(!e.can_upgrade(1, plant)); // the last level
    for (std::uint32_t k = t; k < t + 10000; k += 33) e.update(k);
    CHECK(e.current_order(1) == 0 && has(EconomyEvent::Type::TaskComplete));
}

// --- the shipped data ------------------------------------------------------------------------------

void test_real_costs(const std::filesystem::path& game) {
    std::printf("economy: shipped schemes and trees\n");
    auto trf = resolve_path(game, "MISSIONS/SCRIPTS/full.trf");
    CHECK(trf.has_value());
    if (!trf) return;
    Research full(mission::load_research_tree(*trf));
    SchemeCache sc(game);
    const auto cost = [&](const char* f) {
        const mission::Scheme* s = sc.get(f);
        return s ? full.build_cost(*s) : SchemeCost{0, 0, false};
    };
    // designs-research.md §5.1 examples.
    const SchemeCost a = cost("UNITS/UNITS/AI/12hel1.dat"), b = cost("UNITS/UNITS/AI/32_l1.dat"), c = cost("UNITS/BUILDS/PLANT/b_plant1.dat");
    CHECK(a.ok && near(a.power, 33.5f) && near(a.ore, 50.5f));
    CHECK(b.ok && near(b.power, 268.5f) && near(b.ore, 534.5f));
    CHECK(c.ok && near(c.power, 282) && near(c.ore, 1910));
    // The scheme writer: every shipped design re-encodes to a file that decodes the same.
    std::size_t files = 0, identical = 0, same = 0;
    for (const char* dir : {"UNITS/UNITS/AI", "UNITS/BUILDS/PLANT", "UNITS/BUILDS/MINE", "UNITS/BUILDS/INSTITUT", "UNITS/UNITS/BATTLE"}) {
        auto d = resolve_path(game, dir);
        if (!d) continue;
        for (const auto& f : std::filesystem::directory_iterator(*d)) {
            if (economy::normalize_path(f.path().extension().string()) != ".DAT") continue;
            const Bytes raw = read_file(f.path());
            try {
                const mission::Scheme sc1 = mission::decode_scheme(raw);
                const Bytes enc = mission::encode_scheme(sc1);
                ++files;
                identical += enc == raw;
                const std::function<bool(const mission::SchemeNode&, const mission::SchemeNode&)> eq = [&](auto& x, auto& y) {
                    if (x.lib != y.lib || x.entry != y.entry || x.name != y.name || x.one != y.one || x.mount != y.mount ||
                        x.category != y.category || x.children.size() != y.children.size())
                        return false;
                    for (std::size_t i = 0; i < x.children.size(); ++i)
                        if (!eq(x.children[i], y.children[i])) return false;
                    return true;
                };
                const mission::Scheme sc2 = mission::decode_scheme(enc);
                same += sc2.flags == sc1.flags && eq(sc1.root, sc2.root);
            } catch (const std::exception&) {
            }
        }
    }
    std::printf("  scheme writer: %zu files, %zu byte-identical, %zu decode the same\n", files, identical, same);
    CHECK(files > 100 && same == files);
    // BuildDat.lst and behpsp.res.
    game::World w;
    Economy e(w, game, nullptr);
    CHECK(e.data().build_table.level_count(game::kTypeInstitute) == 4 && e.data().build_table.level_count(game::kTypeHangar) == 1);
    CHECK(e.data().professions.count("prof_plant") && near(e.data().professions.at("prof_plant").use_power, 4) &&
          near(e.data().professions.at("prof_institute").use_ore, 1) && near(e.data().professions.at("prof_generator").power_out, 10));
    // Batteries: i_pws_b_01's CTLD device (type 0x13) property 0.
    mission::Scheme bat = scheme(game::kTypeWarbot, "i_pws_b_01");
    AssetLibrary lib(game);
    const float cap = battery_capacity(bat, [&](std::string_view l, std::string_view n) -> std::span<const std::uint8_t> {
        const NResEntry* r = lib.find(l, n);
        return r ? r->data : std::span<const std::uint8_t>{};
    });
    std::printf("  12hel1 P %.1f O %.1f, b_plant1 P %.0f O %.0f, i_pws_b_01 capacity %.0f\n", a.power, a.ore, c.power, c.ore, cap);
    CHECK(near(cap, 25000)); // header +0x2c (CDevice::Load copies header +0x2c.. to +0x54; GetParam(0) reads +0x54)

    // A real construction: 12hel1 at a plant fed by a storage, on the full tree (no free bot).
    game::World w2;
    w2.clans.resize(1);
    w2.add(object(0x80000001, 0, game::kTypePlant, {500, 500, 0}, "UNITS\\BUILDS\\PLANT\\b_plant1.dat"));
    w2.add(object(0x80000002, 0, game::kTypeStorage, {800, 500, 0}, "UNITS\\BUILDS\\STORAGE\\sstore01.dat"));
    w2.add(object(0x80000003, 0, game::kTypeGenerator, {500, 800, 0}, "UNITS\\BUILDS\\GENER\\gener01.dat"));
    Economy x(w2, game, nullptr);
    x.set_research(0, full);
    x.set_object_params(0x80000002, {.current_ore = 4000.f, .maximum_ore = 4000.f});
    std::vector<std::uint32_t> brains{kNoObject};
    x.set_brains([&](std::uint32_t) { return &brains; });
    std::uint32_t produced = 0;
    x.set_event_sink([&](const EconomyEvent& ev) {
        if (ev.type == EconomyEvent::Type::UnitProduced) produced = x.now();
    });
    x.start([](std::uint32_t) { return true; });
    x.wrap_controls();
    std::printf("  b_plant1 battery %.0f, size %u\n", x.state(0x80000001)->battery_cap, x.building_size(0x80000001));
    // Its control devices (fr_b_plant.ctl with the i_pws_f_03 / i_fsh_f_03 / i_rps_f_03 modules, then the
    // u_pln_def_a_01 component): four batteries of 8 (power 500/s) and one type-0x1a device with param0 5, so
    // pf = GetParam(0x300) = 5 · HP ratio 1 · k 1 (GetDevicePropertyById 1002c040, GetPowerFactor 100198e0).
    const combat::EnergyBudget* pb = x.power(0x80000001);
    CHECK(pb && pb->devices.size() == 14 && near(*pb->battery_capacity(), 32));
    if (pb) {
        std::size_t batteries = 0, consumers = 0;
        for (const auto& d : pb->devices) batteries += d.type == 0x13 && near(d.power, 500), consumers += d.type == 0x1a && near(d.param[0], 5);
        CHECK(batteries == 4 && consumers == 1);
    }
    CHECK(near(x.state(0x80000001)->power_factor(), 5));
    CHECK(x.order_construct(0x80000001, "UNITS\\UNITS\\AI\\12hel1.dat"));
    for (std::uint32_t t = 0; t < 600000 && !produced; t += 33) x.update(t);
    std::printf("  12hel1 produced after %.1f s (ore 50.5 / power 33.5, storage %.1f left), pf %.2f, battery %.3f\n", produced / 1000.0,
                x.state(0x80000002)->ore, x.state(0x80000001)->power_factor(), x.state(0x80000001)->battery_frac);
    // At pf 5 the plant collects power 5·Use_Power 4 = 20/s (33.5 → ≈ 1.7 s) and the ore cost is 50.5 / 5; the
    // ore requests (≈ 0.2·pf of the remainder a takt) and M_Task_Construct's 5 s minimum set the time.
    CHECK(produced > 5000 && produced < 30000);

    // A real research: the first available, unresearched item of a campaign tree, at a fed institute.
    auto ptrf = resolve_path(game, "MISSIONS/SCRIPTS/c1m3p.trf");
    CHECK(ptrf.has_value());
    if (!ptrf) return;
    Research pr(mission::load_research_tree(*ptrf));
    std::int32_t item = -1;
    for (std::int32_t i = 0; i < std::int32_t(pr.item_count()) && item < 0; ++i)
        if ((pr.state(i) & (kResAvailable | kResResearched | kResPresent)) == (kResAvailable | kResPresent) && pr.info(i)->research_ore > 0 &&
            !pr.unlocks(i).empty())
            item = i;
    CHECK(item >= 0);
    if (item < 0) return;
    game::World w3;
    w3.clans.resize(1);
    w3.add(object(0x80000001, 0, game::kTypeInstitute, {0, 0, 0}, "UNITS\\BUILDS\\INSTITUT\\sinst01.dat"));
    w3.add(object(0x80000002, 0, game::kTypeStorage, {300, 0, 0}, "UNITS\\BUILDS\\STORAGE\\sstore01.dat"));
    w3.add(object(0x80000003, 0, game::kTypeGenerator, {0, 300, 0}, "UNITS\\BUILDS\\GENER\\gener01.dat"));
    Economy y(w3, game, nullptr);
    y.set_research(0, pr);
    y.set_object_params(0x80000002, {.current_ore = 4000.f, .maximum_ore = 4000.f});
    std::uint32_t done = 0;
    y.set_event_sink([&](const EconomyEvent& ev) {
        if (ev.type == EconomyEvent::Type::ResearchDone && ev.item == item) done = y.now();
    });
    y.start([](std::uint32_t) { return true; });
    CHECK(y.order_research(0, item));
    const float ore = pr.info(item)->research_ore;
    for (std::uint32_t t = 0; t < 2000000 && !done; t += 33) y.update(t);
    std::printf("  c1m3p.trf item %d (%s, ore %.0f, power %.0f) researched after %.1f s\n", item, pr.item(item)->code.c_str(), ore,
                pr.info(item)->research_power, done / 1000.0);
    CHECK(done > 0 && done / 1000.f >= ore * 0.9f);
    CHECK(y.research(0)->state(item) & kResResearched);
}

// --- the headless corpus run: the AI clans build, research and produce ------------------------------

void test_corpus_ai(const std::filesystem::path& game) {
    const std::uint32_t minutes = 20;
    std::printf("economy: AI economy over %u minutes of game time\n", minutes);
    std::uint64_t total_units = 0;
    std::vector<std::string> missions;
    for (int c = 0; c < 6; ++c)
        for (int k = 1; k < 10; ++k) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "MISSIONS/CAMPAIGN/CAMPAIGN.%02d/Mission.%02d", c, k);
            if (resolve_path(game, buf)) missions.push_back(buf);
        }
    for (const char* f : {"MISSIONS/Single.01", "MISSIONS/Single.02", "MISSIONS/Autodemo.00"}) missions.push_back(f);
    int economic = 0;
    for (const std::string& ms : missions) {
        const char* m = ms.c_str();
        if (!resolve_path(game, m)) continue;
        try {
            auto rt = game::MissionRuntime::load(game, m);
            std::map<std::uint32_t, std::unique_ptr<FakeControl>> controls;
            std::map<std::uint32_t, std::unique_ptr<TestMover>> movers;
            const auto legs = [&](std::uint32_t lid) {
                auto m = std::make_unique<TestMover>();
                m->c = controls[lid].get();
                rt->economy()->set_mover(lid, m.get());
                movers[lid] = std::move(m);
            };
            for (auto& o : rt->world().objects()) {
                auto c = std::make_unique<FakeControl>();
                c->pos = o.placed;
                o.control = c.get();
                controls[o.logic_id] = std::move(c);
                if (!o.building()) legs(o.logic_id);
            }
            struct Count {
                int units = 0, research = 0, placed = 0, built = 0, upgraded = 0, failed = 0;
            };
            std::map<std::uint32_t, Count> by_clan;
            rt->set_economy_sink([&](const EconomyEvent& e) {
                Count& c = by_clan[e.clan];
                switch (e.type) {
                case EconomyEvent::Type::UnitProduced: { // the app's part: a body (here a still control)
                    ++c.units;
                    auto fc = std::make_unique<FakeControl>();
                    fc->pos = rt->world().find(e.object)->placed;
                    rt->world().attach(e.object, fc.get());
                    controls[e.object] = std::move(fc);
                    legs(e.object);
                    break;
                }
                case EconomyEvent::Type::ResearchDone: ++c.research; break;
                case EconomyEvent::Type::BuildingPlaced: ++c.placed; break;
                case EconomyEvent::Type::BuildingBuilt: ++c.built; break;
                case EconomyEvent::Type::BuildingUpgraded: ++c.upgraded; break;
                case EconomyEvent::Type::TaskFailed: ++c.failed; break;
                default: break;
                }
            });
            for (std::uint32_t t = 0; t <= minutes * 60'000; t += 50) {
                rt->update(t);
                for (auto& [lid, m] : movers) m->step(0.05f);
            }
            bool header = false;
            int ai_units = 0;
            for (std::uint32_t clan = 0; clan < rt->world().clans.size(); ++clan) {
                const Count& c = by_clan[clan];
                const game::SuperAI* s = rt->super_ai(clan);
                const Distributor* d = rt->economy()->distributor(clan);
                const bool computer = clan != rt->player_clan() && rt->world().clans[clan].mode != 0;
                if (computer) ai_units += c.units;
                if (!c.units && !c.research && !c.placed && !(s && s->handler_calls[43])) continue;
                if (!header) std::printf("  %s\n", m), header = true;
                std::printf("    clan %u %-8s %s: %d robots, %d researched, %d placed / %d built, %d upgraded, %d failed orders; "
                            "power %.2f/%.2f, buffer %.0f, brains %u free of %zu, designs %zu; calls h15 %llu h18 %llu h41 %llu h43 %llu\n",
                            clan, rt->world().clans[clan].name.c_str(), computer ? "AI    " : "player", c.units, c.research, c.placed, c.built,
                            c.upgraded, c.failed, d ? d->summary().power_available : 0.f, d ? d->summary().power_usage : 0.f,
                            d ? d->summary().ore_in_buffer : 0.f, rt->economy()->free_brains(clan),
                            s ? const_cast<game::SuperAI*>(s)->brains().size() : 0,
                            s ? const_cast<game::SuperAI*>(s)->planner().entries.size() : 0, s ? (unsigned long long)s->handler_calls[15] : 0ull,
                            s ? (unsigned long long)s->handler_calls[18] : 0ull, s ? (unsigned long long)s->handler_calls[41] : 0ull,
                            s ? (unsigned long long)s->handler_calls[43] : 0ull);
            }
            total_units += std::uint64_t(ai_units);
            economic += ai_units > 0;
            if (std::string_view(m).starts_with("MISSIONS/Single")) CHECK(ai_units > 0);
        } catch (const std::exception& ex) {
            std::printf("  %s: %s\n", m, ex.what());
            ++g_fail;
        }
    }
    std::printf("  AI clans produced %llu robots in all, in %d of %zu missions\n", (unsigned long long)total_units, economic, missions.size());
    CHECK(total_units > 0);
}

} // namespace

void test_building_size_class();

int economy_unit_tests() {
    g_fail = 0;
    test_distributor();
    test_mine();
    test_research();
    test_construct();
    test_builder();
    test_building_size_class();
    return g_fail;
}

// The assembled design's mounts, slots and stats (notes/mounts-slots.md §5, notes/unit-stats.md §9).
void test_assembly(const std::filesystem::path& game) {
    std::printf("economy: assembly and stats of PREBLD scr_pre1\n");
    AssetLibrary lib(game);
    auto p = resolve_path(game, "UNITS/UNITS/PREBLD/scr_pre1.dat");
    CHECK(p);
    if (!p) return;
    const mission::Scheme s = mission::decode_scheme(read_file(*p));
    Assembly a(lib);
    CHECK(assemble(a, s));
    // Parts: chassis 0..11 (node 7 e_tur_lt), turret 12..19 (central_lc at 15, universal_lr at 18 and 19), gun 20..22.
    CHECK(a.part_count() == 23);
    CHECK(a.part_name(7) && *a.part_name(7) == "e_tur_lt");
    CHECK(a.part_name(15) && *a.part_name(15) == "central_lc" && a.part_name(18) && *a.part_name(19) == "universal_lr");
    CHECK(!a.part_name(0) && !a.part_name(20));
    // Devices: chassis 0..15 (i_eng_l … i_arm_l at 5), turret 16..19 (i_rdr_l 17, i_def_l 19), gun 20 (clip slot i_c01_l).
    CHECK(a.device_count() == 21);
    CHECK(a.device_name(0) && *a.device_name(0) == "i_eng_l" && a.device_name(5) && *a.device_name(5) == "i_arm_l");
    CHECK(a.device_name(17) && *a.device_name(17) == "i_rdr_l" && a.device_name(20) && *a.device_name(20) == "i_c01_l");
    CHECK(a.find_part(0) == 0 && a.count_parts(0) == 12);
    const auto st = design_stats(lib, s);
    CHECK(st);
    if (!st) return;
    std::printf("  mass %.1f, free payload %.1f, speed %.2f m/s, defence %.1f, offence %.1f, radar %.0f, code %s\n", st->mass,
                st->payload_free, st->speed, st->defence, st->offence, st->radar.value_or(-1),
                unit_debug_code(st->size_class, st->chassis_type, s.flags).c_str());
    CHECK(near(st->mass, 5556.4f, 0.5f) && near(st->payload_free, 2818.6f, 0.5f));
    CHECK(near(st->speed_nominal, 33.33f, 0.01f) && near(st->speed, 16.73f, 0.02f));
    CHECK(near(st->defence, 1483.1f, 0.5f) && near(st->offence, 600.f, 0.5f) && st->radar && near(*st->radar, 250.f));
    CHECK(st->valid() && st->size_class == 2 && st->chassis_type == 3);
    CHECK(unit_debug_code(st->size_class, st->chassis_type, s.flags) == "SWW");
}

// Control class 3 size class and upgrade level (buildings.md section 8.2, 1000cee0 / 10005e60).
void test_building_size_class() {
    std::printf("economy: building size class\n");
    const auto sc = [](const char* r) { return building_size_class(scheme(game::kTypePlant, r)); };
    CHECK(sc("fr_e_inst") == 5 && sc("fr_e_brige") == 5 && sc("fr_l_plant") == 2 && sc("fr_m_plant") == 3);
    CHECK(sc("fr_b_plant") == 4 && sc("fr_b_towH") == 4 && sc("FR_B_PLANT") == 4);
    CHECK(sc("fr_t_x") == 0 && sc("fr_") == 0 && sc("") == 0); // t is 0 for a building
    CHECK(robot_size(scheme(game::kTypePlant | game::kBuildingBit, "fr_t_x")) == 1); // robot_size unchanged
    CHECK(robot_size(scheme(game::kTypePlant | game::kBuildingBit, "fr_e_inst")) == 4);
    CHECK(game::level_from_size_class(2) == 0 && game::level_from_size_class(3) == 1 && game::level_from_size_class(4) == 2 &&
          game::level_from_size_class(5) == 3 && game::level_from_size_class(0) == 0);
}

// Every FORT record of objects.rlb: size class by the letter at entry[3], level from it.
void test_corpus_building_classes(const std::filesystem::path& game) {
    std::printf("economy: FORT size classes (corpus)\n");
    AssetLibrary lib(game);
    int forts = 0;
    for (const auto& e : lib.archive("objects.rlb").entries()) {
        if (e.type != fourcc("FORT")) continue;
        ++forts;
        const std::uint32_t sz = game::building_size_class(e.name), lv = game::level_from_size_class(sz);
        const char c = char(std::tolower((unsigned char)e.name[3]));
        const std::uint32_t want_sz = c == 'l' ? 2 : c == 'm' ? 3 : c == 'b' ? 4 : c == 'e' ? 5 : 0;
        CHECK(sz == want_sz && sz >= 2); // no t building exists
        CHECK(lv == (c == 'l' ? 0u : c == 'm' ? 1u : c == 'b' ? 2u : 3u));
    }
    CHECK(forts == 34);
}

int economy_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    test_assembly(game);
    test_corpus_building_classes(game);
    test_real_costs(game);
    test_corpus_ai(game);
    return g_fail;
}
