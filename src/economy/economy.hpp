// The economy and base-building core of a running mission: ore and power per object, one Resource
// Distributor and one research tree per clan, and the Behavior.dll tasks that turn resources into things:
//   building tasks  M_Task_Mine (0xa), M_Task_Construct (0xc, plants), M_Task_Research (0xe / 0x10,
//                   institutes), M_Task_ShowUpgrade (0x12, the build / upgrade show);
//   unit tasks      M_Task_Build (7, builders), M_Task_Upgrade (0x18, builders), M_Task_Transport (6).
// ORDER_LEAVE (0x14), the walk out of a plant, is a unit brain task (ai/behaviour.cpp); the economy gives it.
// Spec: notes/base-building.md (formulas, distributor, tasks), notes/designs-research.md (costs, research),
// notes/hq-ui.md (what the HQ screens read and order).
//
// HOW IT IS WIRED
// - The economy sits on the strategy layer's world model (game::World): every object keeps its LogicID,
//   clan, type and position there; the economy adds its state (EcoState) by LogicID.
// - Orders reach it through the objects' UnitControl: wrap_controls() puts an EconomyControl in front of
//   every object's control (buildings have none of their own). Economy orders (6, 7, 0xa, 0xc, 0xe, 0x10,
//   0x12, 0x18) become economy tasks; the other orders go to the wrapped control (the unit's brain). So the
//   SuperAI scripts (game::SuperAI::give) and the HQ (the order_* calls below) use the same path.
// - Units walk through a Mover (set_mover; play implements it over ai::AiUnit). Without one a unit "walks"
//   on a timer at kFallbackSpeed without moving [port: headless runs].
// - New objects (a produced robot, a new building) are added to the world and reported as events; the
//   host (MissionRuntime / the app) creates their bodies and registers them with the SuperAI.
//
// PLAY WIRING (for src/app): MissionRuntime::load attaches the economy. The app then
//   - after attaching its UnitAdapters, lets the first MissionRuntime::update wrap them (or calls
//     Economy::wrap_controls); for an object it spawns later it attaches the control and calls
//     MissionRuntime::object_added(lid);
//   - gives every builder / transport a Mover (set_mover) and sets radius_of (the models' bounding
//     spheres), ground_height (terrain) and World::terrain_height (h18);
//   - handles MissionRuntime::set_economy_sink events: UnitProduced → create the robot's body at
//     World::find(lid)->placed (it then receives ORDER_LEAVE through its control), BuildingPlaced → create
//     the building (show_animation / footprint drive its look during the show), BuildingUpgraded → swap
//     the model (same LogicID, World::find(lid)->resource), ResearchDone → VOICE_RSRCH_COMPLETE + pager 0x7da;
//   - the HQ screens read plant() / institute() / builder() / research(clan) / minerals_percent /
//     energy_percent / free_brains and order through order_construct / order_research / order_build
//     (+ set_temp_building, placement_ok for the ghost) / order_upgrade (+ can_upgrade) / order_transport.
//
// PORT DECISIONS (not traced; marked [inferred] / [port] at their place):
// - pf (the power factor, Σ device property 0xe00) uses the base-building.md §8 approximation: 1 while the
//   building's batteries hold charge, 0 when empty; the batteries drain by power_usage · dt. Initial charge
//   fraction 1.0. Battery capacity = Σ the design's type-0x13 module devices (econ_data.hpp).
// - Task takts run on the MBehaviour task timer (0.45–0.9 s, MB+0x938); dt = elapsed since the task's
//   previous takt. The builder's tool device is taken as powered (10029420 / 10033250 not modelled).
// - Bounding spheres for the placement test: Economy::radius (a per-type table unless the host sets
//   radius_of). The test is against buildings (class 3) only.
// - LogicIDs of new objects: one above the highest of the same kind (bit 31 for buildings).
// - The clans share one lode table (the system map's mineral records).
#pragma once

#include "economy/distributor.hpp"
#include "economy/econ_data.hpp"
#include "economy/research.hpp"
#include "game/world.hpp"

#include <deque>
#include <memory>

