#include "game/super_ai.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace parkan::game {

namespace {

constexpr std::uint32_t kPbmBuildingCapture = 9, kPbmBaseDefence = 12; // varset.var PBM_*
constexpr std::uint32_t kTargetId = 0x201, kTargetPlace = 0x202, kTargetType = 0x203, kTargetName = 0x205;
constexpr std::uint32_t kOrderRepair = 9;

// x87 __ftol into a 32-bit cell: the low dword of the 64-bit truncation.
std::uint32_t ftol(double x) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(x)); }
std::uint32_t fbits(float f) { return std::bit_cast<std::uint32_t>(f); }
float dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); } // 1000fc30

bool small_warbot(const WorldObject& o) { return o.type == kTypeWarbot && (o.size_class == 1 || o.size_class == 2); }

// h35's class rank: generator 1, plant 2, mine 3, institute 4, storage 5, anything else 10.
int capture_rank(std::uint32_t type) {
    switch (type) {
    case kTypeGenerator: return 1;
    case kTypePlant: return 2;
    case kTypeMine: return 3;
    case kTypeInstitute: return 4;
    case kTypeStorage: return 5;
    default: return 10;
    }
}

} // namespace

SuperAI::SuperAI(World& world, std::vector<SuperAI*>& registry, std::uint32_t clan, ai::ScriptProgram program,
                 HostCallback callback, RandomSource rand)
    : world_(world), registry_(registry), clan_(clan), vm_(std::move(program)), callback_(std::move(callback)),
      rand_(std::move(rand)) {
    if (registry_.size() < kMaxClans) registry_.resize(kMaxClans, nullptr);
    if (clan_ < world_.clans.size()) {
        base_x_ = world_.clans[clan_].base_x;
        base_y_ = world_.clans[clan_].base_y;
        const auto& rels = world_.clans[clan_].relations;
        for (std::size_t c = 0; c < rels.size() && c < kMaxClans; ++c) {
            rel_[c].value = rels[c];
            rel_[c].attitude = rels[c] == 2 ? std::bit_cast<float>(0x3f555326u)
                               : rels[c] == 1 ? std::bit_cast<float>(0x3efff972u)
                                              : std::bit_cast<float>(0x3e2aa64cu);
        }
    }
    // Ctor 10001000: bind the C++-controlled variables and look the fixed events up by name.
    var_current_problem_ = vm_.var_index("dCurrentProblem");
    var_current_sender_ = vm_.var_index("dCurrentSender");
    const auto ev = [&](std::string_view name) -> std::int64_t {
        auto i = vm_.program().find_event(name);
        return i ? std::int64_t(*i) : -1;
    };
    ev_mission_ = ev("Mission");
    ev_problems_ = ev("Problems0"); // "Problems" + DAT_10038c06
    ev_mech_ = ev("Mech_GeneratorFound");
    ev_fort_ = ev("Fort_Task_Complete");
    if (clan_ < kMaxClans) registry_[clan_] = this;
}

SuperAI::~SuperAI() {
    if (clan_ < registry_.size() && registry_[clan_] == this) registry_[clan_] = nullptr; // slot 1 (10001740)
}

void SuperAI::run_event(std::int64_t event) {
    if (event < 0 || running_) return; // +0x78 guards against re-entry
    running_ = true;
    vm_.run_event(std::uint32_t(event), *this);
    running_ = false;
}

void SuperAI::start(std::uint32_t mode) {
    mode_ = mode;
    if (!init_done_) {
        if (auto init = vm_.program().find_event("Init")) {
            run_event(*init);
            init_done_ = true;
        }
    }
}

void SuperAI::run_mission_event() { run_event(ev_mission_); }

std::uint32_t SuperAI::current_problem() const {
    return var_current_problem_ ? const_cast<ai::ScriptVM&>(vm_).var(*var_current_problem_).get_dword() : 0;
}

void SuperAI::takt(std::uint32_t now_ms) {
    now_ = now_ms;
    if (mode_ == 0 || next_problems_ >= std::int64_t(now_ms)) return;
    next_problems_ = std::int64_t(now_ms) + 7000 + rand_() % 1000;
    clock_ += 7;
    update_problems(); // 10005910
    // 10001e50: _Continue for every SOLVING problem.
    for (std::size_t i = 0; i < problems_.size(); ++i) {
        const Problem& p = problems_[i];
        if (p.type == 0 || p.state != kStSolving) continue;
        if (var_current_problem_) vm_.var(*var_current_problem_).set_dword(std::uint32_t(i));
        if (auto it = problem_events_.find(p.type); it != problem_events_.end()) run_event(it->second.second);
    }
    run_event(ev_problems_);
    // 10001bf0: _Start for the top-priority problem that is neither SOLVING nor SOLVED.
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < problems_.size(); ++i) {
        const Problem& p = problems_[i];
        if (p.type == 0 || p.state == kStSolving || p.state == kStSolved) continue;
        if (!best || p.priority > problems_[*best].priority) best = i;
    }
    if (best) {
        if (var_current_problem_) vm_.var(*var_current_problem_).set_dword(std::uint32_t(*best));
        if (auto it = problem_events_.find(problems_[*best].type); it != problem_events_.end()) run_event(it->second.first);
    }
}

void SuperAI::update_problems() {
    for (std::size_t i = 0; i < problems_.size(); ++i) {
        Problem& p = problems_[i];
        if (p.type == 0) continue;
        // 10004ee0: age, reset stale states, then the completion expressions.
        p.lifetime -= p.decay;
        if (p.lifetime <= 0) {
            release_problem_units(p);
            p.type = 0;
            continue;
        }
        if (p.decay > 0 && ((p.state == kStSolving && p.units.empty()) || (p.state == kStNone && !p.units.empty())))
            p.state = kStUnsolved;
        if (p.exprs.empty()) continue;
        bool any = false, all = true;
        for (auto& x : p.exprs) {
            x.done = x.done || expr_done(p, x);
            any = any || x.done;
            all = all && x.done;
        }
        if (p.and_mode ? all : any) set_problem_state(std::uint32_t(i), kStSolved);
    }
}

