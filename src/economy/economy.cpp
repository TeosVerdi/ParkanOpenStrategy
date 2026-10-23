#include "economy/economy.hpp"

#include "core/paths.hpp"
#include "res/library.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::economy {

using namespace game;

namespace {

constexpr float kBuildingMargin = 4.0f;      // 10059848: the placement test's sphere margin
constexpr float kMineLodeRadius = 250.f;     // M_Task_Mine::SetTarget (1002cd10): lodes within 250 m (3-D)
constexpr float kConstructTime = 5.f;        // M_Task_Construct: T = 5 s (0x40a00000)
constexpr float kSpawnHeight = 15.f;         // 1002a920: z = ground + 15 m
constexpr std::uint32_t kUpgradeHold = 50000; // M_Task_Upgrade state 2: 50 000 ms
constexpr float kStorageFactor = 1.5f;       // 10029110: a storage must hold more than Building_Cost · 1.5

float dist3(Vec3 a, Vec3 b) { return std::sqrt(dot(a - b, a - b)); }
float dist2(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y); }
float ratio(float c, float cap) { return cap > 0 ? c / cap : 1.f; }

EconomyEvent make_event(EconomyEvent::Type t, std::uint32_t object, std::uint32_t source = kNoObject, std::uint32_t clan = kNoClan) {
    EconomyEvent e;
    e.type = t;
    e.object = object, e.source = source, e.clan = clan;
    return e;
}

// M_Task_Construct free-bot time table (plant size × robot size), base-building.md §3.1.
float free_bot_time(std::uint32_t plant, std::uint32_t robot) {
    static const float t[3][4] = {{30, 60, 20, 20}, {20, 35, 60, 20}, {10, 20, 40, 60}};
    if (plant < 2 || plant > 4 || robot < 1 || robot > 4) return 20;
    return t[plant - 2][robot - 1];
}

} // namespace

bool is_economy_order(std::uint32_t t) {
    return t == kOrderTransport || t == kOrderBuild || t == kOrderMine || t == kOrderConstruct || t == kOrderResearch ||
           t == kOrderResearchScheme || t == kOrderShowUpgrade || t == kOrderUpgrade;
}

