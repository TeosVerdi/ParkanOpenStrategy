// Strategy-layer tests (src/game: world model, CSuperAI handlers, mission runtime). Handler tests run
// on a hand-made world; the corpus test builds every shipped mission and runs it for 5 minutes of
// game time. Linked into parkan-tests.
#include "core/paths.hpp"
#include "game/mission_runtime.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

using namespace parkan;
using namespace parkan::game;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

// A unit the app would simulate: stands still and takes every order.
struct FakeControl : UnitControl {
    Vec3 pos{};
    std::vector<std::pair<UnitOrder, int>> orders;
    bool accept = true;
    bool give_order(const UnitOrder& o, int mode) override {
        if (accept) orders.push_back({o, mode});
        return accept;
    }
    std::uint32_t current_order() const override { return orders.empty() ? 0 : orders.back().first.type; }
    Vec3 position() const override { return pos; }
    float speed() const override { return 5.f; }
};

constexpr const char* kVarset = R"(
VAR( DWORD, d0, 0)
VAR( DWORD, d1, 1)
VAR( DWORD, d2, 2)
VAR( DWORD, dCurrentProblem, 0)
VAR( DWORD, dCurrentSender, 0)
VAR( DWORD, dMaxPlant, 1)
VAR( DWORD, PBM_TEST, 3)
VAR( DWORD, x0, 0)
VAR( DWORD, x1, 0)
VAR( DWORD, x2, 0)
VAR( DWORD, x3, 0)
VAR( DWORD, x4, 0)
VAR( DWORD, x5, 0)
VAR( DWORD, x6, 0)
VAR( DWORD, x7, 0)
VAR( DWORD, x8, 0)
VAR( DWORD, x9, 0)
VAR( DWORD, x10, 0)
VAR( float, g0, 0)
VAR( float, g1, 0)
VAR( float, g2, 0)
VAR( float, g3, 0)
)";

ai::ScriptProgram test_program() {
    ai::ScriptProgram p;
    p.vars = ai::parse_varset(kVarset);
    const auto idx = [&](std::string_view n) {
        for (std::uint32_t i = 0; i < p.vars.size(); ++i)
            if (p.vars[i].name == n) return i;
        return 0u;
    };
    for (const char* n : {"Init", "Problems0", "Mission", "Fort_Task_Complete", "Fort_Captured", "Mech_GeneratorFound",
                          "Mech_Mineral_Found", "Mech_Task_Complete", "Hero_Teleported"})
        p.events.push_back({n, std::uint32_t(p.events.size()), {}});
    ai::Record solving; // PBM_TEST_Start: SetCurrentProblemState(ST_SOLVING)
    solving.handler = 8;
    solving.refs = {idx("d1")};
    ai::Record nop; // PBM_TEST_Continue: Nop(d0)
    nop.handler = 0;
    nop.refs = {idx("d0")};
    p.events.push_back({"PBM_TEST_Start", 9, {solving}});
    p.events.push_back({"PBM_TEST_Continue", 10, {nop}});
    return p;
}

// Clan 0 = player (hero 1, generator 0x80000002), clan 1 = enemy (warbots 2, 3, plant 0x80000001).
World test_world() {
    World w;
    w.clans.resize(2);
    w.clans[0].relations = {1, 0};
    w.clans[1].relations = {0, 1};
    w.clans[1].base_x = 500, w.clans[1].base_y = 500;
    const auto add = [&](std::uint32_t id, std::uint32_t clan, std::uint32_t type, float x, float y, std::uint32_t size) {
        WorldObject o;
        o.logic_id = id, o.clan = clan, o.type = type, o.placed = {x, y, 0}, o.size_class = size;
        o.armour = kPlaceholderArmour;
        o.weapon = (type & kClassRobot) ? kPlaceholderWeapon : 0.f;
        w.add(o);
    };
    add(1, 0, kTypeHero, 100, 100, 2);
    add(0x80000002, 0, kTypeGenerator, 100, 120, 0);
    add(2, 1, kTypeWarbot, 500, 500, 2);
    add(3, 1, kTypeWarbot, 540, 500, 3);
    add(0x80000001, 1, kTypePlant, 510, 510, 0);
    w.tactical.push_back({0, {{90, 90}, {110, 90}, {110, 110}, {90, 110}}});
    w.tactical.push_back({1, {{400, 400}, {450, 400}, {450, 450}}});
    return w;
}