bool SuperAI::expr_done(const Problem& p, const ProblemExpr& x) const {
    const auto unit_done = [&](std::uint32_t id, std::uint32_t action) {
        const WorldObject* o = world_.live(id);
        switch (action) {
        case 1: case 4: return o == nullptr;                      // ACTION_DESTROY / GROUP_DESTROY
        case 2: case 5: return !o || o->current_order() == 0;     // (GROUP_)NOTHING_DOING
        case 3: return o && o->clan == clan_;                     // CAPTURE_BUILDING
        default: return false;
        }
    };
    if (x.kind == 0) return unit_done(x.id, x.action);
    if (x.kind == 1) {
        if (x.id >= p.groups.size() || p.groups[x.id].index < 0) return true; // the group is gone
        for (std::uint32_t u : p.groups[x.id].units)
            if (u < units_.size() && !unit_done(units_[u].logic_id, x.action)) return false;
        return true;
    }
    return false;
}

void SuperAI::on_object_added(std::uint32_t kind, std::uint32_t logic_id, bool before_start) {
    if (mode_ == 0 && !before_start) return;
    const WorldObject* o = world_.live(logic_id);
    if (!o || unit_index(logic_id)) return;
    if (kind == 1 || kind == 2) {
        units_.push_back({logic_id, o->type, false, kUnitFree});
        if (kind == 2) {
            switch (o->type) {
            case kTypeMine: ++built_[0]; break;
            case kTypeStorage: ++built_[1]; break;
            case kTypePlant: ++built_[2]; break;
            case kTypeHangar: ++built_[3]; break;
            case kTypeTowerMedium: ++built_[4]; break;
            case kTypeTowerLarge: ++built_[5]; break;
            case kTypeInstitute: ++built_[6]; break;
            default: break;
            }
            grow_base_radius();
        }
    }
    next_problems_ -= 1400; // the next Problems run comes 1.4 s sooner
}

void SuperAI::on_object_removed(std::uint32_t logic_id) {
    if (auto i = unit_index(logic_id)) {
        release_unit(std::uint32_t(*i), false);
        units_[*i].logic_id = kNoObject;
    }
}

void SuperAI::notify_object_event(std::uint32_t sender, std::uint32_t event) {
    if (var_current_sender_) vm_.var(*var_current_sender_).set_dword(sender);
    if (running_) return;
    std::int64_t ev = -1;
    if (sender & kBuildingBit) {
        if (event == 3) ev = ev_fort_;
        else if (event == 7 && ev_fort_ >= 0) ev = ev_fort_ + 1;
    } else if (ev_mech_ >= 0) {
        if (event == 1) ev = ev_mech_;
        else if (event == 2) ev = ev_mech_ + 1;
        else if (event == 3) ev = ev_mech_ + 2;
        else if (event == 8) ev = ev_mech_ + 3;
    }
    run_event(ev);
}

void SuperAI::set_relation(std::uint32_t clan, std::uint32_t value) {
    if (clan >= kMaxClans) return;
    if (value == 2) rel_[clan].attitude = std::bit_cast<float>(0x3f555326u);
    else if (value == 1) rel_[clan].attitude = std::bit_cast<float>(0x3efff972u);
    else if (value == 0) rel_[clan].attitude = std::bit_cast<float>(0x3e2aa64cu);
    rel_[clan].value = value;
    if (SuperAI* other = registry_[clan]; other && clan_ < kMaxClans) {
        other->rel_[clan_].value = value;
        other->rel_[clan_].attitude = rel_[clan].attitude;
    }
}

std::uint32_t SuperAI::relation(std::uint32_t clan) const { return clan < kMaxClans ? rel_[clan].value : kNoObject; }

void SuperAI::add_hostility(std::uint32_t clan) {
    if (clan < kMaxClans) rel_[clan].hostility += 0.004f; // _DAT_10034284
}

bool SuperAI::remove_brain(std::size_t i) {
    if (i >= brains_.size()) return false;
    brains_.erase(brains_.begin() + std::ptrdiff_t(i));
    return true;
}

std::optional<std::size_t> SuperAI::unit_index(std::uint32_t logic_id) const { // 10005c10
    if (logic_id == kNoObject) return std::nullopt;
    for (std::size_t i = 0; i < units_.size(); ++i)
        if (units_[i].logic_id == logic_id) return i;
    return std::nullopt;
}

Problem* SuperAI::problem(std::uint32_t index) { return index < problems_.size() ? &problems_[index] : nullptr; }

bool SuperAI::hostile(std::uint32_t clan) const { return clan < kMaxClans && clan != clan_ && rel_[clan].value == 0; }

float SuperAI::unit_hits(std::uint32_t logic_id) const { // 100065e0
    const WorldObject* o = world_.live(logic_id);
    return o ? combat_hits(o->armour, o->weapon) : 0.f;
}

// 10006130: the units of every areal within the 2-D radius; clan -1 = every hostile clan.
float SuperAI::hits_around(float x, float y, float radius, std::uint32_t clan) const {
    float sum = 0;
    for (const auto& o : world_.objects()) {
        if (!o.alive || o.building()) continue;
        if (clan == kNoObject ? !hostile(o.clan) : o.clan != clan) continue;
        const Vec3 p = o.position();
        if (dist2d(p.x, p.y, x, y) <= radius) sum += combat_hits(o.armour, o.weapon);
    }
    return sum;
}

std::optional<Vec3> SuperAI::object_pos(std::uint32_t logic_id) const { // 100062d0
    const WorldObject* o = world_.live(logic_id);
    return o ? std::optional<Vec3>(o->position()) : std::nullopt;
}

bool SuperAI::lower_priority(const UnitRec& u, float than) const {
    return u.problem < problems_.size() && problems_[u.problem].type != 0 && problems_[u.problem].priority < than;
}

// 10004010 (+ 10003e80, the repair order of a released unit when the clan runs in mode 2).
void SuperAI::release_unit(std::uint32_t unit, bool repair_order) {
    if (unit >= units_.size()) return;
    UnitRec& u = units_[unit];
    if (u.problem < problems_.size()) {
        Problem& p = problems_[u.problem];
        p.units.erase(std::remove(p.units.begin(), p.units.end(), unit), p.units.end());
        if (u.critical) p.critical_lost = true;
    }
    u.problem = kUnitFree;
    if (repair_order && mode_ == 2 && u.logic_id != kNoObject) {
        UnitOrder o;
        o.type = kOrderRepair;
        o.target_kind = 0x204;
        o.limits[7] = 0, o.limits[8] = 1.f, o.limits[9] = 0.2f, o.limits[10] = 0.5f;
        give(u.logic_id, o, 3);
    }
}