namespace parkan::economy {

enum EcoOrder : std::uint32_t {
    kOrderTransport = 6,
    kOrderBuild = 7,
    kOrderMine = 0xa,
    kOrderConstruct = 0xc,
    kOrderResearch = 0xe,
    kOrderResearchScheme = 0x10,
    kOrderShowUpgrade = 0x12,
    kOrderLeave = 0x14,
    kOrderUpgrade = 0x18,
};
inline constexpr std::uint32_t kTargetLogicId = 0x201, kTargetPlace = 0x202, kTargetNone = 0x204, kTargetName = 0x205,
                               kTargetMatrix = 0x206;
inline constexpr std::uint32_t kResourceOre = 0x2000100, kResourceLeftOre = 0x0afc0900; // varset.var
inline constexpr std::uint32_t kTempBuilding = 0x80000000; // M_Task_Build param: use UNITS\tempbld.dat
inline constexpr const char* kTempBuildingScheme = "UNITS\\tempbld.dat";
inline constexpr float kFallbackSpeed = 10.f; // m/s of a unit without a Mover [port]

bool is_economy_order(std::uint32_t type);

struct EconomyEvent {
    enum class Type {
        UnitProduced,     // a plant finished a robot (OnWarbotBuilt, 1002a920): object = new unit, source = plant
        BuildingPlaced,   // a builder created a building (10029240); its build show starts: object, source = builder
        BuildingBuilt,    // the show ended ("building built", OnBuildingBuilt 1000c920): object
        BuildingUpgraded, // M_Task_Upgrade replaced the building by its next level (same LogicID): object
        ResearchDone,     // MarkResearched by an institute: item, source = institute
        TaskComplete,     // an economy task ended normally (NotifyObjectEvent 3): object = the worker
        TaskFailed,       // the order failed (OnBuilderFailed 0x102 technology / 0x103 placement, ...): object
    };
    Type type = Type::TaskComplete;
    std::uint32_t object = game::kNoObject;
    std::uint32_t source = game::kNoObject;
    std::uint32_t clan = game::kNoClan;
    std::int32_t item = -1;  // ResearchDone: the research item
    std::uint32_t code = 0;  // TaskComplete/TaskFailed: the order type; TaskFailed also: 0x102 / 0x103 in `item`
    std::string scheme;      // the design of a new object
};
const char* to_string(EconomyEvent::Type t);

// A unit's legs for the walking tasks (play: over ai::AiUnit — go_to = an ORDER_GO to the brain, idle = the
// walker is idle). idle() must read false from a successful go_to until the unit has stopped.
class Mover {
public:
    virtual ~Mover() = default;
    virtual bool go_to(Vec3 target) = 0; // false = unreachable
    virtual bool idle() const = 0;       // arrived (or stopped)
};

// --- queries for the HQ screens (hq-ui.md §7–§9) ---
struct PlantInfo {
    bool busy = false;           // current task is 0xc (PlantList "idle" = !busy)
    std::string scheme;          // design being produced
    float progress = 0;          // min(time/T, ore/O, power/P, 1)
    float ore_cost = 0, ore_got = 0, power_cost = 0, power_got = 0, time_cost = 0, elapsed = 0;
    std::uint32_t free_bots = 0; // attribute/property 0x803
    std::uint32_t size_class = 0; // MB+0x960
    std::size_t queued = 0;      // orders after the current one
};
struct InstituteInfo {
    bool busy = false;
    std::int32_t item = -1;       // the item being researched (-1 between items)
    float item_progress = 0;      // IResearch progress of that item
    std::vector<std::int32_t> list; // the task's item list
    std::uint32_t free_techno = 0;
    float research_time = 2;
    std::size_t queued = 0;
};
struct BuilderInfo {
    std::uint32_t order = 0; // 7 build, 0x18 upgrade, 6 transport, 0 none
    int state = -1;          // the task's state (M_Task_Build 0..4, Upgrade 1..4, Transport 0..6)
    std::uint32_t building_type = 0;
    std::uint32_t target = game::kNoObject; // storage/mine/building in use
    float ore = 0, ore_cost = 0;
};

class Economy;

// The UnitControl in front of an object's own control (wrap_controls).
class EconomyControl final : public game::UnitControl {
public:
    EconomyControl(Economy& eco, std::uint32_t lid, game::UnitControl* inner) : eco_(eco), lid_(lid), inner_(inner) {}
    bool give_order(const game::UnitOrder& order, int mode) override;
    std::uint32_t current_order() const override;
    Vec3 position() const override;
    float speed() const override { return inner_ ? inner_->speed() : 0.f; }
    game::UnitControl* inner() const { return inner_; }
    void set_inner(game::UnitControl* c) { inner_ = c; }

private:
    friend class Economy;
    Economy& eco_;
    std::uint32_t lid_;
    game::UnitControl* inner_;
    std::deque<std::pair<game::UnitOrder, int>> pending_; // mode-1 brain orders held while an economy task runs
};

struct EconomyData {
    std::map<std::string, Profession> professions;
    BuildTable build_table;
    BehConstants constants;
};

class Economy {
public:
    // `tma` gives the object parameters and the lodes; `game_dir` the schemes, behpsp.res, BuildDat.lst,
    // the clans' .trf and the battery modules. Missing files leave the defaults (no research tree, …).
    Economy(game::World& world, const std::filesystem::path& game_dir, const mission::Tma* tma, std::uint32_t seed = 1);
    // Headless construction (tests): data given, no files.
    Economy(game::World& world, EconomyData data, SchemeCache schemes, std::vector<Lode> lodes, std::uint32_t seed = 1);
    Economy(const Economy&) = delete;
    Economy& operator=(const Economy&) = delete;
    ~Economy();