struct Harness {
    World world = test_world();
    std::vector<SuperAI*> registry = std::vector<SuperAI*>(kMaxClans, nullptr);
    std::vector<std::array<std::uint32_t, 3>> calls; // (channel, kind, b)
    std::map<std::uint32_t, FakeControl> controls;
    std::uint32_t seed = 1;
    std::unique_ptr<SuperAI> player, enemy;

    Harness() {
        auto cb = [this](std::uint32_t, std::uint32_t ch, std::uint32_t k, std::uint32_t b) { calls.push_back({ch, k, b}); };
        auto rand = [this] {
            seed = seed * 214013u + 2531011u;
            return int((seed >> 16) & 0x7fff);
        };
        for (auto& o : world.objects()) {
            controls[o.logic_id].pos = o.placed;
            o.control = &controls[o.logic_id];
        }
        player = std::make_unique<SuperAI>(world, registry, 0, test_program(), cb, rand);
        enemy = std::make_unique<SuperAI>(world, registry, 1, test_program(), cb, rand);
        for (auto& o : world.objects()) registry[o.clan]->on_object_added(o.building() ? 2 : 1, o.logic_id, true);
        player->start(1);
        enemy->start(2);
    }
    // Sets the named variables, then calls handler h on them (refs in order).
    std::uint32_t call(SuperAI& s, std::uint32_t h, std::vector<std::pair<std::string, double>> args) {
        ai::Record r;
        r.handler = h;
        for (auto& [name, v] : args) {
            const auto i = *s.vm().var_index(name);
            if (s.vm().var(i).type == ai::VarType::Float) s.vm().var(i).f = float(v);
            else s.vm().var(i).u = std::uint32_t(std::int64_t(v));
            r.refs.push_back(i);
        }
        return s.call_handler(h, s.vm(), r);
    }
    std::uint32_t var(SuperAI& s, std::string_view name) { return s.vm().var(*s.vm().var_index(name)).get_dword(); }
};

void test_orders() {
    std::printf("game: GiveOrder / guard\n");
    Harness h;
    // GiveOrder(2, PATROL, REPLACE, 250, f×4, TARGET_BY_PLACE, 803, 252)
    const auto patrol = [&](SuperAI& s, double unit, double kind) {
        return h.call(s, 15, {{"x0", unit}, {"x1", 4}, {"x2", 3}, {"x3", 250}, {"g0", 0}, {"g1", 0}, {"g2", 0.5}, {"g3", 1}, {"x4", kind}, {"x5", 803}, {"x6", 252}});
    };
    CHECK(patrol(*h.enemy, 2, 0x202) == 1);
    const auto& o = h.controls[2].orders;
    CHECK(o.size() == 1 && o[0].first.type == 4 && o[0].first.param == 250 && o[0].second == 3);
    CHECK(o.size() == 1 && o[0].first.place.x == 803.f && o[0].first.place.y == 252.f && o[0].first.limits[9] == 0.5f);
    CHECK(patrol(*h.enemy, 99, 0x202) == 5);               // unknown unit
    CHECK(patrol(*h.enemy, 2, 0x205) == 0);                // TARGET_BY_NAME: no design table
    CHECK(h.enemy->neutral_calls[15] == 1);
    h.enemy->start(1);                                     // mode 1, clan 1: GiveOrder is blocked
    CHECK(patrol(*h.enemy, 3, 0x202) == 0 && h.controls[3].orders.empty());
    CHECK(patrol(*h.player, 1, 0x202) == 1);               // clan 0 may always order
}