const char* to_string(EconomyEvent::Type t) {
    using T = EconomyEvent::Type;
    switch (t) {
    case T::UnitProduced: return "unit produced";
    case T::BuildingPlaced: return "building placed";
    case T::BuildingBuilt: return "building built";
    case T::BuildingUpgraded: return "building upgraded";
    case T::ResearchDone: return "research done";
    case T::TaskComplete: return "task complete";
    case T::TaskFailed: return "task failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------
// Workers and tasks

struct Economy::Task {
    explicit Task(const UnitOrder& o) : order(o) {}
    virtual ~Task() = default;
    virtual bool set_target(Economy&, Worker&) { return true; } // slot 3; false = refused
    virtual bool start(Economy&, Worker&) { return true; }      // slot 6; false = "Cannot StartTask"
    virtual bool takt(Economy&, Worker&, float dt) = 0;          // slot 7; false = over
    virtual void abort(Economy&, Worker&) {}                     // dropped before its end
    virtual void fill(BuilderInfo&) const {}
    UnitOrder order;
    bool started = false;
    std::uint32_t last = 0;
    float progress = 0;
    std::uint32_t fail = 0; // the failure notify (0x102, 0x103, …); 0 = ended normally
};

struct Economy::Worker {
    std::uint32_t lid = kNoObject;
    EcoState st;
    ObjectParams params;
    std::uint32_t level = 0, size_class = 0;
    float angle = 0;
    std::int32_t animation = 0; // MB+0x9fc
    int footprint = 1;          // MB+0x968: 1 normal, 2 build show, 0 hidden
    std::deque<std::unique_ptr<Task>> tasks;
    ai::MTimer timer{0, 7, 7}; // MB+0x938
    Mover* mover = nullptr;
    std::uint32_t walk_until = 0; // fallback travel timer
    bool alive = true, initialized = false;
};

// Friend access for the tasks (they live in an anonymous namespace).
struct EconomyAccess {
    static void emit(Economy& e, const EconomyEvent& ev) { e.emit(ev); }
    static std::uint32_t create(Economy& e, std::string_view s, Vec3 p, float a, std::uint32_t clan, std::uint32_t src, std::uint32_t keep) {
        return e.create_object(s, p, a, clan, src, keep);
    }
    static std::uint32_t* claim_brain(Economy& e, std::uint32_t clan, std::uint32_t owner) { return e.claim_brain(clan, owner); }
    static bool walk(Economy& e, std::uint32_t lid, Vec3 t) { return e.walk(lid, t); }
    static bool walked(const Economy& e, std::uint32_t lid) { return e.walking_done(lid); }
    static Economy::Worker& worker(Economy& e, std::uint32_t lid) { return e.worker(lid); }
    static std::vector<std::uint32_t>* brains(Economy& e, std::uint32_t clan) { return e.brains_ ? e.brains_(clan) : nullptr; }
    static int rand(Economy& e) { return e.rand_(); }
    static void join(Economy& e, std::uint32_t lid) { e.join_distributor(lid); }
    static void leave(Economy& e, std::uint32_t lid, std::uint32_t clan) { e.leave_distributor(lid, clan); }
};

namespace {

// --- M_Task_Mine (order 0xa) ---
struct MineTask : Economy::Task {
    using Task::Task;
    float left = 0, stored = 0; // T+0x58 / T+0x5c
    bool set_target(Economy& e, Economy::Worker& w) override { // 1002cd10
        w.st.ore_left = 0;
        left = 0;
        const WorldObject* o = e.world().live(w.lid);
        if (!o) return false;
        bool found = false;
        for (auto& l : e.lodes())
            if (dist3(l.pos, o->position()) <= kMineLodeRadius) {
                left += l.amount;
                w.st.ore_left = left;
                l.flag = 1; // map slot 0xb8: the lode is taken
                found = true;
            }
        return found;
    }
    bool start(Economy& e, Economy::Worker& w) override { // 1002cef0
        w.st.power_usage = e.data().professions.count("prof_mine") ? e.data().professions.at("prof_mine").use_power : 1.f;
        return true;
    }
    bool takt(Economy& e, Economy::Worker& w, float dt) override { // 1002cf60
        if (left <= 0) return false;
        const float pf = w.st.power_factor();
        const float pf_use = w.st.power_usage > 0 ? pf : 1.f;
        if (pf < 0.1f) { // "SetPowerUsage to": an unpowered mine loses what it holds
            stored = 0, progress = 0, w.st.ore = 0;
            return true;
        }
        float rate = dt * e.data().constants.mine_ore_per_second * pf_use;
        const float room = left - stored;
        if (stored + rate > w.st.ore_max) rate = w.st.ore_max - stored;
        stored += rate;
        left -= rate;
        w.st.ore_left = left;
        // [confirmed 1002d057..1002d10c] The attribute is overwritten every takt, whatever the distributor or
        // a transport took from it: a full mine (stored == oreMax) gives ore for ever and stops depleting.
        w.st.ore = stored;
        if (room > rate) {
            progress = left > 0 ? stored / left : 1.f;
            return true;
        }
        progress = 1; // "All Ore mined..."
        w.st.power_usage = 0;
        return false;
    }
};

// --- M_Task_ShowUpgrade (order 0x12) ---
struct ShowStep {
    std::int32_t anim;
    std::uint32_t flags;
    bool push;
    float seconds;
};
const std::vector<ShowStep>& show_steps(std::uint32_t param) {
    static const std::vector<ShowStep> k0{{1, 8, false, 5}, {-1, 0, true, 25}, {-1, 1, true, 5}, {2, 0, false, 5}, {0, 2, false, 1}};
    static const std::vector<ShowStep> k1{{0x309, 0, true, 25}, {-1, 1, false, 1}, {8, 0, false, 90}};
    static const std::vector<ShowStep> k2{{10, 1, false, 3}, {0, 2, false, 1}};
    return param == 1 ? k1 : param == 2 ? k2 : k0;
}

struct ShowUpgradeTask : Economy::Task {
    using Task::Task;
    std::size_t step = 0;
    std::uint32_t step_start = 0;
    bool entered = false;
    void enter(Economy& e, Economy::Worker& w) {
        const ShowStep& s = show_steps(order.param)[step];
        step_start = e.now();
        entered = true;
        if (s.anim != -1) w.animation = s.anim; // and the building's animation unless 0x309
        if (s.flags & 1) w.footprint = 2;
        if (s.flags & 8) w.footprint = 0;
        if (!s.push) return;
        // Every unit within (its radius + the building's − 15 m), not leaving or upgrading, gets
        // ORDER_LEAVE {0x14, param rand, target 0x201 = this building}, insert 2.
        const WorldObject* b = e.world().live(w.lid);
        if (!b) return;
        for (auto& o : e.world().objects()) {
            if (!o.alive || o.building() || !o.control) continue;
            const std::uint32_t cur = o.current_order();
            if (cur == kOrderLeave || cur == kOrderUpgrade) continue;
            if (dist2(o.position(), b->position()) >= e.radius(o) + e.radius(*b) - 15.f) continue;
            UnitOrder leave;
            leave.type = kOrderLeave;
            leave.param = std::uint32_t(EconomyAccess::rand(e)); // "param rand"
            leave.target_kind = kTargetLogicId;
            leave.target_id = w.lid;
            o.control->give_order(leave, 2);
        }
    }
    bool start(Economy& e, Economy::Worker& w) override {
        step = 0;
        enter(e, w);
        return true;
    }
    bool takt(Economy& e, Economy::Worker& w, float) override {
        const auto& steps = show_steps(order.param);
        while (e.now() - step_start >= std::uint32_t(steps[step].seconds * 1000)) {
            if (++step >= steps.size()) {
                w.footprint = 1, w.animation = 0; // normal footprint, refresh, animation 0
                EconomyEvent ev = make_event(EconomyEvent::Type::BuildingBuilt, w.lid);
                if (const WorldObject* o = e.world().find(w.lid)) ev.clan = o->clan, ev.scheme = o->resource;
                EconomyAccess::emit(e, ev);
                return false;
            }
            const std::uint32_t prev_end = step_start + std::uint32_t(steps[step - 1].seconds * 1000);
            enter(e, w);
            step_start = prev_end; // the steps follow each other exactly
        }
        progress = float(step) / float(steps.size());
        return true;
    }
};

} // namespace

namespace {

const Profession& prof_of(const Economy& e, std::uint32_t type) {
    static const Profession kNone;
    auto it = e.data().professions.find(profession_sheet(type));
    return it == e.data().professions.end() ? kNone : it->second;
}

// --- M_Task_Construct (order 0xc on a plant) ---
struct ConstructTask : Economy::Task {
    using Task::Task;
    float ore = 0, power = 0, time = kConstructTime, ore_got = 0, power_got = 0, elapsed = 0; // T+0x64..+0x78
    bool free_bot = false;
    std::uint32_t* reserved = nullptr;
    std::size_t reserved_index = 0;
    std::uint32_t clan = kNoClan;

    void release(Economy& e) {
        if (auto* b = EconomyAccess::brains(e, clan); b && reserved_index < b->size() && (*b)[reserved_index] == 0)
            (*b)[reserved_index] = kNoObject; // owner 0 → -1
        reserved = nullptr;
    }
    bool start(Economy& e, Economy::Worker& w) override { // 10029ba0
        ore = power = ore_got = power_got = elapsed = progress = 0;
        time = kConstructTime;
        const WorldObject* o = e.world().live(w.lid);
        if (!o) return false;
        clan = o->clan;
        auto* brains = EconomyAccess::brains(e, clan);
        std::optional<std::size_t> free;
        for (std::size_t i = 0; brains && i < brains->size(); ++i)
            if ((*brains)[i] == kNoObject) {
                free = i;
                break;
            }
        if (!free) return fail_with(0x101); // " No free brain"
        const mission::Scheme* s = e.schemes().get(order.design);
        if (!s) return fail_with(0x101);
        const std::uint32_t size = robot_size(*s);
        if (size > w.size_class) return fail_with(0x101); // "Robot SizedType not match"
        const float pf = w.st.power_factor();
        const Profession& prof = prof_of(e, w.st.type);
        if (w.params.free_bots > 0) {
            free_bot = true;
            ore = 0, power = 1, time = free_bot_time(w.size_class, size);
        } else {
            const Research* r = e.research(clan);
            if (!r) return fail_with(0x102);
            const SchemeCost c = r->build_cost(*s);
            if (!c.ok) return fail_with(0x102); // "Failed to create … due to technology"
            ore = c.ore, power = c.power;
            if (pf > 0.01f) ore /= pf; // "NewfOreCost"
        }
        w.st.ore = 0, w.st.ore_max = ore; // SetAttribute(0x2000100, 0, &request)
        w.st.power_usage = prof.use_power;  // [inferred for a free bot: the spec names it for the normal path]
        (*brains)[*free] = 0; // reserved (owner 0) at the end of StartTask
        reserved_index = *free;
        reserved = &(*brains)[*free];
        return true;
    }
    bool fail_with(std::uint32_t code) {
        fail = code;
        return false;
    }
    bool takt(Economy& e, Economy::Worker& w, float dt) override { // 1002a4f0
        const float pf = w.st.power_factor();
        const Profession& prof = prof_of(e, w.st.type);
        elapsed += dt;
        power_got += pf * prof.use_power * dt;
        if (ore_got + 0.01f < ore) {
            const float f = ore_got / (ore + 0.01f);
            const float k = f < 0.2f ? 0.2f : (f > 0.8f ? 3.0f : f);
            const float request = (ore - ore_got) * k * pf * 0.2f + 0.07f;
            const float take = std::min(w.st.ore, request); // 10015540
            ore_got += take;
            w.st.ore = 0, w.st.ore_max = request; // buffer emptied, capacity = request
        }
        if (power_got + 0.01f >= power) w.st.power_usage = 0;
        if (ore_got + 0.01f >= ore) w.st.ore = 0, w.st.ore_max = 0;
        progress = std::min({1.f, ratio(power_got, power), ratio(ore_got, ore), ratio(elapsed, time)});
        if (power_got + 0.01f < power || ore_got + 0.01f < ore || elapsed + 0.01f < time) return true;
        release(e);
        // 1002a920: at the plant's (x, y), z = ground + 15 m, identity rotation, flags 8 (claims a brain).
        const WorldObject* plant = e.world().live(w.lid);
        if (!plant) return false;
        Vec3 p = plant->position();
        p.z = (e.ground_height ? e.ground_height(p.x, p.y) : p.z) + kSpawnHeight;
        const std::uint32_t unit = EconomyAccess::create(e, order.design, p, 0, plant->clan, w.lid, kNoObject);
        if (unit == kNoObject) {
            fail = 0x103;
            return false;
        }
        if (w.params.free_bots > 0) --w.params.free_bots;
        w.st.power_usage = 0;
        EconomyEvent ev = make_event(EconomyEvent::Type::UnitProduced, unit, w.lid, plant->clan);
        ev.scheme = order.design;
        EconomyAccess::emit(e, ev);
        // ORDER_LEAVE {0x14, param -1, flags 0, target 0x204}, insert mode 3: it walks out of the plant.
        if (WorldObject* u = e.world().find(unit); u && u->control) {
            UnitOrder leave;
            leave.type = kOrderLeave, leave.param = kNoObject, leave.target_kind = kTargetNone;
            u->control->give_order(leave, 3);
        }
        return false;
    }
    void abort(Economy& e, Economy::Worker& w) override {
        if (reserved) release(e);
        w.st.power_usage = 0;
        w.st.ore = 0, w.st.ore_max = 0;
    }
};

// --- M_Task_Research (order 0xe one item, 0x10 a whole scheme) ---
struct ResearchTask : Economy::Task {
    using Task::Task;
    std::vector<std::int32_t> list; // +0x58
    std::int32_t current = -1;      // +0x6c
    float ore = 0, power = 0, time = 2, ore_got = 0, power_got = 0, elapsed = 0;
    std::uint32_t clan = kNoClan;

    bool set_target(Economy& e, Economy::Worker& w) override { // 1002f470
        const WorldObject* o = e.world().live(w.lid);
        if (!o) return false;
        clan = o->clan;
        const Research* r = e.research(clan);
        if (!r) return false;
        if (order.type == kOrderResearchScheme) order.param = kNoObject; // CreateTaskFromOrder forces -1
        if (order.target_kind == kTargetNone && order.param != kNoObject) {
            list = {std::int32_t(order.param)};
            return true;
        }
        if (order.target_kind == kTargetName) {
            const mission::Scheme* s = e.schemes().get(order.design);
            if (!s) return false;
            list = r->tech_array(*s);
            return true;
        }
        return false;
    }
    bool takt(Economy& e, Economy::Worker& w, float dt) override { // 1002f730
        Research* r = e.research(clan);
        if (!r) return false;
        const Profession& prof = prof_of(e, w.st.type);
        if (current == -1) {
            for (std::int32_t i : list)
                if (auto inf = r->info(i); inf && !inf->researched && inf->available) {
                    current = i;
                    break;
                }
            if (current == -1) { // "cannot research more..."
                w.st.ore = 0, w.st.ore_max = 0;
                w.st.power_usage = 0;
                return false;
            }
            if (w.params.free_techno > 0) { // MB+0x9f4
                --w.params.free_techno;
                ore = power = 0;
            } else {
                const auto inf = r->info(current);
                ore = inf->research_ore, power = inf->research_power;
            }
            time = w.params.free_research_time; // MB+0x9f8
            ore_got = power_got = elapsed = 0;
            w.st.power_usage = prof.use_power;
            w.st.ore = 0, w.st.ore_max = ore;
        }
        if (r->info(current)->researched) { // "Techno N allready researched"
            current = -1;
            return true;
        }
        const float pf = w.st.power_factor();
        const float m0 = std::min({ratio(power_got, power), ratio(ore_got, ore), ratio(elapsed, time)});
        const float take = std::min(w.st.ore, pf * prof.use_ore * dt);
        ore_got += take;
        float rem = ore - ore_got;
        if (rem > 5) rem *= 0.5f;
        if (rem < 0) rem = 0;
        w.st.ore = 0, w.st.ore_max = rem; // buffer emptied (the excess is discarded), new request
        power_got += pf * prof.use_power * dt;
        elapsed += dt;
        const float m1 = std::min({ratio(power_got, power), ratio(ore_got, ore), ratio(elapsed, time)});
        const float p = std::min(r->progress(current) + (m1 - m0), 1.f);
        r->set_progress(current, p);
        progress = p;
        if (p + 0.001f >= 1) { // MResearchCenter::OnTechResearched (10023aa0)
            r->mark_researched(current);
            r->set_progress(current, 1.f);
            EconomyEvent ev = make_event(EconomyEvent::Type::ResearchDone, w.lid, w.lid, clan);
            ev.item = current;
            EconomyAccess::emit(e, ev);
            current = -1;
        }
        return true;
    }
    void abort(Economy&, Economy::Worker& w) override {
        w.st.power_usage = 0;
        w.st.ore = 0, w.st.ore_max = 0;
    }
};

// Nearest own mine/storage holding more than `min_ore` (10029110).
std::uint32_t nearest_store(Economy& e, const WorldObject& unit, float min_ore, bool mines, bool storages) {
    std::uint32_t best = kNoObject;
    float best_d = 0;
    for (const auto& o : e.world().objects()) {
        if (!o.alive || o.clan != unit.clan) continue;
        if (!((mines && o.type == kTypeMine) || (storages && o.type == kTypeStorage))) continue;
        const EcoState* s = e.state(o.logic_id);
        if (!s || s->ore <= min_ore) continue;
        const float d = dist2(o.position(), unit.position());
        if (best == kNoObject || d < best_d) best = o.logic_id, best_d = d;
    }
    return best;
}

// A point next to `site` on the way from `from` (10029300 stand point) [port: radius + 15 m short].
Vec3 stand_point(Vec3 site, Vec3 from, float r) {
    Vec3 d = from - site;
    d.z = 0;
    const float l = std::hypot(d.x, d.y);
    if (l < 1e-3f) return {site.x + r + 15.f, site.y, site.z};
    const float k = (r + 15.f) / l;
    return {site.x + d.x * k, site.y + d.y * k, site.z};
}

// --- M_Task_Build (order 7, a builder) ---
struct BuildTask : Economy::Task {
    using Task::Task;
    int state = 0;
    std::string scheme;
    float ore_cost = 0;                  // T+0x120
    std::uint32_t storage = kNoObject;   // the storage / mine used
    std::uint32_t arrived = 0;           // T+0x128
    bool walking = false;
    Vec3 site{};
    std::uint32_t clan = kNoClan;

    bool start(Economy& e, Economy::Worker& w) override { // 10028800
        const WorldObject* u = e.world().live(w.lid);
        if (!u) return false;
        clan = u->clan;
        site = order.place;
        if (order.target_kind == kTargetPlace && e.ground_height) site.z = e.ground_height(site.x, site.y); // z = ground height
        const std::string* path = e.building_scheme(order.param, 0);
        if (!path) return fail_with(0x102);
        scheme = *path;
        const mission::Scheme* s = e.schemes().get(scheme);
        if (!s) return fail_with(0x102);
        const Research* r = e.research(clan);
        if (!r) return fail_with(0x102);
        const SchemeCost c = r->build_cost(*s);
        if (!c.ok) return fail_with(0x102); // notify 0x102 → OnBuilderFailed
        ore_cost = c.ore;
        // 1001cf00: a known building code, equal to the order's type when one is given.
        if (!(s->flags & kBuildingBit) || (order.param != kTempBuilding && s->flags != order.param)) return fail_with(0x102);
        pick(e, w);
        return true;
    }
    bool fail_with(std::uint32_t code) {
        fail = code;
        return false;
    }
    void pick(Economy& e, Economy::Worker& w) { // slot 0x3c (10028ff0)
        walking = false;
        const BehConstants& k = e.data().constants;
        if (order.param == kTypeMine || w.st.ore >= k.building_cost) { // mines cost no carried ore
            state = 3;
            return;
        }
        const WorldObject* u = e.world().live(w.lid);
        storage = u ? nearest_store(e, *u, k.building_cost * kStorageFactor, true, true) : kNoObject;
        state = storage == kNoObject ? 0 : 1; // "Cannot"
    }
    bool takt(Economy& e, Economy::Worker& w, float dt) override { // 10028b80
        const BehConstants& k = e.data().constants;
        switch (state) {
        case 0: pick(e, w); return true;
        case 1: { // GoToStorage
            const WorldObject* s = e.world().live(storage);
            if (!s || s->clan != clan) {
                pick(e, w);
                return true;
            }
            if (!walking) walking = EconomyAccess::walk(e, w.lid, stand_point(s->position(), e.world().find(w.lid)->position(), e.radius(*s)));
            if (walking && EconomyAccess::walked(e, w.lid)) state = 2, walking = false;
            return true;
        }
        case 2: { // OreOnBoard
            EcoState* s = e.state(storage);
            const WorldObject* so = e.world().live(storage);
            if (!s || !so || so->clan != clan) {
                pick(e, w);
                return true;
            }
            const float target = order.param == kNoObject ? k.transport_max_ore : ore_cost;
            const float need = std::max(0.f, target - w.st.ore);
            const float take = std::min({dt * k.transport_ore_onboard_per_second, s->ore, need});
            s->ore -= take;
            w.st.ore += take;
            if (w.st.ore >= target) state = 3;
            else if (s->ore < 0.01f) pick(e, w); // [inferred] the storage ran dry: look again
            return true;
        }
        case 3: { // GoToBuild
            const WorldObject* u = e.world().find(w.lid);
            if (!walking) {
                const std::string* path = &scheme;
                float r = 30;
                if (const mission::Scheme* s = e.schemes().get(*path)) {
                    WorldObject probe;
                    probe.type = s->flags;
                    r = e.radius(probe);
                }
                walking = EconomyAccess::walk(e, w.lid, stand_point(site, u->position(), r));
                if (!walking) return true;
            }
            if (EconomyAccess::walked(e, w.lid)) state = 4, walking = false, arrived = e.now();
            return true;
        }
        case 4: { // Wait, then create the building (10029240)
            if (e.now() - arrived < 1000) return true;
            const std::uint32_t b = EconomyAccess::create(e, scheme, site, order.angle, clan, w.lid, kNoObject);
            if (b == kNoObject) {
                fail = 0x103; // the bounding-sphere test refused it
                return false;
            }
            w.st.ore -= ore_cost; // may go negative: the next build loads the debt too
            EconomyEvent ev = make_event(EconomyEvent::Type::BuildingPlaced, b, w.lid, clan);
            ev.scheme = scheme;
            EconomyAccess::emit(e, ev);
            UnitOrder show;
            show.type = kOrderShowUpgrade, show.param = 0, show.target_kind = kTargetNone;
            e.give_order(b, show, 2); // [inferred] insert 2: its default order (a mine's Mine) follows the show
            return false;
        }
        default: return false;
        }
    }
    void fill(BuilderInfo& i) const override {
        i.state = state, i.building_type = order.param, i.target = storage, i.ore_cost = ore_cost;
    }
};

// --- M_Task_Upgrade (order 0x18, a builder) ---
struct UpgradeTask : Economy::Task {
    using Task::Task;
    int state = 1;
    std::uint32_t since = 0;
    bool walking = false;
    bool set_target(Economy& e, Economy::Worker& w) override { // 100332e0
        if (order.target_kind != kTargetLogicId) return false;
        return e.can_upgrade(w.lid, order.target_id);
    }
    bool takt(Economy& e, Economy::Worker& w, float) override {
        const WorldObject* b = e.world().live(order.target_id);
        switch (state) {
        case 1: { // GoToBuild
            if (!b) return false;
            if (!walking) {
                walking = EconomyAccess::walk(e, w.lid, stand_point(b->position(), e.world().find(w.lid)->position(), e.radius(*b)));
                if (!walking) return false;
            }
            if (!EconomyAccess::walked(e, w.lid) || e.showing(order.target_id)) return true;
            UnitOrder show;
            show.type = kOrderShowUpgrade, show.param = 1, show.target_kind = kTargetNone;
            e.give_order(order.target_id, show, 2);
            since = e.now();
            state = 2; // SphereDown
            return true;
        }
        case 2:
            if (e.now() - since < kUpgradeHold) return true;
            replace(e);
            return state == 4;
        case 4: return e.showing(order.target_id);
        default: return false;
        }
    }
    // Read the building's clan, LogicID, type, matrix and ore; delete it; create the next level at the same
    // matrix with the same LogicID (flags 4); level + 1; the old ore.
    void replace(Economy& e) {
        WorldObject* b = e.world().find(order.target_id);
        if (!b || !b->alive) {
            state = 0;
            return;
        }
        auto& bw = EconomyAccess::worker(e, b->logic_id);
        const std::uint32_t lvl = bw.level + 1;
        const std::string* path = e.building_scheme(b->type, lvl);
        if (!path) {
            state = 0;
            return;
        }
        const float ore = bw.st.ore;
        const std::uint32_t clan = b->clan;
        const std::uint32_t got = EconomyAccess::create(e, *path, b->position(), bw.angle, clan, b->logic_id, b->logic_id);
        if (got == kNoObject) {
            state = 0;
            return;
        }
        auto& nw = EconomyAccess::worker(e, got);
        nw.level = lvl;
        nw.st.ore = ore;
        EconomyEvent ev = make_event(EconomyEvent::Type::BuildingUpgraded, got, order.target_id, clan);
        ev.scheme = *path;
        EconomyAccess::emit(e, ev);
        // [inferred] The new building plays the short show (param 2, 4 s); state 4 waits for its end.
        UnitOrder show;
        show.type = kOrderShowUpgrade, show.param = 2, show.target_kind = kTargetNone;
        e.give_order(got, show, 2);
        state = 4;
    }
    void abort(Economy& e, Economy::Worker&) override {
        if (state == 2) replace(e); // the builder died: the replacement happens at once
    }
    void fill(BuilderInfo& i) const override { i.state = state, i.target = order.target_id; }
};

// --- M_Task_Transport (order 6): mine → storage runs ---
struct TransportTask : Economy::Task {
    using Task::Task;
    int state = 0; // 0 Cannot, 1 GoToMine, 2 OreOnBoard, 3 GoToStorage, 4 OreOffBoard, 5/6 waiting
    std::uint32_t mine = kNoObject, storage = kNoObject; // slots 0x40 / 0x44
    bool walking = false;
    std::uint32_t clan = kNoClan;
    std::uint32_t wait_until = 0;

    void pick(Economy& e, Economy::Worker& w) {
        walking = false;
        const WorldObject* u = e.world().live(w.lid);
        if (!u) return;
        clan = u->clan;
        // [inferred] the nearest own mine holding ore and the nearest own storage with room.
        mine = nearest_store(e, *u, 0, true, false);
        storage = kNoObject;
        float best = 0;
        for (const auto& o : e.world().objects()) {
            if (!o.alive || o.clan != u->clan || o.type != kTypeStorage) continue;
            const EcoState* s = e.state(o.logic_id);
            if (!s || s->ore_max - s->ore <= 0.01f) continue;
            const float d = dist2(o.position(), u->position());
            if (storage == kNoObject || d < best) storage = o.logic_id, best = d;
        }
        if (storage == kNoObject || (mine == kNoObject && w.st.ore <= 0)) state = 0;
        else state = w.st.ore > 0 ? 3 : 1;
    }
    bool start(Economy& e, Economy::Worker& w) override {
        pick(e, w);
        return true;
    }
    bool go(Economy& e, Economy::Worker& w, std::uint32_t lid) {
        const WorldObject* t = e.world().live(lid);
        if (!t || t->clan != clan) {
            pick(e, w);
            return false;
        }
        if (!walking) walking = EconomyAccess::walk(e, w.lid, stand_point(t->position(), e.world().find(w.lid)->position(), e.radius(*t)));
        if (walking && EconomyAccess::walked(e, w.lid)) {
            walking = false;
            return true;
        }
        return false;
    }
    bool takt(Economy& e, Economy::Worker& w, float dt) override {
        const BehConstants& k = e.data().constants;
        if (const WorldObject* u = e.world().live(w.lid); u && u->clan != clan && state != 0) pick(e, w); // clan change
        switch (state) {
        case 0: pick(e, w); return true;
        case 1:
            if (go(e, w, mine)) state = 2;
            return true;
        case 2: {
            EcoState* m = e.state(mine);
            if (!m || !e.world().live(mine)) {
                pick(e, w);
                return true;
            }
            const float take = std::min({dt * k.transport_ore_onboard_per_second, m->ore, w.st.ore_max - w.st.ore});
            m->ore -= take;
            w.st.ore += take;
            if (w.st.ore >= w.st.ore_max - 0.01f || (m->ore <= 0.01f && w.st.ore > 0)) state = 3;
            return true;
        }
        case 3:
            if (go(e, w, storage)) state = 4;
            return true;
        case 4: {
            EcoState* s = e.state(storage);
            if (!s || !e.world().live(storage)) {
                pick(e, w);
                return true;
            }
            const float give = std::min({dt * k.transport_ore_offboard_per_second, w.st.ore, s->ore_max - s->ore});
            s->ore += give;
            w.st.ore -= give;
            if (w.st.ore <= 0.01f) {
                w.st.ore = std::max(w.st.ore, 0.f);
                pick(e, w);
            } else if (s->ore_max - s->ore <= 0.01f) { // the storage is full: wait near it
                const WorldObject* so = e.world().live(storage);
                for (int i = 0; i < 100 && so; ++i) { // random points ±30 m, 100 tries
                    const Vec3 p = so->position();
                    const float rx = float(EconomyAccess::rand(e) & 0xff) / 256.f, ry = float(EconomyAccess::rand(e) & 0xff) / 256.f;
                    if (EconomyAccess::walk(e, w.lid, {p.x + (rx - 0.5f) * 60, p.y + (ry - 0.5f) * 60, p.z})) break;
                }
                state = 5;
                wait_until = e.now() + 5000;
            }
            return true;
        }
        case 5:
        case 6:
            state = 6;
            if (e.now() >= wait_until) {
                if (EcoState* s = e.state(storage); s && s->ore_max - s->ore > 0.01f) state = 3, walking = false;
                else wait_until = e.now() + 5000;
            }
            return true;
        default: return false;
        }
    }
    void fill(BuilderInfo& i) const override { i.state = state, i.target = state <= 2 ? mine : storage; }
};

} // namespace

// ---------------------------------------------------------------------------------------------------
// EconomyControl

bool EconomyControl::give_order(const UnitOrder& order, int mode) {
    if (is_economy_order(order.type)) return eco_.give_order(lid_, order, mode);
    if (!inner_) return false;
    if (eco_.current_order(lid_) != 0) {
        if (mode == 1) { // held until the economy task ends (the stack's append) [port]
            pending_.push_back({order, mode});
            return true;
        }
        eco_.cancel(lid_);
    }
    return inner_->give_order(order, mode);
}

std::uint32_t EconomyControl::current_order() const {
    const std::uint32_t c = eco_.current_order(lid_);
    return c != 0 ? c : inner_ ? inner_->current_order() : 0;
}

Vec3 EconomyControl::position() const {
    if (inner_) return inner_->position();
    const WorldObject* o = eco_.world().find(lid_);
    return o ? o->placed : Vec3{};
}

// ---------------------------------------------------------------------------------------------------
// Economy

Economy::Economy(World& world, EconomyData data, SchemeCache schemes, std::vector<Lode> lodes, std::uint32_t seed)
    : world_(world), data_(std::move(data)), schemes_(std::move(schemes)), lodes_(std::move(lodes)), rand_seed_(seed) {
    rand_ = [this] {
        rand_seed_ = rand_seed_ * 214013u + 2531011u;
        return int((rand_seed_ >> 16) & 0x7fff);
    };
}

Economy::Economy(World& world, const std::filesystem::path& game_dir, const mission::Tma* tma, std::uint32_t seed)
    : Economy(world, EconomyData{}, SchemeCache(game_dir), tma ? lodes_from_tma(*tma) : std::vector<Lode>{}, seed) {
    if (auto p = resolve_path(game_dir, "behpsp.res")) {
        try {
            data_.professions = load_professions(*p);
        } catch (const std::exception&) {
        }
    }
    if (auto p = resolve_path(game_dir, "BuildDat.lst")) {
        const Bytes b = read_file(*p);
        data_.build_table = parse_build_table({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    for (std::uint32_t c = 0; c < world_.clans.size(); ++c) {
        if (world_.clans[c].research.empty()) continue;
        if (auto p = resolve_path(game_dir, world_.clans[c].research)) {
            try {
                research_[c] = Research(mission::load_research_tree(*p));
            } catch (const std::exception&) {
            }
        }
    }
    auto lib = std::make_shared<AssetLibrary>(game_dir);
    lookup_ = [lib](std::string_view l, std::string_view e) -> std::span<const std::uint8_t> {
        try {
            if (const NResEntry* r = lib->find(l, e)) return r->data;
        } catch (const std::exception&) {
        }
        return {};
    };
    if (tma)
        for (auto& o : world_.objects())
            if (o.tma_index >= 0 && std::size_t(o.tma_index) < tma->objects.size()) set_object_params(o.logic_id, object_params(tma->objects[std::size_t(o.tma_index)]));
}

Economy::~Economy() {
    // Leave the hosts' controls behind (the wrappers die with the economy).
    for (auto& [lid, c] : controls_)
        if (WorldObject* o = world_.find(lid); o && o->control == c.get()) o->control = c->inner();
}

Economy::Worker& Economy::worker(std::uint32_t lid) {
    auto& w = workers_[lid];
    if (!w) {
        w = std::make_unique<Worker>();
        w->lid = lid;
        w->st.logic_id = lid;
    }
    return *w;
}

const Economy::Worker* Economy::find_worker(std::uint32_t lid) const {
    auto it = workers_.find(lid);
    return it == workers_.end() ? nullptr : it->second.get();
}

EcoState* Economy::state(std::uint32_t lid) {
    auto it = workers_.find(lid);
    return it == workers_.end() || !it->second->alive ? nullptr : &it->second->st;
}
const EcoState* Economy::state(std::uint32_t lid) const {
    auto it = workers_.find(lid);
    return it == workers_.end() || !it->second->alive ? nullptr : &it->second->st;
}

Research* Economy::research(std::uint32_t clan) {
    auto it = research_.find(clan);
    return it == research_.end() ? nullptr : &it->second;
}
const Research* Economy::research(std::uint32_t clan) const {
    auto it = research_.find(clan);
    return it == research_.end() ? nullptr : &it->second;
}
void Economy::set_research(std::uint32_t clan, Research r) { research_[clan] = std::move(r); }

Distributor* Economy::distributor(std::uint32_t clan) {
    auto it = distributors_.find(clan);
    return it == distributors_.end() ? nullptr : &it->second;
}
const Distributor* Economy::distributor(std::uint32_t clan) const {
    auto it = distributors_.find(clan);
    return it == distributors_.end() ? nullptr : &it->second;
}

void Economy::set_object_params(std::uint32_t lid, const ObjectParams& p) {
    Worker& w = worker(lid);
    w.params = p;
    if (p.current_ore) w.st.ore = *p.current_ore;
    if (p.maximum_ore) w.st.ore_max = *p.maximum_ore;
}

const std::string* Economy::building_scheme(std::uint32_t type, std::size_t level) const {
    static const std::string kTemp = kTempBuildingScheme;
    if (type == kTempBuilding) return level == 0 ? &kTemp : nullptr;
    return data_.build_table.scheme(type, level);
}

float Economy::radius(const WorldObject& o) const {
    if (radius_of) return radius_of(o);
    // [port] Bounding radii by type until the host supplies the models' spheres.
    switch (o.type) {
    case kTypePlant: return 60;
    case kTypeInstitute: case kTypeHangar: case kTypeTeleport: return 50;
    case kTypeGenerator: case kTypeMine: case kTypeStorage: return 35;
    case kTypeTowerMedium: case kTypeTowerLarge: case kTypeBunkerSmall: case kTypeBunkerMedium: case kTypeBunkerLarge: return 20;
    case kTypeBridge: return 40;
    default: return (o.type & kBuildingBit) ? 30.f : o.size_class >= 3 ? 8.f : 4.f;
    }
}

std::uint32_t Economy::level(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    return w ? w->level : 0;
}
std::uint32_t Economy::building_size(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    return w ? w->size_class : 0;
}

void Economy::init_object(WorldObject& o) {
    Worker& w = worker(o.logic_id);
    if (w.initialized) return;
    w.initialized = true;
    w.alive = o.alive;
    w.st.type = o.type;
    const Profession& prof = prof_of(*this, o.type);
    w.st.power_out = prof.power_out;
    w.st.ore_offboard = prof.ore_offboard;
    // A new object (no data.tma parameters): the capacities of BehConstants [inferred].
    if (!w.params.maximum_ore) {
        switch (o.type) {
        case kTypeMine: w.st.ore_max = data_.constants.mine_max_ore; break;
        case kTypeStorage: w.st.ore_max = data_.constants.storage_max_ore; break;
        case kTypeTransport: case kTypeBuilder: w.st.ore_max = data_.constants.transport_max_ore; break;
        default: break;
        }
    }
    if (const mission::Scheme* s = o.resource.ends_with(".dat") || o.resource.ends_with(".DAT") ? schemes_.get(o.resource) : nullptr) {
        w.size_class = o.building() ? building_size_class(*s) : robot_size(*s);
        if (lookup_) w.st.battery_cap = battery_capacity(*s, lookup_);
        if (o.building()) { // [inferred] level = the scheme's place in BuildDat.lst
            if (auto it = data_.build_table.levels.find(o.type); it != data_.build_table.levels.end())
                for (std::size_t i = 0; i < it->second.size(); ++i)
                    if (normalize_path(it->second[i]) == normalize_path(o.resource)) w.level = std::uint32_t(i);
        }
    }
    if (o.building() && w.size_class == 0) w.size_class = o.size_class;
}

void Economy::wrap_controls() {
    for (auto& o : world_.objects()) {
        if (!o.alive) continue;
        auto& c = controls_[o.logic_id];
        if (c && o.control == c.get()) continue;
        if (!c) c = std::make_unique<EconomyControl>(*this, o.logic_id, o.control);
        else c->set_inner(o.control);
        o.control = c.get();
    }
}

void Economy::join_distributor(std::uint32_t lid) {
    const WorldObject* o = world_.live(lid);
    if (!o || !o->building() || o->clan == kNoClan) return;
    if (has_superai_ && !has_superai_(o->clan)) return;
    auto it = distributors_.find(o->clan);
    if (it == distributors_.end())
        it = distributors_.emplace(o->clan, Distributor([this, clan = o->clan](std::uint32_t id) -> EcoState* {
                                      const WorldObject* x = world_.live(id);
                                      return x && x->clan == clan ? state(id) : nullptr;
                                  })).first;
    it->second.add_equal_share(lid); // MBehaviour::OnInternalInit (10005e60)
}

void Economy::leave_distributor(std::uint32_t lid, std::uint32_t clan) {
    if (Distributor* d = distributor(clan)) d->remove(lid);
}

void Economy::start(const std::function<bool(std::uint32_t)>& has_superai) {
    has_superai_ = has_superai;
    started_ = true;
    for (auto& o : world_.objects()) {
        if (!o.alive) continue;
        init_object(o);
        join_distributor(o.logic_id);
    }
    for (auto& o : world_.objects())
        if (o.alive && o.type == kTypeMine && o.clan != kNoClan) { // default order of every mine {10, 0x2000100}
            UnitOrder m;
            m.type = kOrderMine, m.param = kResourceOre, m.target_kind = kTargetNone;
            give_order(o.logic_id, m, 3);
        }
}

void Economy::on_object_added(std::uint32_t lid) {
    WorldObject* o = world_.find(lid);
    if (!o) return;
    Worker& w = worker(lid);
    if (w.initialized && w.alive) return; // already known (e.g. created by the economy itself)
    w.alive = true;
    init_object(*o);
    join_distributor(lid);
    if (o->type == kTypeMine && o->clan != kNoClan) {
        UnitOrder m;
        m.type = kOrderMine, m.param = kResourceOre, m.target_kind = kTargetNone;
        give_order(lid, m, 3);
    }
}

void Economy::on_object_removed(std::uint32_t lid) {
    auto it = workers_.find(lid);
    if (it == workers_.end()) return;
    Worker& w = *it->second;
    while (!w.tasks.empty()) {
        auto t = std::move(w.tasks.front());
        w.tasks.pop_front();
        if (t->started) t->abort(*this, w);
    }
    w.alive = false;
    w.initialized = false; // a later object with this LogicID starts afresh
    w.mover = nullptr;
    if (const WorldObject* o = world_.find(lid)) {
        leave_distributor(lid, o->clan);
        if (auto* b = brains_ ? brains_(o->clan) : nullptr) // [inferred] a dead robot frees its brain
            for (auto& owner : *b)
                if (owner == lid) owner = kNoObject;
    }
}

void Economy::on_object_captured(std::uint32_t lid, std::uint32_t old_clan) {
    // MBehaviour::Capture (10008e40): Remove from the old clan's distributor, AddEqualShare into the new.
    leave_distributor(lid, old_clan);
    join_distributor(lid);
    cancel(lid);
}

std::uint32_t* Economy::claim_brain(std::uint32_t clan, std::uint32_t owner) {
    auto* b = brains_ ? brains_(clan) : nullptr;
    if (!b) return nullptr;
    for (auto& x : *b)
        if (x == kNoObject) {
            x = owner;
            return &x;
        }
    return nullptr;
}

std::uint32_t Economy::free_brains(std::uint32_t clan) const {
    auto* b = brains_ ? brains_(clan) : nullptr;
    return b ? std::uint32_t(std::count(b->begin(), b->end(), kNoObject)) : 0;
}

std::uint32_t Economy::new_logic_id(bool building) const {
    std::uint32_t best = building ? kBuildingBit : 0;
    for (const auto& o : world_.objects()) {
        if (o.logic_id == kNoObject || o.building() != building) continue;
        best = std::max(best, o.logic_id);
    }
    return best + 1;
}

bool Economy::sphere_free(Vec3 c, float r, std::uint32_t type, std::uint32_t ignore) const {
    for (const auto& o : world_.objects()) {
        if (!o.alive || !o.building() || o.logic_id == ignore) continue;
        if (type == kTypeBridge && o.type == kTypeBridge) continue; // a bridge ignores other bridges
        if (dist3(c, o.position()) < radius(o) + r + kBuildingMargin) return false;
    }
    return true;
}

bool Economy::placement_ok(std::uint32_t type, Vec3 place) const {
    const std::string* path = building_scheme(type, 0);
    if (!path) return false;
    WorldObject probe;
    probe.type = type == kTempBuilding ? kBuildingBit : type;
    if (type == kTempBuilding)
        if (const mission::Scheme* s = const_cast<SchemeCache&>(schemes_).get(*path)) probe.type = s->flags;
    return sphere_free(place, radius(probe), probe.type, kNoObject);
}

std::uint32_t Economy::create_object(std::string_view scheme_path, Vec3 pos, float angle, std::uint32_t clan, std::uint32_t source,
                                     std::uint32_t keep_lid) {
    (void)source;
    const mission::Scheme* s = schemes_.get(scheme_path);
    if (!s) return kNoObject;
    const std::uint32_t type = s->flags;
    const bool building = (type & kBuildingBit) != 0;
    if (building) {
        WorldObject probe;
        probe.type = type;
        if (!sphere_free(pos, radius(probe), type, keep_lid)) return kNoObject;
    }
    std::uint32_t lid = keep_lid;
    if (keep_lid != kNoObject) { // the upgrade: delete + create with the same LogicID (flags 4)
        WorldObject* old = world_.find(keep_lid);
        if (!old) return kNoObject;
        const std::uint32_t old_clan = old->clan;
        on_object_removed(keep_lid);
        old->type = type;
        old->resource = std::string(scheme_path);
        old->placed = pos;
        old->clan = clan;
        old->alive = true;
        (void)old_clan;
        Worker fresh;
        fresh.lid = keep_lid, fresh.st.logic_id = keep_lid;
        *workers_[keep_lid] = std::move(fresh);
    } else {
        lid = new_logic_id(building);
        WorldObject o;
        o.logic_id = lid;
        o.clan = clan;
        o.type = type;
        o.size_class = building ? 0 : robot_size(*s);
        o.resource = std::string(scheme_path);
        o.placed = pos;
        o.armour = kPlaceholderArmour;
        o.weapon = (type & kClassRobot) ? kPlaceholderWeapon : 0.f;
        world_.add(std::move(o));
        if (!building && !claim_brain(clan, lid)) { // flag 8: a unit needs a free brain
            world_.kill(lid);
            return kNoObject;
        }
    }
    WorldObject* o = world_.find(lid);
    Worker& w = worker(lid);
    w.alive = true;
    w.angle = angle;
    init_object(*o);
    if (building) o->size_class = w.size_class;
    // Its own control wrapper (the host attaches the body's control into it later through wrap_controls).
    auto& c = controls_[lid];
    if (!c) c = std::make_unique<EconomyControl>(*this, lid, o->control);
    if (o->control != c.get()) c->set_inner(o->control), o->control = c.get();
    join_distributor(lid);
    if (type == kTypeMine && clan != kNoClan) {
        UnitOrder m;
        m.type = kOrderMine, m.param = kResourceOre, m.target_kind = kTargetNone;
        give_order(lid, m, 3);
    }
    return lid;
}

std::unique_ptr<Economy::Task> Economy::make_task(std::uint32_t, const UnitOrder& o) {
    switch (o.type) {
    case kOrderMine: return std::make_unique<MineTask>(o);
    case kOrderConstruct: return std::make_unique<ConstructTask>(o);
    case kOrderResearch:
    case kOrderResearchScheme: return std::make_unique<ResearchTask>(o);
    case kOrderShowUpgrade: return std::make_unique<ShowUpgradeTask>(o);
    case kOrderBuild: return std::make_unique<BuildTask>(o);
    case kOrderUpgrade: return std::make_unique<UpgradeTask>(o);
    case kOrderTransport: return std::make_unique<TransportTask>(o);
    default: return nullptr;
    }
}

bool Economy::give_order(std::uint32_t lid, const UnitOrder& order, int mode) {
    const WorldObject* o = world_.live(lid);
    if (!o || !is_economy_order(order.type)) return false;
    Worker& w = worker(lid);
    if (!w.initialized) init_object(*world_.find(lid));
    auto t = make_task(lid, order);
    if (!t || !t->set_target(*this, w)) return false;
    // [port] A building in its show keeps it: orders that would replace it go after it.
    if (mode != 1 && order.type != kOrderShowUpgrade && showing(lid)) mode = 0;
    if (mode == 3) {
        while (!w.tasks.empty()) {
            auto old = std::move(w.tasks.front());
            w.tasks.pop_front();
            if (old->started) old->abort(*this, w);
        }
        w.tasks.push_back(std::move(t));
    } else if (mode == 2) {
        w.tasks.push_front(std::move(t));
    } else if (mode == 0 && !w.tasks.empty()) {
        w.tasks.insert(w.tasks.begin() + 1, std::move(t));
    } else {
        w.tasks.push_back(std::move(t));
    }
    if (mode != 1) w.timer.reset();
    if (o->building() && w.tasks.size() == 1) w.timer.reset();
    return true;
}

void Economy::cancel(std::uint32_t lid) {
    auto it = workers_.find(lid);
    if (it == workers_.end()) return;
    Worker& w = *it->second;
    while (!w.tasks.empty()) {
        auto t = std::move(w.tasks.front());
        w.tasks.pop_front();
        if (t->started) t->abort(*this, w);
    }
}

std::uint32_t Economy::current_order(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    return w && !w->tasks.empty() ? w->tasks.front()->order.type : 0;
}

std::int32_t Economy::show_animation(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    return w ? w->animation : 0;
}

int Economy::footprint(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    return w ? w->footprint : 1;
}

bool Economy::showing(std::uint32_t lid) const { return current_order(lid) == kOrderShowUpgrade; }

void Economy::set_mover(std::uint32_t lid, Mover* m) { worker(lid).mover = m; }

bool Economy::walk(std::uint32_t lid, Vec3 target) {
    Worker& w = worker(lid);
    if (w.mover) return w.mover->go_to(target);
    const WorldObject* o = world_.find(lid);
    if (!o) return false;
    w.walk_until = now_ + std::uint32_t(dist2(o->position(), target) / kFallbackSpeed * 1000.f);
    return true;
}

bool Economy::walking_done(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    if (!w) return true;
    return w->mover ? w->mover->idle() : now_ >= w->walk_until;
}

void Economy::emit(EconomyEvent e) {
    log_.push_back(e);
    if (sink_) sink_(log_.back());
}

void Economy::run_tasks(Worker& w) {
    // MTaskStack::Takt: the current task's takt; a finished one is popped and the next started.
    if (!w.tasks.empty() && w.tasks.front()->started) {
        Task& cur = *w.tasks.front();
        const float dt = float(now_ - cur.last) * 0.001f;
        cur.last = now_;
        if (cur.takt(*this, w, dt)) return;
        auto done = std::move(w.tasks.front());
        w.tasks.pop_front();
        EconomyEvent ev = make_event(done->fail ? EconomyEvent::Type::TaskFailed : EconomyEvent::Type::TaskComplete, w.lid);
        ev.code = done->order.type;
        ev.item = std::int32_t(done->fail);
        if (const WorldObject* o = world_.find(w.lid)) ev.clan = o->clan;
        if (done->order.type != kOrderShowUpgrade) emit(ev);
    }
    while (!w.tasks.empty() && !w.tasks.front()->started) {
        Task& t = *w.tasks.front();
        if (t.start(*this, w)) {
            t.started = true;
            t.last = now_;
            return;
        }
        auto dropped = std::move(w.tasks.front());
        w.tasks.pop_front(); // "Cannot StartTask"
        EconomyEvent ev = make_event(EconomyEvent::Type::TaskFailed, w.lid);
        ev.code = dropped->order.type;
        ev.item = std::int32_t(dropped->fail);
        if (const WorldObject* o = world_.find(w.lid)) ev.clan = o->clan;
        emit(ev);
    }
}

void Economy::update(std::uint32_t now_ms) {
    const float dt = started_ && now_ms > last_update_ ? float(now_ms - last_update_) * 0.001f : 0.f;
    now_ = last_update_ = now_ms;
    // [port approximation] the batteries drain by the power the building's devices use.
    for (auto& [lid, w] : workers_)
        if (w->alive && w->st.power_usage > 0 && w->st.battery_cap > 0)
            w->st.battery_frac = std::max(0.f, w->st.battery_frac - w->st.power_usage * dt / w->st.battery_cap);
    // CSuperAI::Takt calls distributor slot 11 every AI frame.
    for (auto& [clan, d] : distributors_) d.takt(now_ms, rand_);
    // Collect first: tasks may create objects (and workers).
    std::vector<Worker*> ws;
    for (auto& [lid, w] : workers_)
        if (w->alive && !w->tasks.empty()) ws.push_back(w.get());
    for (Worker* w : ws) {
        if (!world_.live(w->lid)) continue;
        if (!w->timer.check_and_rearm(now_ms, rand_)) continue;
        run_tasks(*w);
        if (w->tasks.empty())
            if (auto it = controls_.find(w->lid); it != controls_.end() && it->second->inner_) {
                auto pending = std::move(it->second->pending_);
                it->second->pending_.clear();
                for (auto& [o, m] : pending) it->second->inner_->give_order(o, m);
            }
    }
}

// --- HQ / play API ---

bool Economy::order_construct(std::uint32_t plant, std::string_view scheme_path, int mode) {
    const mission::Scheme* s = schemes_.get(scheme_path);
    const WorldObject* o = world_.live(plant);
    if (!s || !o || free_brains(o->clan) == 0) return false; // sent only when CountFreeBrains > 0 (10098680)
    UnitOrder ord;
    ord.type = kOrderConstruct, ord.param = s->flags, ord.target_kind = kTargetName, ord.design = std::string(scheme_path);
    return o->control ? o->control->give_order(ord, mode) : give_order(plant, ord, mode);
}

std::vector<std::int32_t> Economy::research_orders(std::uint32_t clan) const {
    std::vector<std::int32_t> out;
    for (const auto& [lid, w] : workers_) {
        const WorldObject* o = world_.live(lid);
        if (!o || o->clan != clan || o->type != kTypeInstitute) continue;
        for (const auto& t : w->tasks)
            if (t->order.type == kOrderResearch) out.push_back(std::int32_t(t->order.param));
    }
    return out;
}

bool Economy::order_research(std::uint32_t clan, std::int32_t item) {
    const Research* r = research(clan);
    if (!r || !r->valid(item) || (r->state(item) & kResResearched)) return false;
    const auto queued = research_orders(clan);
    if (std::find(queued.begin(), queued.end(), item) != queued.end()) return false;
    std::uint32_t best = kNoObject;
    std::size_t best_n = 0;
    for (const auto& o : world_.objects()) {
        if (!o.alive || o.clan != clan || o.type != kTypeInstitute || showing(o.logic_id)) continue;
        const Worker* w = find_worker(o.logic_id);
        const std::size_t n = w ? w->tasks.size() : 0;
        if (best == kNoObject || n < best_n) best = o.logic_id, best_n = n;
    }
    if (best == kNoObject) return false;
    UnitOrder ord;
    ord.type = kOrderResearch, ord.param = std::uint32_t(item), ord.target_kind = kTargetNone;
    return give_order(best, ord, 1);
}

bool Economy::order_build(std::uint32_t builder, std::uint32_t building_type, Vec3 place, float angle, int mode) {
    UnitOrder ord;
    ord.type = kOrderBuild, ord.param = building_type, ord.target_kind = kTargetMatrix, ord.place = place, ord.angle = angle;
    const WorldObject* o = world_.live(builder);
    if (!o) return false;
    return o->control ? o->control->give_order(ord, mode) : give_order(builder, ord, mode);
}

bool Economy::can_upgrade(std::uint32_t builder, std::uint32_t building) const {
    const WorldObject* u = world_.live(builder);
    const WorldObject* b = world_.live(building);
    if (!u || !b || !b->building() || b->clan != u->clan || showing(building)) return false;
    return level(building) + 1 < data_.build_table.level_count(b->type);
}

bool Economy::order_upgrade(std::uint32_t builder, std::uint32_t building) {
    UnitOrder ord;
    ord.type = kOrderUpgrade, ord.target_kind = kTargetLogicId, ord.target_id = building;
    const WorldObject* o = world_.live(builder);
    if (!o) return false;
    return o->control ? o->control->give_order(ord, 3) : give_order(builder, ord, 3);
}

bool Economy::order_transport(std::uint32_t transport) {
    UnitOrder ord;
    ord.type = kOrderTransport, ord.param = kNoObject, ord.target_kind = kTargetNone;
    const WorldObject* o = world_.live(transport);
    if (!o) return false;
    return o->control ? o->control->give_order(ord, 3) : give_order(transport, ord, 3);
}

void Economy::set_share(std::uint32_t clan, std::uint32_t lid, float share, bool lock) {
    if (Distributor* d = distributor(clan)) d->set_share(lid, share, lock ? 1 : 0);
}

std::optional<PlantInfo> Economy::plant(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    const WorldObject* o = world_.live(lid);
    if (!w || !o || o->type != kTypePlant) return std::nullopt;
    PlantInfo i;
    i.free_bots = w->params.free_bots;
    i.size_class = w->size_class;
    if (!w->tasks.empty()) i.queued = w->tasks.size() - 1;
    if (!w->tasks.empty() && w->tasks.front()->order.type == kOrderConstruct) {
        const auto& t = static_cast<const ConstructTask&>(*w->tasks.front());
        i.busy = true;
        i.scheme = t.order.design;
        i.progress = t.progress;
        i.ore_cost = t.ore, i.ore_got = t.ore_got, i.power_cost = t.power, i.power_got = t.power_got;
        i.time_cost = t.time, i.elapsed = t.elapsed;
    }
    return i;
}

std::optional<InstituteInfo> Economy::institute(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    const WorldObject* o = world_.live(lid);
    if (!w || !o || o->type != kTypeInstitute) return std::nullopt;
    InstituteInfo i;
    i.free_techno = w->params.free_techno;
    i.research_time = w->params.free_research_time;
    if (!w->tasks.empty()) i.queued = w->tasks.size() - 1;
    if (!w->tasks.empty() && (w->tasks.front()->order.type == kOrderResearch || w->tasks.front()->order.type == kOrderResearchScheme)) {
        const auto& t = static_cast<const ResearchTask&>(*w->tasks.front());
        i.busy = true;
        i.item = t.current;
        i.list = t.list;
        if (const Research* r = research(o->clan); r && t.current >= 0) i.item_progress = r->progress(t.current);
    }
    return i;
}

std::optional<BuilderInfo> Economy::builder(std::uint32_t lid) const {
    const Worker* w = find_worker(lid);
    if (!w || !world_.live(lid)) return std::nullopt;
    BuilderInfo i;
    i.ore = w->st.ore;
    if (!w->tasks.empty()) {
        i.order = w->tasks.front()->order.type;
        w->tasks.front()->fill(i);
    }
    return i;
}

float Economy::minerals_percent(std::uint32_t clan) const {
    const Distributor* d = distributor(clan);
    return d ? std::clamp(d->summary().ore_in_buffer / 4500.f, 0.f, 1.f) * 100.f : 0.f;
}

float Economy::energy_percent(std::uint32_t clan) const {
    const Distributor* d = distributor(clan);
    if (!d) return 0;
    float all = 0;
    for (const auto& [c, x] : distributors_) all += x.summary().power_available;
    if (all <= 0) return 0;
    return std::clamp((d->summary().power_available - d->summary().power_usage) / all, 0.f, 1.f) * 100.f;
}

} // namespace parkan::economy