    // --- wiring ---
    // Put an EconomyControl in front of every live object's control (idempotent; re-wraps a control the
    // host replaced). Call after the host attached its controls.
    void wrap_controls();
    // OnInternalInit for every live object: economy state, distributor membership (buildings of a clan
    // with a strategist, `has_superai(clan)`), the default orders (mines: Mine). Called once at start.
    void start(const std::function<bool(std::uint32_t clan)>& has_superai);
    void set_mover(std::uint32_t lid, Mover* m);
    void set_event_sink(std::function<void(const EconomyEvent&)> sink) { sink_ = std::move(sink); }
    // The clan's SuperAI brain list (ISuperAI slots 14–17; owner LogicID, -1 free).
    void set_brains(std::function<std::vector<std::uint32_t>*(std::uint32_t clan)> f) { brains_ = std::move(f); }
    // Bounding sphere radius of an object (placement test, unit push, stand points); default: by type.
    std::function<float(const game::WorldObject&)> radius_of;
    // Ground height for new objects (z of a produced robot = ground + 15 m); default: the plant's z.
    std::function<float(float x, float y)> ground_height;

    // Once per game frame.
    void update(std::uint32_t now_ms);

    // World changes the host reports.
    void on_object_removed(std::uint32_t lid); // death: distributor Remove, tasks dropped, brain freed
    void on_object_captured(std::uint32_t lid, std::uint32_t old_clan); // after World::set_clan (Capture 10008e40)
    // A new object the host created (e.g. a scripted spawn): state + default order + distributor.
    void on_object_added(std::uint32_t lid);

    // --- orders (IBehaviour::GiveOrder for the economy order types) ---
    bool give_order(std::uint32_t lid, const game::UnitOrder& order, int mode);
    void cancel(std::uint32_t lid); // drop every economy task of the object (control slot 0xa0(0))
    std::uint32_t current_order(std::uint32_t lid) const; // 0 = no economy task
    bool showing(std::uint32_t lid) const;                // property 0x20c: the current task is 0x12

    // --- HQ / play API ---
    // Factory screen (PlantRec_IssueBuildOrder 100873b0): {0xc, param = the scheme's type, 0x205 path}, mode 3.
    bool order_construct(std::uint32_t plant, std::string_view scheme_path, int mode = 3);
    // Research screen (ResearchUI_OrderItem 100884c0): the clan's alive, not-showing institute with the
    // shortest queue gets {0xe, item, 0x204} with mode 1. False when the item is researched/queued or no
    // institute exists.
    bool order_research(std::uint32_t clan, std::int32_t item);
    // Build screen: {7, type (or kTempBuilding with set_temp_building), 0x206 matrix}; angle about z.
    bool order_build(std::uint32_t builder, std::uint32_t building_type, Vec3 place, float angle = 0, int mode = 3);
    void set_temp_building(mission::Scheme s) { schemes_.put(kTempBuildingScheme, std::move(s)); }
    // CanUpgrade (iron3d 100343f0 / M_Task_Upgrade::SetTarget 100332e0) and cmd 0x20.
    bool can_upgrade(std::uint32_t builder, std::uint32_t building) const;
    bool order_upgrade(std::uint32_t builder, std::uint32_t building);
    bool order_transport(std::uint32_t transport); // cmd 0x14: {6, -1, 0x204}
    // The placement test of CreateObjectFromScheme for a ghost: true when a building of `type` (scheme
    // level 0, or the temp scheme) fits at `place`.
    bool placement_ok(std::uint32_t type, Vec3 place) const;
    // Distributor shares: the original exposes SetShare (slot 3) but no UI calls it (hq-ui.md §7).
    void set_share(std::uint32_t clan, std::uint32_t lid, float share, bool lock);