void SuperAI::assign_unit(std::uint32_t unit, std::uint32_t index) { // 10005080
    if (unit >= units_.size() || index >= problems_.size()) return;
    release_unit(unit, false);
    problems_[index].units.push_back(unit);
    units_[unit].problem = index;
}

void SuperAI::release_problem_units(Problem& p) { // 100050f0
    const auto list = p.units;
    for (std::uint32_t u : list) release_unit(u, true);
    p.units.clear();
    p.groups.clear();
}

void SuperAI::set_problem_state(std::uint32_t index, std::uint32_t state) { // 10005710
    Problem* p = problem(index);
    if (!p) return;
    if (state == kStSolved || state == kStUnsolved) {
        release_problem_units(*p);
        p->type = p->c = p->d = 0;
        if (state == kStSolved) p->e = 0;
        p->exprs.clear();
    }
    p->state = state;
}

ProblemGroup* SuperAI::group(Problem& p, std::int32_t g) {
    return g >= 0 && std::size_t(g) < p.groups.size() && p.groups[std::size_t(g)].index >= 0 ? &p.groups[std::size_t(g)] : nullptr;
}

std::int32_t SuperAI::add_group(Problem& p) { // 10005540
    p.groups.push_back({std::int32_t(p.groups.size()), {}});
    return p.groups.back().index;
}

void SuperAI::release_group(Problem& p, std::int32_t g) { // 100055b0
    ProblemGroup* gr = group(p, g);
    if (!gr) return;
    for (std::uint32_t u : gr->units)
        if (u < units_.size() && units_[u].problem < problems_.size() && &problems_[units_[u].problem] == &p) release_unit(u, true);
    gr->index = -1;
    gr->units.clear();
}

bool SuperAI::give(std::uint32_t logic_id, const UnitOrder& order, int mode) {
    const WorldObject* o = world_.live(logic_id);
    const bool ok = o && o->control && o->control->give_order(order, mode);
    ok ? ++orders_given : ++orders_refused;
    return ok;
}

// 10006700: the base radius grows to the farthest own building within maxRadius + 100, capped.
void SuperAI::grow_base_radius() {
    for (const auto& u : units_) {
        if (!(u.logic_id & kBuildingBit) || u.logic_id == kNoObject) continue;
        const WorldObject* o = world_.live(u.logic_id);
        if (!o || o->clan != clan_) continue;
        const Vec3 p = o->position();
        const float d = dist2d(p.x, p.y, base_x_, base_y_);
        if (d < max_base_radius_ + 100.f) base_radius_ = std::max(base_radius_, std::min(d, max_base_radius_));
    }
}

void SuperAI::reserve_spot(float x, float y) { // 1000fb60: a 20-slot table, oldest replaced
    auto& spots = world_.reserved_spots;
    if (spots.size() >= 20) spots.erase(spots.begin());
    spots.push_back({x, y, now_});
}

// 100068b0: inside the base radius; not a teleport, ruins, generator, hangar, bridge or bunker; not showing
// (property 0x20c); a clan; level + 1 < the type's level count; the next level's scheme fully researched.
bool SuperAI::upgradable(const UnitRec& u) const {
    if (!economy_ || u.logic_id == kNoObject || !(u.logic_id & kBuildingBit)) return false;
    const WorldObject* o = world_.live(u.logic_id);
    if (!o || o->clan == kNoClan || o->clan != clan_) return false;
    switch (o->type) {
    case kTypeTeleport: case kTypeRuins: case kTypeGenerator: case kTypeHangar: case kTypeBridge:
    case kTypeBunkerSmall: case kTypeBunkerMedium: case kTypeBunkerLarge: return false;
    default: break;
    }
    const Vec3 p = o->position();
    if (!(dist2d(p.x, p.y, base_x_, base_y_) < base_radius_)) return false;
    if (economy_->showing(u.logic_id)) return false;
    const std::uint32_t lvl = economy_->level(u.logic_id);
    if (!(lvl + 1 < economy_->data().build_table.level_count(o->type))) return false;
    const std::string* next = economy_->building_scheme(o->type, lvl + 1);
    const economy::Research* r = economy_->research(clan_);
    const mission::Scheme* s = next ? economy_->schemes().get(*next) : nullptr;
    return s && r && r->fully_researched(*s);
}

