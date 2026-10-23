// One clan's script-driven strategist: ai.dll CSuperAI (ctor 10001000, CreateSuperAI 1000f710,
// ISuperAI vtable 100341b8) with its task manager ("mgr", CSuperAI+0x7c, ctor 100057f0): the unit
// list, the problem list and groups, the relations, the brains, the AI clock, and the 73 script
// handlers. Spec: notes/script-vm.md, notes/script-handlers-a.md, notes/script-handlers-b.md,
// notes/components.md "ISuperAI"; decompiled slots in notes/decomp/ai.dll/isuperai.c.
//
// ECONOMY HANDLERS (set_economy; without an economy they return the neutral values below and count
// them in neutral_calls): h3 GetEnergySurplus (the clan distributor's summary), h15/h28 TARGET_BY_NAME
// (the design planner), h16/h17 the ore attributes, h18 FindBuildPlace, h40 FindMineSite (the lode
// table), h41 OrderNextResearch, h43 LoadDesignList, h53 CountClanOreLodes, h56 AllDesignsAvailable,
// h58 FindUpgradableBuilding, h65 IsLargeRobotResearched (game/design_planner.hpp, notes/base-building.md §6).
// Neutral without an economy: h3 → 0.0; h15 by name → no order, 0; h16/h17 → 0.0; h18 → 0, 0, returns 1;
// h40 → 0, 0, -1; h41 → 0 (-1 when the institute is gone); h43/h53/h56/h65 → 0; h58 → -1.
// Always neutral: h46 Stub46 → 0 (the original returns a stale FPU value); h70 Random(0) → 0 (the
// original divides by zero).
// h18 without World::terrain_height accepts every point's corners (no areal / flatness test) [port].
//
// Port decisions (not traced in the original):
//   - A cleared problem slot (type 0) is reused by the next AddProblem; the spec says "appends".
//   - Expression completion (10005180) as the action names say: DESTROY = the object is gone,
//     NOTHING_DOING = the unit's current order is 0, CAPTURE_BUILDING = the building is ours, GROUP_*
//     = the same for every unit of the group. A satisfied problem is cleared like ST_SOLVED.
//   - A new problem starts in AND mode (+0x48 = 1): h42, the only writer, can only clear it.
//   - _Start runs for the single highest-priority problem that is neither SOLVING nor SOLVED.
//   - Dead objects: on_object_removed marks the unit record dead (LogicID -1) and drops it from its
//     problem (the original's removal path is not traced).
#pragma once

#include "ai/script_vm.hpp"
#include "game/design_planner.hpp"
#include "game/world.hpp"

#include <functional>
#include <map>

namespace parkan::game {

using RandomSource = std::function<int()>; // the CRT's rand() (0..0x7fff)

// CreateSuperAI's 10th argument (iron3d OnSuperAINotify 100611d0): channel 0 = SendGameMessage
// (kind, id), channel 2 = run mission.cfg script<kind>.
using HostCallback = std::function<void(std::uint32_t clan, std::uint32_t channel, std::uint32_t kind, std::uint32_t b)>;

inline constexpr std::uint32_t kUnitFree = 0xFFFFFFFF, kUnitReserved = 0xFFFE; // UnitRec +0x14
inline constexpr std::size_t kMaxClans = 64;                                   // GetSuperAI table 10055398

enum ProblemState : std::uint32_t { kStNone = 0, kStSolving = 1, kStSolved = 2, kStUnsolved = 3 };

struct UnitRec { // 0x20 bytes at CSuperAI+0x8c
    std::uint32_t logic_id = kNoObject; // -1 = dead
    std::uint32_t type = 0;
    bool critical = false;
    std::uint32_t problem = kUnitFree; // problem index, kUnitFree, or kUnitReserved
};

struct ProblemExpr {
    std::uint32_t action = 0, kind = 0, id = 0;
    bool done = false;
};

struct ProblemGroup { // 0x20 bytes: +8 own index (-1 = deleted), +0xc unit-list indices
    std::int32_t index = -1;
    std::vector<std::uint32_t> units;
};

struct Problem { // 0x64 bytes at CSuperAI+0xa0 (ctor 10004e50)
    std::uint32_t type = 0; // PBM_*; 0 = free slot
    std::uint32_t c = 0, d = 0, e = 0;
    float priority = 0;
    std::uint32_t state = kStNone;
    std::vector<std::uint32_t> units; // unit-list indices
    std::int32_t lifetime = 0, refill = 0, decay = 0;
    bool critical_lost = false;
    std::vector<ProblemExpr> exprs;
    std::uint32_t and_mode = 1;
    std::vector<ProblemGroup> groups;
};

class SuperAI : public ai::ScriptHost {
public:
    // `registry` is the GetSuperAI table shared by every clan (at least kMaxClans entries); the
    // SuperAI enters itself at [clan] and leaves on destruction. Relations start from world.clans.
    SuperAI(World& world, std::vector<SuperAI*>& registry, std::uint32_t clan, ai::ScriptProgram program,
            HostCallback callback, RandomSource rand);
    ~SuperAI() override;
    SuperAI(const SuperAI&) = delete;
    SuperAI& operator=(const SuperAI&) = delete;