    std::optional<PlantInfo> plant(std::uint32_t lid) const;
    std::optional<InstituteInfo> institute(std::uint32_t lid) const;
    std::optional<BuilderInfo> builder(std::uint32_t lid) const;
    std::vector<std::int32_t> research_orders(std::uint32_t clan) const; // ResearchUI_CollectResearchOrders
    std::uint32_t free_brains(std::uint32_t clan) const;                // CountFreeBrains
    // Resource strip (hq-ui.md §7): minerals % = clamp(oreInBuffer / 4500, 0, 1) · 100, energy % =
    // clamp((powerAvail − powerUsage) / Σ clans' powerAvail, 0, 1) · 100.
    float minerals_percent(std::uint32_t clan) const;
    float energy_percent(std::uint32_t clan) const;

    // --- state ---
    EcoState* state(std::uint32_t lid);
    const EcoState* state(std::uint32_t lid) const;
    Research* research(std::uint32_t clan);
    const Research* research(std::uint32_t clan) const;
    void set_research(std::uint32_t clan, Research r);
    Distributor* distributor(std::uint32_t clan);
    const Distributor* distributor(std::uint32_t clan) const;
    std::vector<Lode>& lodes() { return lodes_; }
    const std::vector<Lode>& lodes() const { return lodes_; }
    SchemeCache& schemes() { return schemes_; }
    const EconomyData& data() const { return data_; }
    game::World& world() { return world_; }
    const game::World& world() const { return world_; }
    std::uint32_t level(std::uint32_t lid) const;          // property 0x209
    std::uint32_t building_size(std::uint32_t lid) const; // MB+0x960 / property 0x201
    const std::vector<EconomyEvent>& events() const { return log_; }
    std::uint32_t now() const { return now_; }
    float radius(const game::WorldObject& o) const;
    void set_object_params(std::uint32_t lid, const ObjectParams& p); // data.tma parameters (start / tests)
    // The level-n scheme of a building type (BuildDat.lst), or the temp scheme for kTempBuilding.
    const std::string* building_scheme(std::uint32_t type, std::size_t level) const;
    // The build show (M_Task_ShowUpgrade): the building's animation (MB+0x9fc) and footprint mode (MB+0x968:
    // 1 normal, 2 during the show, 0 hidden) for the renderer.
    std::int32_t show_animation(std::uint32_t lid) const;
    int footprint(std::uint32_t lid) const;

    struct Task;     // economy.cpp
    struct Worker;   // per object: params, task queue, timers

private:
    friend class EconomyControl;
    friend struct EconomyAccess;
    Worker& worker(std::uint32_t lid);
    const Worker* find_worker(std::uint32_t lid) const;
    void init_object(game::WorldObject& o);
    std::unique_ptr<Task> make_task(std::uint32_t lid, const game::UnitOrder& order);
    void emit(EconomyEvent e);
    void run_tasks(Worker& w);
    std::uint32_t new_logic_id(bool building) const;
    // CreateObjectFromScheme (ArealMap 1001d440) for the economy: the sphere test (buildings), the world
    // object, its state, its brain (units, flag 8). Returns the LogicID or kNoObject (refused).
    std::uint32_t create_object(std::string_view scheme_path, Vec3 pos, float angle, std::uint32_t clan, std::uint32_t source,
                                std::uint32_t keep_lid = game::kNoObject);
    bool sphere_free(Vec3 c, float r, std::uint32_t type, std::uint32_t ignore) const;
    void join_distributor(std::uint32_t lid);
    void leave_distributor(std::uint32_t lid, std::uint32_t clan);
    std::uint32_t* claim_brain(std::uint32_t clan, std::uint32_t owner);
    bool walk(std::uint32_t lid, Vec3 target); // start a walk
    bool walking_done(std::uint32_t lid) const;

    game::World& world_;
    EconomyData data_;
    SchemeCache schemes_;
    std::vector<Lode> lodes_;
    std::uint32_t rand_seed_;
    ai::RandomSource rand_;
    std::map<std::uint32_t, Research> research_;
    std::map<std::uint32_t, Distributor> distributors_;
    std::map<std::uint32_t, std::unique_ptr<Worker>> workers_;
    std::map<std::uint32_t, std::unique_ptr<EconomyControl>> controls_;
    std::function<void(const EconomyEvent&)> sink_;
    std::function<std::vector<std::uint32_t>*(std::uint32_t)> brains_;
    std::function<bool(std::uint32_t)> has_superai_;
    std::vector<EconomyEvent> log_;
    std::uint32_t now_ = 0, last_update_ = 0;
    bool started_ = false;
    ResourceLookup lookup_;
};

} // namespace parkan::economy