std::uint32_t SuperAI::call_handler(std::uint32_t index, ai::ScriptVM& vm, const ai::Record& rec) {
    if (index < handler_calls.size()) ++handler_calls[index];
    const auto ref = [&](std::size_t i) -> ai::ScriptValue& { return vm.var(rec.refs.at(i)); };
    const auto u32 = [&](std::size_t i) { return ref(i).get_dword(); };
    const auto f32 = [&](std::size_t i) { return ref(i).get_float(); };
    const auto out = [&](std::size_t i, std::uint32_t v) { ref(i).set_dword(v); }; // ScriptVar::SetNumber
    const auto raw = [&](std::size_t i) -> std::uint32_t {
        const auto& v = ref(i);
        return v.type == ai::VarType::Dword ? v.u : v.type == ai::VarType::Float ? fbits(v.f) : 0;
    };
    const auto neutral = [&](std::uint32_t r) {
        ++neutral_calls[index];
        return r;
    };
    const auto var_u32 = [&](std::string_view name) -> std::uint32_t {
        auto i = vm.var_index(name);
        return i ? vm.var(*i).get_dword() : 0;
    };
    const std::uint32_t cur = current_problem();
    Problem* cp = problem(cur);
    const float cur_prio = cp ? cp->priority : 0.f;
    const auto pos = [&](const UnitRec& u) { return object_pos(u.logic_id); };
    const auto take_into_group = [&](Problem& p, std::int32_t g, std::uint32_t unit) { // 100044e0 + 10005080
        if (ProblemGroup* gr = group(p, g)) gr->units.push_back(unit);
        assign_unit(unit, cur);
    };
    // h15/h28 order block (Order init 10004330; limits[7..10] from op4..op7).
    const auto make_order = [&](UnitOrder& o, bool by_name_ok) -> bool {
        o.type = u32(1);
        o.param = u32(3);
        for (int k = 0; k < 4; ++k) o.limits[std::size_t(7 + k)] = f32(std::size_t(4 + k));
        o.target_kind = u32(8);
        switch (o.target_kind) {
        case kTargetId: o.target_id = u32(9); break;
        case kTargetPlace: o.place = {float(u32(9)), float(u32(10)), 0}; break;
        case kTargetType: o.mask = u32(9); break;
        case kTargetName: { // 10010be0 / 100107c0: the design table's pick for robot type op3, mode op9
            if (!economy_) return by_name_ok; // no design table: h28 gives the order with an empty target
            if (!planner_.loaded) return false;
            const auto pick = planner_.select_best(o.param, u32(9), std::uint32_t(rand_()) + now_);
            if (!pick) return false;
            o.design = planner_.entries[*pick].file;
            return true;
        }
        default: break;
        }
        return true;
    };
    const bool orders_allowed = mode_ == 2 || clan_ == 0; // CSuperAI+0x3fc == 2 or clan 0

    switch (index) {
    case 0: // Nop
    case 1: // NopFloat
        return 1;
    case 2: { // AddProblem(typeVar, prio, lifetime, decay, c, d, e) — mgr 100059f0
        const std::uint32_t type = u32(0);
        const std::string& name = vm.program().vars.at(rec.refs.at(0)).name;
        const float prio = f32(1);
        const auto lifetime = std::int32_t(u32(2)), decay = std::int32_t(u32(3));
        const std::uint32_t c = raw(4), d = raw(5), e = raw(6);
        auto it = problem_events_.find(type);
        if (it == problem_events_.end()) {
            const auto s = vm.program().find_event(name + "_Start"), k = vm.program().find_event(name + "_Continue");
            it = problem_events_.emplace(type, std::pair{s ? std::int64_t(*s) : -1, k ? std::int64_t(*k) : -1}).first;
        }
        if (it->second.first < 0 || it->second.second < 0) return 0;
        for (auto& p : problems_) // 10004c50
            if (p.type == type && p.c == c && p.d == d) {
                if (prio >= p.priority) {
                    p.priority = prio;
                    p.e = e;
                    p.lifetime += p.refill;
                }
                return 0;
            }
        Problem np;
        np.type = type, np.c = c, np.d = d, np.e = e, np.priority = prio;
        np.lifetime = np.refill = lifetime;
        np.decay = decay;
        auto slot = std::find_if(problems_.begin(), problems_.end(), [](const Problem& p) { return p.type == 0; });
        if (slot != problems_.end()) *slot = std::move(np);
        else problems_.push_back(std::move(np));
        return 0;
    }
    case 3: { // GetEnergySurplus: fPowerAvailable − fPowerUsage of the last distributor step (0 before)
        if (!economy_) return neutral(fbits(0.f));
        const economy::Distributor* d = economy_->distributor(clan_);
        if (!d || !d->stepped()) return fbits(0.f);
        return fbits(d->summary().power_available - d->summary().power_usage);
    }
    case 4: // ReleaseProblemUnits(problem)
        if (Problem* p = problem(u32(0))) release_problem_units(*p);
        return 0;
    case 5: set_problem_state(u32(0), u32(1)); return 0; // SetProblemState
    case 6: { // TakeUnit(logicId, critical)
        const std::uint32_t id = u32(0), critical = u32(1);
        if (auto i = unit_index(id); i && cp) {
            assign_unit(std::uint32_t(*i), cur);
            units_[*i].critical = critical != 0;
        }
        return 0;
    }
    case 7: // ReleaseCurrentProblemUnits
        if (cp) release_problem_units(*cp);
        return 0;
    case 8: set_problem_state(cur, u32(0)); return 0; // SetCurrentProblemState
    case 9: return 0;                                  // Nop2
    case 10: // FindForeignUnit
        for (const auto& o : world_.objects())
            if (o.alive && !o.building() && o.clan != clan_) return o.logic_id;
        return kNoObject;
    case 11: { // IsUnitLimitReached(type) — 100060e0 + the OnObjectAdded counters
        const std::uint32_t type = u32(0);
        std::uint32_t n = 0;
        for (const auto& u : units_)
            if (u.logic_id != kNoObject && u.type == type && world_.live(u.logic_id)) ++n;
        switch (type) {
        case kTypePlant: return n >= var_u32("dMaxPlant") || built_[2] >= 1;
        case kTypeMine: return n >= var_u32("dMaxMine") || built_[0] >= 1;
        case kTypeStorage: return n >= var_u32("dMaxStorage") || built_[1] >= 1;
        case kTypeTowerMedium: return n >= var_u32("dMaxTower") || built_[4] > 2;
        case kTypeTowerLarge: return n >= var_u32("dMaxTower") || built_[5] >= 1;
        case kTypeTransport: return n >= var_u32("dMaxTransport");
        case kTypeBuilder: return n >= var_u32("dMaxBuilder");
        case kTypeHangar: return built_[3] >= 1;
        case kTypeInstitute: return built_[6] >= 1;
        default: return 0;
        }
    }
    case 12: return fbits(cur_prio); // GetCurrentProblemPriority
    case 13:   // GetFreeUnit(mode, type)
    case 14: { // GetUnitForProblem(mode, ...)
        const std::uint32_t mode = u32(0);
        const auto free_of = [&](auto pred) -> std::uint32_t {
            for (const auto& u : units_)
                if (u.problem == kUnitFree && u.logic_id != kNoObject)
                    if (const WorldObject* o = world_.live(u.logic_id); o && pred(*o)) return u.logic_id;
            return kNoObject;
        };
        const auto lower_of = [&](auto pred) -> std::uint32_t {
            for (const auto& u : units_)
                if (u.problem != kUnitReserved && u.problem != cur && lower_priority(u, cur_prio))
                    if (const WorldObject* o = world_.live(u.logic_id); o && pred(*o)) return u.logic_id;
            return kNoObject;
        };
        if (mode == 2) { // UNIT_ANY_UNIT
            const std::uint32_t type = u32(1);
            const auto of_type = [&](const WorldObject& o) { return o.type == type; };
            const std::uint32_t id = free_of(of_type);
            return id != kNoObject ? id : lower_of(of_type);
        }
        if (index == 13) {
            if (mode == 0) {
                const std::uint32_t type = u32(1);
                return free_of([&](const WorldObject& o) { return o.type == type; });
            }
            return mode == 1 ? free_of(small_warbot) : kNoObject;
        }
        if (mode == 3) { // UNIT_ANY_CAPTURER
            const std::uint32_t id = free_of(small_warbot);
            return id != kNoObject ? id : lower_of(small_warbot);
        }
        if (mode != 4) return kNoObject;
        // UNIT_ANY_NEAREST_CAPTURER(targetKind, target...): least time-to-target = dist / speed.
        float tx = 0, ty = 0;
        if (u32(1) == kTargetPlace) {
            tx = f32(2), ty = f32(3);
        } else {
            auto p = object_pos(u32(2));
            if (!p) return kNoObject;
            tx = p->x, ty = p->y;
        }
        std::uint32_t best = kNoObject;
        float best_t = 0;
        for (const auto& u : units_) {
            if (u.problem != kUnitFree && (u.problem == kUnitReserved || u.problem == cur || !lower_priority(u, cur_prio))) continue;
            const WorldObject* o = world_.live(u.logic_id);
            if (!o || !small_warbot(*o)) continue;
            const float speed = o->control ? o->control->speed() : 0.f;
            if (speed <= 1.f) continue;
            const Vec3 p = o->position();
            const float d = dist2d(p.x, p.y, tx, ty);
            const float t = d <= speed ? 0.f : d / speed;
            if (best == kNoObject || t < best_t) best = u.logic_id, best_t = t;
        }
        return best;
    }
    case 15: { // GiveOrder(unit, type, insert, param, 4 floats, targetKind, t0[, t1])
        const std::uint32_t id = u32(0), insert = u32(2);
        UnitOrder o;
        if (!make_order(o, false)) return economy_ ? 0 : neutral(0);
        if (!orders_allowed) return 0;
        if (!world_.live(id)) return 5;
        return give(id, o, int(insert)) ? 1 : 0;
    }
    case 16:   // SumFreeResourceSpace(type, res): Σ capacity − value over the own units of exactly `type`
    case 17: { // SumResourceAmount(type, res): Σ value; RESOURCE_LEFT_ORE: Σ property 0x105
        if (!economy_) return neutral(fbits(0.f));
        const std::uint32_t type = u32(0), res = u32(1);
        float sum = 0;
        for (const auto& u : units_) {
            if (u.logic_id == kNoObject || u.type != type || !world_.live(u.logic_id)) continue;
            const economy::EcoState* st = economy_->state(u.logic_id);
            if (!st) continue;
            if (index == 17 && res == economy::kResourceLeftOre) sum += st->ore_left;
            else if (res == economy::kResourceOre) sum += index == 16 ? st->ore_max - st->ore : st->ore;
        }
        return fbits(sum);
    }
    case 18: { // FindBuildPlace(outX, outY)
        if (!economy_) {
            out(0, 0), out(1, 0);
            return neutral(1);
        }
        auto& spots = world_.reserved_spots; // 1000fb60: entries older than 60 s expire
        std::erase_if(spots, [&](const World::ReservedSpot& r) { return now_ - r.time > 60000; });
        planner_.refresh_lodes(world_, *economy_);
        std::vector<std::array<float, 2>> obstacles;
        for (const auto& o : world_.objects())
            if (o.alive && o.building()) obstacles.push_back({o.position().x, o.position().y});
        for (const auto& l : planner_.lodes)
            if (!l.has_mine) obstacles.push_back({l.pos.x, l.pos.y});
        for (const auto& r : spots) obstacles.push_back({r.x, r.y});
        const auto ok = [&](float x, float y) {
            if (world_.map_max_x > 0 && (x < 0 || x > world_.map_max_x || y < 0 || y > world_.map_max_y)) return false;
            if (world_.terrain_height) { // the four ±75 corners in valid areals, mean |Δh| ≤ 20 (1000fa80)
                float h[4];
                const float cx[4] = {-75, 75, 75, -75}, cy[4] = {-75, -75, 75, 75};
                for (int k = 0; k < 4; ++k) {
                    auto z = world_.terrain_height(x + cx[k], y + cy[k]);
                    if (!z) return false;
                    h[k] = *z;
                }
                const float mean = (h[0] + h[1] + h[2] + h[3]) / 4;
                float dev = 0;
                for (float v : h) dev += std::fabs(v - mean);
                if (dev / 4 > 20) return false;
            }
            for (const auto& b : obstacles)
                if (dist2d(b[0], b[1], x, y) < 250.f) return false;
            return true;
        };
        float fx = 0, fy = 0;
        bool found = false;
        for (float r = 0; r <= 1500 && !found; r += 150) {
            const float step = r > 0 ? 150.f / r : 7.f;
            for (float a = 0; a < 6.2831855f && !found; a += step) {
                const float x = base_x_ + r * std::cos(a), y = base_y_ + r * std::sin(a);
                if (ok(x, y)) fx = x, fy = y, found = true;
            }
        }
        out(0, ftol(fx)), out(1, ftol(fy));
        reserve_spot(fx, fy);
        return 1;
    }
    case 19: // GetClanBase(outX, outY, outClan)
        out(0, ftol(base_x_)), out(1, ftol(base_y_)), out(2, clan_);
        return 0;
    case 20:   // AddProblemExpression(problem, action, kind, id)
    case 27: { // AddExpression(action, kind, id)
        const std::size_t o = index == 20 ? 1 : 0;
        Problem* p = index == 20 ? problem(u32(0)) : cp;
        const std::uint32_t action = u32(o), kind = u32(o + 1);
        const std::uint32_t id = kind <= 1 ? u32(o + 2) : 0;
        if (p) p->exprs.push_back({action, kind, id, false});
        return 0;
    }
    case 21: // ReleaseProblemGroup(problem, group)
        if (Problem* p = problem(u32(0))) release_group(*p, std::int32_t(u32(1)));
        return 0;
    case 22: // CreateEmptyGroup(problem)
        if (Problem* p = problem(u32(0))) return std::uint32_t(add_group(*p));
        return kNoObject;
    case 23:   // AddUnitToProblemGroup(problem, group, logicId)
    case 26: { // AddUnitToGroup(group, logicId)
        const std::size_t o = index == 23 ? 1 : 0;
        Problem* p = index == 23 ? problem(u32(0)) : cp;
        const auto g = std::int32_t(u32(o));
        if (auto i = unit_index(u32(o + 1)); i && p)
            if (ProblemGroup* gr = group(*p, g)) gr->units.push_back(std::uint32_t(*i));
        return 0;
    }
    case 24: // ReleaseGroup(group)
        if (cp) release_group(*cp, std::int32_t(u32(0)));
        return 0;
    case 25: { // CreateGroup(mode, ...)
        if (!cp) return kNoObject;
        const std::uint32_t mode = u32(0);
        const std::int32_t g = add_group(*cp);
        const auto drop = [&] {
            release_group(*cp, g);
            return kNoObject;
        };
        if (mode == 0) return std::uint32_t(g);
        if (mode == 1) { // TAKE_ALL_FREE
            bool any = false;
            for (std::uint32_t i = 0; i < units_.size(); ++i)
                if (units_[i].problem == kUnitFree && world_.live(units_[i].logic_id)) take_into_group(*cp, g, i), any = true;
            return any ? std::uint32_t(g) : drop();
        }
        if (mode == 3) { // TAKE_ALL_BATTLE_UNITS: free, or busy on a problem of another type
            bool any = false;
            for (std::uint32_t i = 0; i < units_.size(); ++i) {
                const UnitRec& u = units_[i];
                if (u.problem == kUnitReserved || u.type != kTypeWarbot || !world_.live(u.logic_id)) continue;
                if (u.problem != kUnitFree && !(u.problem < problems_.size() && problems_[u.problem].type != cp->type)) continue;
                take_into_group(*cp, g, i), any = true;
            }
            return any ? std::uint32_t(g) : drop();
        }
        if (mode != 2) return std::uint32_t(g);
        // TAKE_BY_HITS(hitsInOut, targetKind, target...): nearest warbots first, free before busy.
        const float want = float(u32(1));
        float tx = 0, ty = 0;
        if (u32(2) == kTargetId) {
            auto p = object_pos(u32(3));
            if (!p) return std::uint32_t(g);
            tx = p->x, ty = p->y;
        } else {
            tx = f32(3), ty = f32(4);
        }
        std::vector<std::pair<float, std::uint32_t>> free_units, busy_units;
        for (std::uint32_t i = 0; i < units_.size(); ++i) {
            const UnitRec& u = units_[i];
            if (u.type != kTypeWarbot) continue;
            const bool is_free = u.problem == kUnitFree;
            if (!is_free && (u.problem == kUnitReserved || u.problem == cur || !lower_priority(u, cur_prio))) continue;
            auto p = pos(u);
            if (!p) continue;
            (is_free ? free_units : busy_units).push_back({dist2d(p->x, p->y, tx, ty), i});
        }
        std::stable_sort(free_units.begin(), free_units.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::stable_sort(busy_units.begin(), busy_units.end(), [](auto& a, auto& b) { return a.first < b.first; });
        float sum = 0;
        for (auto* list : {&free_units, &busy_units})
            for (auto& [d, i] : *list) {
                if (sum >= want && sum > 0) break;
                sum += unit_hits(units_[i].logic_id);
                take_into_group(*cp, g, i);
            }
        if (sum < want || sum == 0) out(1, ftol(sum));
        return sum <= 0 ? drop() : std::uint32_t(g);
    }
    case 28: { // GroupGiveOrder(group, type, insert, param, 4 floats, targetKind, t0[, t1])
        const auto g = std::int32_t(u32(0));
        const std::uint32_t insert = u32(2);
        UnitOrder o;
        if (!make_order(o, true)) return 0; // TARGET_BY_NAME: no design fits
        if (!orders_allowed || !cp) return 0;
        ProblemGroup* gr = group(*cp, g);
        if (!gr) return 0;
        for (std::uint32_t u : gr->units)
            if (u >= units_.size() || !give(units_[u].logic_id, o, int(insert))) return 0;
        return 1;
    }
    case 29: { // GetProblemParam(n)
        if (!cp || cp->type == 0) return kNoObject;
        const std::uint32_t n = u32(0);
        return n == 0 ? cp->c : n == 1 ? cp->d : n == 2 ? cp->e : 0;
    }
    case 30: // SendGameMessage(kind, id): host callback channel 0
        if (callback_) callback_(clan_, 0, u32(0), u32(1));
        return 0;
    case 31: { // CountClanUnits(clan, classMask)
        const std::uint32_t c = u32(0), mask = u32(1);
        const SuperAI* other = c < registry_.size() ? registry_[c] : nullptr;
        if (!other) return 0;
        std::uint32_t n = 0;
        for (const auto& u : other->units_)
            if (u.logic_id != kNoObject && (u.type & mask) != 0) ++n;
        return n;
    }
    case 32: return world_.in_tactical_areal(u32(0), u32(1)) ? 1 : 0; // IsInTacticalAreal(areal, id)
    case 33: { // SelectProblemsEvent(n)
        auto i = vm.program().find_event("Problems" + std::to_string(u32(0)));
        ev_problems_ = i ? std::int64_t(*i) : -1;
        return 0;
    }
    case 34: { // CountOwnUnits(type)
        const std::uint32_t type = u32(0);
        std::uint32_t n = 0;
        for (const auto& u : units_)
            if (u.logic_id != kNoObject && u.type == type) ++n;
        return n;
    }
    case 35: { // FindEnemyBuildingTarget(radius, outDefence)
        const float radius = f32(0);
        std::uint32_t best = kNoObject;
        int best_rank = 0;
        float best_def = 0;
        for (const auto& o : world_.objects()) {
            if (!o.alive || !o.building() || !hostile(o.clan)) continue;
            bool taken = false; // 10006670: already the target of a PBM_BUILDING_CAPTURE
            for (const auto& p : problems_) taken = taken || (p.type == kPbmBuildingCapture && p.c == o.logic_id);
            if (taken) continue;
            const Vec3 p = o.position();
            const float def = hits_around(p.x, p.y, radius, kNoObject);
            const int rank = capture_rank(o.type);
            if (best == kNoObject || rank < best_rank) best = o.logic_id, best_rank = rank, best_def = def;
        }
        if (best != kNoObject) out(1, ftol(best_def));
        return best;
    }
    case 36: { // FindWeakEnemyUnit(radius, outForce)
        const float radius = f32(0);
        std::uint32_t best = kNoObject;
        float best_f = 0;
        for (const auto& o : world_.objects()) {
            if (!o.alive || o.building() || !hostile(o.clan)) continue;
            const Vec3 p = o.position();
            const float f = hits_around(p.x, p.y, radius, kNoObject);
            if (best == kNoObject || f < best_f) best = o.logic_id, best_f = f;
        }
        if (best != kNoObject) out(1, ftol(best_f));
        return best;
    }
    case 37: { // FindWeakestOwnBuilding(outHits)
        std::uint32_t best = kNoObject;
        double best_h = 1e12;
        for (const auto& u : units_) {
            if (!(u.logic_id & kBuildingBit) || u.logic_id == kNoObject) continue;
            auto p = pos(u);
            if (!p) continue;
            const float h = hits_around(p->x, p->y, 250.f, clan_);
            if (best == kNoObject || h < best_h) best = u.logic_id, best_h = h;
        }
        out(0, ftol(best_h));
        return best;
    }
    case 38: { // CountClanHits(clan, FREE_UNITS 0 / ALL_UNITS 1)
        const std::uint32_t c = u32(0), mode = u32(1);
        if (c >= 0x41) return 0;
        const SuperAI* other = c < registry_.size() ? registry_[c] : nullptr;
        if (!other) return 0;
        float sum = 0;
        for (const auto& u : other->units_) {
            if (mode == 0 && (u.type != kTypeWarbot || u.problem != kUnitFree)) continue;
            sum += unit_hits(u.logic_id);
        }
        return ftol(sum);
    }
    case 39: return world_.live(u32(0)) ? 1 : 0; // ObjectExists
    case 40: { // FindMineSite(outX, outY, outHits)
        if (!economy_) {
            out(0, 0), out(1, 0);
            return neutral(kNoObject);
        }
        planner_.refresh_lodes(world_, *economy_);
        const LodeRecord* free = nullptr;
        for (const auto& l : planner_.lodes)
            if (!l.has_mine && (!free || dist2d(l.pos.x, l.pos.y, base_x_, base_y_) < dist2d(free->pos.x, free->pos.y, base_x_, base_y_)))
                free = &l;
        if (free) { // the free lode nearest the base
            out(0, ftol(free->pos.x)), out(1, ftol(free->pos.y));
            out(2, ftol(hits_around(free->pos.x, free->pos.y, 350.f, kNoObject)));
            reserve_spot(free->pos.x, free->pos.y);
            return kNoObject;
        }
        std::uint32_t best = kNoObject;
        float best_h = 0;
        for (const auto& l : planner_.lodes) { // a hostile clan's mine, the least defended
            if (!l.has_mine || l.mine_clan == clan_ || !hostile(l.mine_clan)) continue;
            auto p = object_pos(l.mine);
            if (!p) continue;
            const float h = hits_around(p->x, p->y, 350.f, kNoObject);
            if (best == kNoObject || h < best_h) best = l.mine, best_h = h;
        }
        if (best != kNoObject) {
            out(2, ftol(best_h));
            return best;
        }
        out(0, 0), out(1, 0);
        return kNoObject;
    }
    case 41: { // OrderNextResearch(institute)
        const std::uint32_t inst = u32(0);
        if (!world_.live(inst)) return kNoObject;
        if (!economy_) return neutral(0);
        const economy::Research* r = economy_->research(clan_);
        if (!planner_.loaded || !r) return 0;
        planner_.select_cheapest_research(*economy_, *r);
        if (planner_.selected < 0 || planner_.entries[std::size_t(planner_.selected)].available) return 0;
        UnitOrder o; // DesignPlanner::OrderResearch (10011100)
        o.type = economy::kOrderResearchScheme, o.param = 0, o.target_kind = kTargetName;
        o.design = planner_.entries[std::size_t(planner_.selected)].file;
        o.limits[7] = 1.f, o.limits[8] = 1.f, o.limits[9] = 1.f, o.limits[10] = 0;
        return give(inst, o, 3) ? 1 : 0;
    }
    case 42: // ClearProblemField48(flag)
        if (u32(0) == 0 && cp) cp->and_mode = 0;
        return 0;
    case 43: // LoadDesignList
        if (!economy_) return neutral(0);
        planner_.load(*economy_, economy_->research(clan_));
        return 0;
    case 44: { // HitsAt(radius, clan | ERROR, kind, a[, b])
        const float radius = f32(0);
        const std::uint32_t c = u32(1), kind = u32(2);
        if (kind == kTargetId) {
            auto p = object_pos(u32(3));
            return p ? ftol(hits_around(p->x, p->y, radius, c)) : 0;
        }
        if (kind == kTargetPlace) return ftol(hits_around(f32(3), f32(4), radius, c));
        return 0;
    }
    case 45: { // ProblemForceHits
        if (!cp) return 0;
        float sum = 0;
        for (std::uint32_t u : cp->units)
            if (u < units_.size()) sum += unit_hits(units_[u].logic_id);
        return ftol(sum);
    }
    case 46: return neutral(0); // Stub46
    case 47: { // FindBaseIntruder(extraDist, outHits)
        const float reach = float(u32(0)) + base_radius_;
        std::uint32_t best = kNoObject;
        float best_h = 0;
        for (const auto& o : world_.objects()) {
            if (!o.alive || o.building() || !hostile(o.clan)) continue;
            const Vec3 p = o.position();
            if (float(std::int64_t(dist2d(p.x, p.y, base_x_, base_y_))) >= reach) continue;
            bool taken = false;
            for (const auto& pr : problems_) taken = taken || (pr.type == kPbmBaseDefence && pr.c == o.logic_id);
            if (taken) continue;
            if (o.type == kTypeHero) {
                best = o.logic_id;
                break;
            }
            const float h = combat_hits(o.armour, o.weapon);
            if (best == kNoObject || h > best_h) best = o.logic_id, best_h = h;
        }
        out(1, ftol(best_h));
        return best;
    }
    case 48: { // FindSmallWarbotInProblem(flag): indexes the unit list by slot number (original bug)
        if (u32(0) != 0 || !cp) return kNoObject;
        for (std::size_t k = 0; k < cp->units.size() && k < units_.size(); ++k)
            if (const WorldObject* o = world_.live(units_[k].logic_id); o && small_warbot(*o)) return o->logic_id;
        return kNoObject;
    }
    case 49: return std::uint32_t(std::count(brains_.begin(), brains_.end(), kNoObject)); // CountFreeBrains
    case 50: { // FindProblemUnitWithOrder(orderType)
        const std::uint32_t type = u32(0);
        if (!cp) return kNoObject;
        for (std::uint32_t u : cp->units)
            if (u < units_.size())
                if (const WorldObject* o = world_.live(units_[u].logic_id); o && o->current_order() == type) return o->logic_id;
        return kNoObject;
    }
    case 51: // SetUnitReserved(logicId, flag)
        if (auto i = unit_index(u32(0))) units_[*i].problem = u32(1) != 0 ? kUnitReserved : kUnitFree;
        return 0;
    case 52: { // GetObjectClan
        const WorldObject* o = world_.live(u32(0));
        return o ? o->clan : kNoObject;
    }
    case 53: { // CountClanOreLodes(clan): that clan's lode table, records with flag +1
        const std::uint32_t c = u32(0);
        if (c >= 0x41) return 0;
        if (!economy_) return neutral(0);
        SuperAI* other = c < registry_.size() ? registry_[c] : nullptr;
        if (!other) return 0;
        other->planner_.refresh_lodes(world_, *economy_);
        return std::uint32_t(std::count_if(other->planner_.lodes.begin(), other->planner_.lodes.end(), [](const LodeRecord& l) { return l.flag; }));
    }
    case 54: { // GetObjectOrderType
        const WorldObject* o = world_.live(u32(0));
        return o ? o->current_order() : kNoObject;
    }
    case 55: { // IsObjectOnMap
        const WorldObject* o = world_.live(u32(0));
        if (!o) return kNoObject;
        const Vec3 p = o->position();
        return p.x >= 0 && p.x <= world_.map_max_x && p.y >= 0 && p.y <= world_.map_max_y ? 1 : 0;
    }
    case 56: // AllDesignsAvailable: planner P+0x25
        if (!economy_) return neutral(0);
        return planner_.all_available ? 1 : 0;
    case 57:                           // RunMissionScriptCommand(n, unused): host callback channel 2
        if (callback_) callback_(clan_, 2, u32(0), u32(1));
        return 0;
    case 58: { // FindUpgradableBuilding(n): the n-th own building passing 100068b0
        if (!economy_) return neutral(kNoObject);
        std::uint32_t n = u32(0);
        for (const auto& u : units_)
            if (upgradable(u) && n-- == 0) return u.logic_id;
        return kNoObject;
    }
    case 59: return clock_ + u32(0);    // MakeDeadline(seconds)
    case 60: return u32(0) < clock_ ? 1 : kNoObject; // DeadlinePassed: 1 or ERROR
    case 61: { // GetObjectType
        const WorldObject* o = world_.live(u32(0));
        return o ? o->type : kNoObject;
    }
    case 62: // SetProblemPriority(p)
        if (cp) cp->priority = f32(0);
        return 0;
    case 63: { // IsInsideBase
        auto p = object_pos(u32(0));
        return p && dist2d(p->x, p->y, base_x_, base_y_) < base_radius_ ? 1 : 0;
    }
    case 64: { // FindEnemyBuildingOutsideBase(radius, outHits)
        const float radius = f32(0);
        std::uint32_t best = kNoObject;
        float best_d = 0, best_h = 0;
        for (const auto& o : world_.objects()) {
            if (!o.alive || !o.building() || !hostile(o.clan)) continue;
            const Vec3 p = o.position();
            const float d = dist2d(p.x, p.y, base_x_, base_y_);
            if (d < base_radius_) continue;
            const float h = hits_around(p.x, p.y, radius, kNoObject);
            if (best == kNoObject || d < best_d) best = o.logic_id, best_d = d, best_h = h;
        }
        if (best != kNoObject) out(1, ftol(best_h));
        return best;
    }
    case 65: { // IsLargeRobotResearched
        if (!economy_) return neutral(0);
        if (!planner_.loaded || !economy_->research(clan_)) return 0;
        if (planner_.large_latch) return 1;
        for (const auto& d : planner_.entries)
            if (d.available && !(d.type & kBuildingBit) && d.size > 2 && d.weapon >= 105000.f) {
                planner_.large_latch = true;
                for (std::size_t i = 0; i < problems_.size(); ++i)
                    if (problems_[i].type == kPbmBuildingCapture) set_problem_state(std::uint32_t(i), kStSolved);
                return 1;
            }
        planner_.large_latch = false;
        return 0;
    }
    case 66: { // GetCaptureBonus(logicId, outBonus)
        const WorldObject* o = world_.live(u32(0));
        if (!o) return 0;
        ref(1).set_float(o->type == kTypeGenerator ? 0.04f : o->type == kTypePlant ? 0.05f : 0.f);
        return o->type;
    }
    case 67: { // FindOwnPlant(outHits); without a plant the original writes garbage (left unchanged)
        for (const auto& u : units_)
            if (u.type == kTypePlant && u.logic_id != kNoObject)
                if (auto p = pos(u)) {
                    out(0, ftol(hits_around(p->x, p->y, 250.f, clan_)));
                    return u.logic_id;
                }
        return kNoObject;
    }
    case 68: { // SetMaxBaseRadius(r)
        const float r = f32(0);
        if (r > 0) {
            base_radius_ = 0;
            max_base_radius_ = r;
            grow_base_radius();
        }
        return 1;
    }
    case 69: // SetDesignChoiceSpread(n)
        if (std::int32_t(u32(0)) >= 0) planner_.spread = u32(0);
        return 1;
    case 70: { // Random(n)
        const auto n = std::int32_t(u32(0));
        if (n == 0) return neutral(0);
        return std::uint32_t(rand_() % n);
    }
    case 71: { // FindWeakestEnemyBuildingOfType(radius, type, outHits)
        const float radius = f32(0);
        const std::uint32_t type = u32(1);
        std::uint32_t best = kNoObject;
        double best_h = 1e12;
        for (const auto& o : world_.objects()) {
            if (!o.alive || !o.building() || o.type != type || !hostile(o.clan)) continue;
            const Vec3 p = o.position();
            const float h = hits_around(p.x, p.y, radius, kNoObject);
            if (best == kNoObject || h < best_h) best = o.logic_id, best_h = h;
        }
        out(2, ftol(best_h));
        return best;
    }
    case 72: { // GetSizeClass
        const WorldObject* o = world_.live(u32(0));
        return o ? o->size_class : kNoObject;
    }
    default:
        return 0;
    }
}

} // namespace parkan::game