void test_problems() {
    std::printf("game: problems, groups, clock\n");
    Harness h;
    SuperAI& e = *h.enemy;
    // AddProblem(PBM_TEST, 0.5, lifetime 3, decay 0 (never expires), c 7, d 0, e 0)
    h.call(e, 2, {{"PBM_TEST", 3}, {"g0", 0.5}, {"x1", 3}, {"x2", 0}, {"x3", 7}, {"x4", 0}, {"x5", 0}});
    CHECK(e.problems().size() == 1 && e.problems()[0].type == 3 && e.problems()[0].c == 7);
    // A higher priority for the same (type, c, d) refills the lifetime.
    h.call(e, 2, {{"PBM_TEST", 3}, {"g0", 0.6}, {"x1", 3}, {"x2", 0}, {"x3", 7}, {"x4", 0}, {"x5", 0}});
    CHECK(e.problems().size() == 1 && e.problems()[0].lifetime == 6 && std::abs(e.problems()[0].priority - 0.6f) < 1e-6f);
    e.takt(10'000); // problems age, then _Start runs: SetCurrentProblemState(ST_SOLVING)
    CHECK(e.clock() == 7 && e.problems()[0].state == kStSolving && e.problems()[0].lifetime == 6);
    e.takt(10'001); // within the 7 s period: nothing
    CHECK(e.clock() == 7);
    const auto nops = e.handler_calls[0];
    e.takt(20'000); // _Continue (Nop) runs for the SOLVING problem (with decay > 0 and no units it would
                    // be reset to UNSOLVED first)
    CHECK(e.clock() == 14 && e.handler_calls[0] == nops + 1);

    // CreateGroup(TAKE_BY_HITS, want, TARGET_BY_PLACE, 500, 500): both warbots (10 hits each) fall short of 50.
    h.call(e, 33, {{"x0", 0}}); // SelectProblemsEvent(0): found
    const std::uint32_t g = h.call(e, 25, {{"x0", 2}, {"x1", 50}, {"x2", 0x202}, {"g0", 500}, {"g1", 500}});
    CHECK(g == 0 && h.var(e, "x1") == 20);
    CHECK(e.problems()[0].units.size() == 2 && e.units()[0].problem == 0);
    CHECK(h.call(e, 45, {}) == 20); // ProblemForceHits
    // GroupGiveOrder(g, ATTACK, END, 0, f×4, TARGET_BY_LOGIC_ID, 1)
    CHECK(h.call(e, 28, {{"x0", g}, {"x1", 3}, {"x2", 1}, {"x3", 0}, {"g0", 0}, {"g1", 0}, {"g2", 0}, {"g3", 0}, {"x4", 0x201}, {"x5", 1}}) == 1);
    CHECK(h.controls[2].orders.size() == 1 && h.controls[3].orders.size() == 1 && h.controls[3].orders[0].first.target_id == 1);
    CHECK(h.call(e, 50, {{"x0", 3}}) == 2); // FindProblemUnitWithOrder(ATTACK)
    // ReleaseGroup: the units are free again and (mode 2) get the repair order.
    h.call(e, 24, {{"x0", g}});
    CHECK(e.problems()[0].units.empty() && e.units()[0].problem == kUnitFree && h.controls[2].orders.back().first.type == 9);
    // TakeUnit(3, critical) then SetCurrentProblemState(ST_SOLVED): released, slot freed.
    h.call(e, 6, {{"x0", 3}, {"x1", 1}});
    CHECK(e.problems()[0].units.size() == 1);
    h.call(e, 8, {{"x0", 2}});
    CHECK(e.problems()[0].type == 0 && e.problems()[0].units.empty() && e.units()[1].problem == kUnitFree);
    // Lifetime: 2 ticks with decay 1 clear a problem of lifetime 2.
    h.call(e, 2, {{"PBM_TEST", 3}, {"g0", 0.1}, {"x1", 2}, {"x2", 1}, {"x3", 8}, {"x4", 0}, {"x5", 0}});
    CHECK(e.problems().size() == 1); // the free slot was reused
    e.takt(30'000);
    e.takt(40'000);
    CHECK(e.problems()[0].type == 0);
    // Expressions: ACTION_DESTROY on unit 1, AND mode; killing it solves the problem.
    h.call(e, 2, {{"PBM_TEST", 3}, {"g0", 0.9}, {"x1", 100}, {"x2", 0}, {"x3", 9}, {"x4", 0}, {"x5", 0}});
    h.call(e, 8, {{"x0", 1}}); // SOLVING
    h.call(e, 27, {{"x0", 1}, {"x1", 0}, {"x2", 1}});
    e.takt(50'000);
    CHECK(e.problems()[0].type == 3);
    h.world.kill(1);
    e.takt(60'000);
    CHECK(e.problems()[0].type == 0);

    // Deadlines on the AI clock (+7 per Problems run).
    const std::uint32_t t = h.call(e, 59, {{"x0", 10}});
    CHECK(t == e.clock() + 10);
    CHECK(h.call(e, 60, {{"x0", t}}) == kNoObject);
    e.takt(70'000), e.takt(80'000);
    CHECK(h.call(e, 60, {{"x0", t}}) == 1);
}

void test_queries() {
    std::printf("game: queries, messages, relations\n");
    Harness h;
    SuperAI &p = *h.player, &e = *h.enemy;
    CHECK(h.call(p, 31, {{"x0", 1}, {"x1", 0x01000000}}) == 2);         // CountClanUnits(enemy, CLASS_ROBOT)
    CHECK(h.call(p, 31, {{"x0", 1}, {"x1", 0x80000000}}) == 1);
    CHECK(h.call(p, 34, {{"x0", kTypeHero}}) == 1);                     // CountOwnUnits
    CHECK(h.call(p, 32, {{"x0", 0}, {"x1", 1}}) == 1);                  // hero in tactical areal 0
    CHECK(h.call(p, 32, {{"x0", 1}, {"x1", 1}}) == 0);
    CHECK(h.call(p, 52, {{"x0", 0x80000001}}) == 1);                    // GetObjectClan
    CHECK(h.call(p, 52, {{"x0", 77}}) == kNoObject);
    CHECK(h.call(p, 61, {{"x0", 2}}) == kTypeWarbot && h.call(p, 72, {{"x0", 3}}) == 3);
    h.call(p, 19, {{"x0", 0}, {"x1", 0}, {"x2", 0}});                   // GetClanBase
    CHECK(h.var(p, "x2") == 0);
    h.call(e, 19, {{"x0", 0}, {"x1", 0}, {"x2", 0}});
    CHECK(h.var(e, "x0") == 500 && h.var(e, "x1") == 500 && h.var(e, "x2") == 1);
    // FindEnemyBuildingTarget from the enemy: the player's generator, defended by the hero (10 hits).
    CHECK(h.call(e, 35, {{"g0", 300}, {"x1", 0}}) == 0x80000002 && h.var(e, "x1") == 10);
    CHECK(h.call(e, 44, {{"g0", 100}, {"x1", 0xffffffff}, {"x2", 0x202}, {"g1", 100}, {"g2", 100}}) == 10); // HitsAt
    CHECK(h.call(e, 44, {{"g0", 100}, {"x1", 1}, {"x2", 0x201}, {"x3", 2}}) == 20);
    CHECK(h.call(e, 37, {{"x0", 0}}) == 0x80000001 && h.var(e, "x0") == 20); // FindWeakestOwnBuilding
    // Base radius: SetMaxBaseRadius(300) takes the plant (14 m out); the hero is far outside.
    CHECK(h.call(e, 68, {{"g0", 300}}) == 1 && e.base_radius() > 14.f && e.base_radius() < 15.f);
    CHECK(h.call(e, 63, {{"x0", 2}}) == 1 && h.call(e, 63, {{"x0", 1}}) == 0);
    CHECK(h.call(e, 47, {{"x0", 1000}, {"x1", 0}}) == 1);               // FindBaseIntruder: the hero
    // IsUnitLimitReached(PLANT): one plant registered as a building.
    CHECK(h.call(e, 11, {{"x0", kTypePlant}}) == 1 && h.call(p, 11, {{"x0", kTypePlant}}) == 0);
    // Free units, reservation.
    CHECK(h.call(e, 13, {{"x0", 0}, {"x1", kTypeWarbot}}) == 2);
    h.call(e, 51, {{"x0", 2}, {"x1", 1}});
    CHECK(h.call(e, 13, {{"x0", 0}, {"x1", kTypeWarbot}}) == 3);
    CHECK(h.call(e, 13, {{"x0", 1}, {"x1", 0}}) == kNoObject);          // capturer: size 1–2 only (3 is size 3)
    // Messages and script commands reach the host callback.
    h.call(p, 30, {{"x0", 1}, {"x1", 6}});
    h.call(p, 57, {{"x0", 2}, {"x1", 2}});
    CHECK(h.calls.size() == 2 && h.calls[0] == (std::array<std::uint32_t, 3>{0, 1, 6}) && h.calls[1] == (std::array<std::uint32_t, 3>{2, 2, 2}));
    // Relations mirror into the other clan.
    p.set_relation(1, 2);
    CHECK(e.relation(0) == 2 && std::abs(e.attitude(0) - 0.8333f) < 1e-3f);
    CHECK(h.call(e, 35, {{"g0", 300}, {"x1", 0}}) == kNoObject);      // no longer hostile
    // Brains.
    p.add_brain(), p.add_brain(42);
    CHECK(h.call(p, 49, {}) == 1);
    // Object events: dCurrentSender is set.
    e.notify_object_event(0x80000001, 3);
    CHECK(h.var(e, "dCurrentSender") == 0x80000001);
    // A death empties the record.
    e.on_object_removed(2);
    CHECK(h.call(p, 31, {{"x0", 1}, {"x1", 0x01000000}}) == 1);
}

void test_cfg() {
    std::printf("game: mission.cfg parser\n");
    const auto objs = parse_cfg("# c\nobject script\n\tscript1 = \"create(1, 2, 10, 0, m.dat, 0)\"\nend\n"
                                "object message1\n  message_index = 6 ; the number\n  text_resource = \"T01_T01\"\nend\n");
    CHECK(objs.size() == 2 && objs[0].name == "script" && objs[0].get("script1") && *objs[0].get("script1") == "create(1, 2, 10, 0, m.dat, 0)");
    CHECK(objs.size() == 2 && objs[1].get("message_index") && *objs[1].get("message_index") == "6");
    CHECK(type_code_for_resource("UNITS\\BUILDS\\PLANT\\lplant01.dat") == kTypePlant);
    CHECK(type_code_for_resource("UNITS\\BUILDS\\BUNKER\\mbunk02.dat") == kTypeBunkerMedium);
    CHECK(type_code_for_resource("UNITS\\UNITS\\HERO\\tut2_p.dat") == kTypeHero);
    CHECK(type_code_for_resource("units/units/battle/x.dat") == kTypeWarbot);
}

void test_corpus_missions(const std::filesystem::path& game) {
    std::printf("game: every mission, 5 minutes of game time\n");
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
    CHECK(missions.size() == 29);
    std::uint64_t total_calls = 0, total_neutral = 0;
    std::array<std::uint64_t, 73> neutral_by{};
    for (const auto& m : missions) {
        try {
            auto rt = MissionRuntime::load(game, m);
            std::map<std::uint32_t, std::unique_ptr<FakeControl>> controls;
            for (auto& o : rt->world().objects()) {
                auto c = std::make_unique<FakeControl>();
                c->pos = o.placed;
                o.control = c.get();
                controls[o.logic_id] = std::move(c);
            }
            const bool tut2 = m.ends_with("CAMPAIGN.00/Mission.02");
            bool acted = false;
            for (std::uint32_t t = 0; t <= 300'000; t += 33) {
                rt->update(t);
                if (tut2 && t >= 60'000 && !acted) { // the player captures the plant and kills the animals
                    acted = true;
                    rt->object_captured(0x80000001, 0);
                    rt->object_killed(2), rt->object_killed(3);
                }
            }
            std::uint64_t calls = 0, neutral = 0, given = 0, refused = 0;
            std::size_t problems = 0;
            for (std::uint32_t c = 0; c < rt->world().clans.size(); ++c)
                if (SuperAI* s = rt->super_ai(c)) {
                    for (std::size_t i = 0; i < 73; ++i) calls += s->handler_calls[i], neutral += s->neutral_calls[i], neutral_by[i] += s->neutral_calls[i];
                    given += s->orders_given, refused += s->orders_refused;
                    for (const auto& p : s->problems()) problems += p.type != 0;
                }
            total_calls += calls, total_neutral += neutral;
            std::map<std::string, int> kinds;
            for (const auto& ev : rt->events()) ++kinds[to_string(ev.type)];
            std::string ks;
            for (auto& [k, n] : kinds) ks += " " + k + "×" + std::to_string(n);
            std::printf("  %-38s %zu clans, %6llu calls (%4llu neutral), orders %llu/%llu, %zu open problems;%s\n", m.c_str(),
                        rt->world().clans.size(), (unsigned long long)calls, (unsigned long long)neutral, (unsigned long long)given,
                        (unsigned long long)(given + refused), problems, ks.empty() ? " no events" : ks.c_str());
            if (tut2 || m.ends_with("CAMPAIGN.02/Mission.01")) {
                for (const auto& ev : rt->events())
                    std::printf("      clan %u: %s %u %s\n", ev.clan, to_string(ev.type), ev.id, ev.text.c_str());
            }
            if (tut2) {
                const auto has = [&](MissionEvent::Type t, std::uint32_t id) {
                    for (const auto& ev : rt->events())
                        if (ev.type == t && ev.id == id) return true;
                    return false;
                };
                CHECK(has(MissionEvent::Type::Message, 6));          // the hero starts in tactical areal 0
                CHECK(has(MissionEvent::Type::ObjectiveComplete, 0)); // the plant is ours
                CHECK(has(MissionEvent::Type::ObjectiveComplete, 3)); // the animals are dead
                CHECK(!has(MissionEvent::Type::MissionFailed, 0));
            }
        } catch (const std::exception& ex) {
            ++g_fail;
            std::printf("  FAILED %s: %s\n", m.c_str(), ex.what());
        }
    }
    std::string nb;
    for (std::size_t i = 0; i < 73; ++i)
        if (neutral_by[i]) nb += " h" + std::to_string(i) + "×" + std::to_string(neutral_by[i]);
    std::printf("  total %llu handler calls, %llu on neutral (unimplemented) paths:%s\n", (unsigned long long)total_calls,
                (unsigned long long)total_neutral, nb.c_str());
}

} // namespace

// Property 0x201 for units (MBehaviour slot 0xd8, Behavior 1000cee0): the root entry's 3rd letter.
void test_unit_size_class() {
    CHECK(unit_size_class("R_T_02") == 1 && unit_size_class("R_L_03") == 2 && unit_size_class("R_M_04") == 3);
    CHECK(unit_size_class("R_B_04") == 4 && unit_size_class("r_b_03") == 4);
    CHECK(unit_size_class("R_H_02") == 2 && unit_size_class("r_h_01") == 2); // the hero / l_targ chassis
    CHECK(unit_size_class("R_E_01") == 0 && unit_size_class("R_") == 0 && unit_size_class("") == 0);
}

// Tut_1 ("Capture the neutral warbots"): the hero takes over the neutral clan 3's units (ENTER_STATE,
// OnUICommandDown 10072120): the world clan and the player SuperAI's OnObjectAdded(1); the old clan keeps it.
void test_tut1_unit_capture(const std::filesystem::path& game) {
    if (!resolve_path(game, "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.01")) return;
    auto rt = MissionRuntime::load(game, "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.01");
    const WorldObject* mf1 = rt->world().find(0xd);
    const WorldObject* helic = rt->world().find(0x8);
    const WorldObject* hero = rt->world().find(0x1);
    CHECK(mf1 && mf1->clan == 3 && mf1->size_class == 3); // tut1_mf1, R_M_02: not boardable
    CHECK(helic && helic->clan == 3 && helic->size_class == 1);
    CHECK(hero && hero->size_class == 2);
    for (std::uint32_t t = 0; t <= 3000; t += 33) rt->update(t);
    SuperAI* player = rt->super_ai(0);
    CHECK(player != nullptr);
    if (!player) return;
    auto knows = [&](SuperAI& ai, std::uint32_t lid) {
        return std::any_of(ai.units().begin(), ai.units().end(), [&](const auto& u) { return u.logic_id == lid; });
    };
    CHECK(!knows(*player, 0xd));
    rt->unit_captured(0xd, 0);
    CHECK(rt->world().find(0xd)->clan == 0 && knows(*player, 0xd));
}

int game_unit_tests() {
    g_fail = 0;
    test_unit_size_class();
    test_orders();
    test_problems();
    test_queries();
    test_cfg();
    return g_fail;
}

int game_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    test_corpus_missions(game);
    test_tut1_unit_capture(game);
    return g_fail;
}