    // ISuperAI slot 5 Start(mode) (10001ae0): stores the mode (+0x3fc) and runs Init the first time.
    void start(std::uint32_t mode);
    // Slot 9's body (10001b80): runs Mission. The 2-second gate is global in the original
    // (DAT_10054c68) and lives in MissionRuntime.
    void run_mission_event();
    // Slot 3 Takt (10001780), called every game frame: every 7 s + rand()%1000 ms the AI clock gains 7,
    // problems age, _Continue runs for SOLVING problems, then Problems, then one _Start.
    void takt(std::uint32_t now_ms);
    // Slot 4 OnObjectAdded(kind 1 unit / 2 building, logicId) (10001880). Ignored before Start unless
    // `before_start` (the port registers the placed objects first so Init's SetUnitReserved finds them).
    void on_object_added(std::uint32_t kind, std::uint32_t logic_id, bool before_start = false);
    void on_object_removed(std::uint32_t logic_id);
    // Slot 18 NotifyObjectEvent (100020a0 → 10005d70): dCurrentSender = sender; units: 1 GeneratorFound,
    // 2 MineralFound, 3 TaskComplete, 8 HeroTeleported (Mech_* events); buildings: 3 Fort_Task_Complete,
    // 7 Fort_Captured.
    void notify_object_event(std::uint32_t sender, std::uint32_t event);

    // Slots 6–8 / 13: relations 0 hostile, 1, 2, mirrored into the other clan's SuperAI (10005e80).
    void set_relation(std::uint32_t clan, std::uint32_t value);
    std::uint32_t relation(std::uint32_t clan) const;
    void add_hostility(std::uint32_t clan); // +0.004 to the attitude-side float
    float attitude(std::uint32_t clan) const { return clan < kMaxClans ? rel_[clan].attitude : 0.f; }

    // Slots 14–17: brain records; the first dword is the owner LogicID (-1 = free).
    void add_brain(std::uint32_t owner = kNoObject) { brains_.push_back(owner); }
    bool remove_brain(std::size_t i);
    std::size_t brain_count() const { return brains_.size(); }
    std::uint32_t& brain(std::size_t i) { return brains_.at(i); }
    std::vector<std::uint32_t>& brains() { return brains_; }

    // The economy the handlers of base-building.md §6 read (nullptr: neutral values). Not owned.
    void set_economy(economy::Economy* e) { economy_ = e; }
    DesignPlanner& planner() { return planner_; }

    std::uint32_t call_handler(std::uint32_t index, ai::ScriptVM& vm, const ai::Record& rec) override;

    std::uint32_t clan() const { return clan_; }
    std::uint32_t mode() const { return mode_; }
    std::uint32_t clock() const { return clock_; }
    float base_x() const { return base_x_; }
    float base_y() const { return base_y_; }
    float base_radius() const { return base_radius_; }
    const std::vector<UnitRec>& units() const { return units_; }
    std::vector<Problem>& problems() { return problems_; }
    ai::ScriptVM& vm() { return vm_; }
    std::uint32_t current_problem() const; // the dCurrentProblem variable

    std::array<std::uint64_t, ai::kHandlerCount> handler_calls{};
    std::array<std::uint64_t, ai::kHandlerCount> neutral_calls{};
    std::uint64_t orders_given = 0, orders_refused = 0;

private:
    struct Relation {
        float attitude = 0, hostility = 0;
        std::uint32_t value = 1;
    };

    void run_event(std::int64_t event);
    std::optional<std::size_t> unit_index(std::uint32_t logic_id) const;
    Problem* problem(std::uint32_t index);
    bool hostile(std::uint32_t clan) const;
    float unit_hits(std::uint32_t logic_id) const;
    float hits_around(float x, float y, float radius, std::uint32_t clan) const;
    std::optional<Vec3> object_pos(std::uint32_t logic_id) const;
    bool lower_priority(const UnitRec& u, float than) const;
    void release_unit(std::uint32_t unit, bool repair_order);
    void assign_unit(std::uint32_t unit, std::uint32_t problem);
    void release_problem_units(Problem& p);
    void set_problem_state(std::uint32_t index, std::uint32_t state);
    void release_group(Problem& p, std::int32_t group);
    std::int32_t add_group(Problem& p);
    ProblemGroup* group(Problem& p, std::int32_t g);
    bool give(std::uint32_t logic_id, const UnitOrder& order, int mode);
    void grow_base_radius();
    void update_problems();
    bool expr_done(const Problem& p, const ProblemExpr& x) const;
    void reserve_spot(float x, float y); // 1000fb60
    bool upgradable(const UnitRec& u) const; // 100068b0

    World& world_;
    std::vector<SuperAI*>& registry_;
    std::uint32_t clan_;
    ai::ScriptVM vm_;
    HostCallback callback_;
    RandomSource rand_;

    std::uint32_t mode_ = 0;        // +0x3fc
    bool init_done_ = false;        // +0x8a8
    bool running_ = false;          // +0x78
    std::uint32_t clock_ = 0;       // +0x854
    std::int64_t next_problems_ = 0; // +0x860/+0x864
    float base_x_ = 0, base_y_ = 0, base_radius_ = 0, max_base_radius_ = 0;
    std::vector<UnitRec> units_;
    std::vector<Problem> problems_;
    std::map<std::uint32_t, std::pair<std::int64_t, std::int64_t>> problem_events_; // type → _Start, _Continue
    std::array<Relation, kMaxClans> rel_{};
    std::vector<std::uint32_t> brains_;
    std::array<std::uint32_t, 7> built_{}; // +0x3e0..+0x3f8: mine, storage, plant, hangar, tower M, tower L, institute
    DesignPlanner planner_;                // +0x40c (spread +0x10: h69)
    economy::Economy* economy_ = nullptr;
    std::uint32_t now_ = 0;                // the last Takt time (timeGetTime for 1000fb60 / h15)
    std::int64_t ev_mission_ = -1, ev_problems_ = -1, ev_mech_ = -1, ev_fort_ = -1;
    std::optional<std::uint32_t> var_current_problem_, var_current_sender_;
};

} // namespace parkan::game
