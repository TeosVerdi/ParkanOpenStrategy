// The player's HQ (МЦУ) in iron3d.dll: the view-state stack that leads into it, the HQ camera, selection
// and orders to own units, the sidebar, the unit and building list screens, the order menu, the building
// placement ghost, the research screen, the factory screen and the resource strip. Spec: notes/hq-ui.md
// (section numbers below), notes/base-building.md, notes/designs-research.md.
//
// SDL-free like the cockpit (ui/hud): the app feeds the pointer, keys and world picks in; the controller
// keeps the HQ state, gives orders through the objects' controls in game::World (the path the scripts use,
// the economy's EconomyControl in front) and the economy's order_* calls; draw_hq() renders it as overlay
// quads on the 640×480 virtual screen.
//
// Port decisions (marked [inferred] / [port] / [deferred] at their place):
// - View states 1 (piloting a warbot), 2 (remote control), 3 (HQ through a mobile HQ unit), 6 (a manned turret)
//   and 7 (the in-game menu): the queue and the HQ side here; the game side (the controlled unit, the hero's body,
//   the turret, the pause) through HqHost::transition in the app (view-states.md).
// - Cursors: the system pointer, or with FORCE_SOFTWARE_CURSOR the ui/cursor.cfg sprites (draw_cursor).
// - The 3D previews (factory 115×115, research 127×127, the designer's views) are overlay insets (draw_model_view).
// - The unit designer (CConstructorShell / CConstructor) is ui::Designer (ui/designer.hpp); the factory works
//   with the recent-project list the designer keeps (mission.cfg `object prebuild`, CConstructor_LoadPrebuildProjects
//   1004e190).
#pragma once

#include "core/math.hpp"
#include "economy/assembly.hpp"
#include "economy/economy.hpp"
#include "game/world.hpp"
#include "ui/hud.hpp"

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace parkan::ui {

// §0: the view states (CViewStateQueue, CGame+0x20).
enum HqViewState : int {
    kStateHero = 0,       // hero on foot
    kStatePilot = 1,      // hero pilots a warbot
    kStateRemote = 2,     // remote control of a unit from the HQ
    kStateHqUnit = 3,     // HQ through a mobile HQ unit
    kStateHqBunker = 4,   // HQ through a bunker (view mode 2)
    kStateInBuilding = 5, // inside an own plant / laboratory: factory or research screen, no sidebar
    kStateTurret = 6,     // manning a tower turret
    kStateMenu = 7,
};
// g_ViewStateTransitions 10103b18 [from · 8 + to]: the pushes the original has a handler for.
bool view_transition_exists(int from, int to);

// §1: CFreeCamera as the HQ camera (mission+0x100). Times in seconds of the game clock.
class HqCamera {
public:
    static constexpr float kSpeed = 125, kRamp = 0.5f;            // HqCam_UpdateMoveSpeeds 100372a0 (80 in the autodemo)
    static constexpr float kTurnRate = 1.5f, kZoomTurnRate = 0.2f; // HqCam_UpdateTurnRates 10037540 (0.5 autodemo)
    static constexpr float kNormalFov = 1.04f, kZoomFov = 0.2f;    // HqCam_Init 10036ed0 0x3f851eb8; zoom 10037bc0
    static constexpr float kGroundClearance = 36, kHeightBand = 200, kAnchorBox = 200; // HqCam_Integrate 10037720
    // HqCam_Init: roll 0, pitch 0, yaw π/2, tilt 1.0, FOV 1.04, distance 30; HqCam_SetPose(x, y, z, 0, 0, π/2).
    void set_pose(Vec3 pos, float yaw = 1.5707964f);
    void anchor(Vec3 building);                        // HqCam_AnchorBuilding 10037f10
    void detach() { anchor_.reset(), follow_pos.reset(); } // HqCam_Detach 10037f40
    // HqCam_FollowUnit 10037ee0: follow a unit (state 3): latches and speeds cleared, distance 0. The controller
    // feeds the unit's position and radius (+0x98) every frame through follow_pos / follow_radius.
    void follow_unit(Vec3 p, float radius);
    std::optional<Vec3> follow_pos;
    float follow_radius = 1, follow_dist = 0; // unit+0x98, +0x38
    void toggle_zoom();                                // JAMES_ZOOM_MODE in view mode 2 (10072120)
    // Key latches (OnUICommandDown / Up for JAMES_HQ_MOVE_*): the ramp clock restarts on every change.
    enum Key { Left, Right, Forward, Back, Up, Down };
    void key(Key k, bool down, double now);
    // HqCam_Update 10037bc0 in view mode 2: edge turning from the virtual pointer (skipped while
    // box-selecting: `edges` false), zoom step, speeds, then HqCam_Integrate. `ground` = landscape height,
    // `map_size` = the unit registry's map size (+8).
    void update(double now, float dt, float mouse_x, float mouse_y, bool edges, const std::function<float(float, float)>& ground,
                float map_size);
    // The placement Rot(roll, π/2 − tilt, yaw): forward = its X column, up = its Z column.
    Vec3 forward() const;
    Vec3 up() const;
    float fov() const { return fov_; }

    Vec3 pos{};
    float yaw = 1.5707964f, tilt = 1.0f;
    bool zoomed = false;

private:
    struct Latch {
        bool on = false;
        double since = 0;
    };
    Latch keys_[6], edge_[4]; // edges: x < 7, x > 633, y < 7, y > 473 (+0x95, +0x94, +0x97, +0x96)
    float strafe_ = 0, fwd_ = 0, vert_ = 0, tilt_rate_ = 0, yaw_rate_ = 0; // +0x50 +0x54 +0x58 +0x5c +0x60
    float fov_ = kNormalFov;
    std::optional<Vec3> anchor_;
};

// iron_3d.ini [TEMP] (LoadStatNormalizeIni 1006fb60): the stats block's percentage scales; the shipped values.
struct HqStatNormalize {
    bool normalize = true;
    float offence_min = 0, offence_max = 6550, defence_min = 0, defence_max = 24400;
};

// What the controller needs from the game.
struct HqHost {
    std::function<std::string(std::uint32_t id)> text;        // iron3d string id → cp1251
    std::function<float(std::uint32_t lid)> health;           // GetHealthPercent 1007eda0: 0..100
    // The assembled design of a unit (economy::design_stats of its scheme; cached by the host): the stats block and
    // the name's debug code (properties 0x201 / 0x207).
    std::function<std::optional<economy::UnitStats>(std::uint32_t lid)> unit_stats;
    std::function<std::optional<economy::UnitStats>(const mission::Scheme& s)> design_stats;
    // Render meshes for the 3D previews: an assembled design, or an objects.rlb part / prototype (the research item's
    // display model). nullptr when they do not load.
    std::function<std::shared_ptr<const RenderMesh>(const mission::Scheme& s)> design_mesh;
    std::function<std::shared_ptr<const RenderMesh>(const std::string& entry)> part_mesh;
    // objects.dlb has a DSCR entry for this display model (the designer's list filter, §3.2).
    std::function<bool(const std::string& model)> described;
    bool full_research_tree = false; // FULL_RESEARCH_TREE (IsFullResearchTreeIni 1008b070): the designer lists all
    // The view-state handler's game side (g_ViewStateTransitions: the controlled unit, the hero's body, the
    // cameras): called for every push and pop before the queue changes, with the unit / building of the record
    // pushed or uncovered; false vetoes it (ComputeWarbotExitPlacement's "exit blocked").
    std::function<bool(int from, int to, std::uint32_t object)> transition;
    std::function<float(std::uint32_t lid)> radius; // unit+0x98 (the HQ camera's follow distance limit)
    AssetLibrary* library = nullptr;  // the designer's assembly (objects.rlb records); no designer without it
    HqStatNormalize stat_norm;
    std::function<void(const std::string& key, std::uint32_t lid)> voice; // QueueUnitVoice (lid kNoObject: 2D)
    std::function<void(const std::string& header, const std::string& body)> pager; // PostComputerPagerText
    std::function<void(const char* sound)> sound;             // UI sounds (BUTTON_CLICK, BAR_OPEN)
    std::function<bool(float x, float y)> passable;           // FUN_10076bc0's ground-cell test [inferred: landscape]
    std::function<void(const std::string& line)> log;
};

// What lies under the pointer (HqUi_ClassifyPointer's picks): an object and the terrain point.
struct HqPick {
    std::uint32_t object = game::kNoObject;
    std::optional<Vec3> ground;
};

// HqUi_ClassifyPointer 1008dec0 action codes (§2.2) and the cursors of UpdateHqCursor 10058be0 (§10).
enum HqAction : int {
    kActNone = 0, kActMove = 1, kActWrong = 2, kActAttack = 3, kActCapture = 4, kActSelect = 7, kActWaypoint = 8,
    kActGuardUnit = 9, kActGuardBuilding = 10, kActRoute = 0xc, kActPlace = 0xd, kActTarget = 0x10, kActOpen = 0x11,
};
enum HqCursor : int {
    kCurArrow = 1, kCurPick = 2, kCurPlace = 3, kCurTarget = 4, kCurGuard = 5, kCurCapture = 6, kCurBox = 7, kCurGhost = 8,
    kCurWrong = 9, kCurUpgrade = 10,
};
int hq_cursor_for(int action);

// g_CStateMode 1010b388 (§2.1).
enum HqMode : int {
    kModeFree = 0, kModeAttackTarget = 1, kModeBuilding = 2, kModeGuardTarget = 3, kModePlaceBuilding = 4, kModeWay = 5,
    kModePlaceMine = 6,
};

// The screen ids (CHqInterface+0x5f0, §3.3).
enum HqScreen : int {
    kScreenNone = 0, kScreenWarbots = 1, kScreenTransports = 2, kScreenBuilders = 3, kScreenResearch = 4, kScreenPlants = 5,
    kScreenTowers = 6, kScreenBunkers = 7, kScreenOther = 8,
};
// The object type mask a list screen shows (HqUi_SetScreen, HqUi_DrawUnitScreen).
std::uint32_t screen_mask(int screen);

// The order menu table 10103f98 (22 × {cmd, unitTypeMask, needsTarget, cstateHint, textId}).
struct HqMenuItem {
    int cmd;
    std::uint32_t mask;
    bool needs_target;
    int hint;
    std::uint32_t text;
};
extern const HqMenuItem kOrderMenu[22];

// A plant record (PlantList, player+0x60, 0x1c bytes; PlantList_Add 100879f0).
struct HqPlant {
    std::uint32_t lid = game::kNoObject; // +0x14
    int project = -1;                    // +4 selected recent project (−1 none: the current production)
    std::string path;                    // +8 the scheme being produced
    bool batch = false;                  // +0x19
};

// A recent project of the designer (CConstructor +0xb674.., 5 × 0x7c, count +0xb8e0).
struct HqProject {
    std::string file; // game-relative scheme path
    mission::Scheme scheme;
};

// The sidebar's twelve buttons (§3.2; LoadHqInterface 10082930): y, icon, tooltip text, screen (0 = special).
struct HqSidebarButton {
    float y;
    const char* icon;
    std::uint32_t tooltip;
    int screen;
};
inline constexpr HqSidebarButton kSidebar[12] = {
    {12, "objpanel_icon_hero", 1612, 0},                  {65, "objpanel_icon_warbots", 1600, kScreenWarbots},
    {94, "objpanel_icon_builder", 1602, kScreenBuilders}, {123, "objpanel_icon_cargo", 1601, kScreenTransports},
    {176, "objpanel_icon_restree", 5080, kScreenResearch}, {229, "objpanel_icon_factory", 1607, kScreenPlants},
    {258, "objpanel_icon_tower", 1614, kScreenTowers},    {287, "objpanel_icon_bunker", 1608, kScreenBunkers},
    {316, "objpanel_icon_build", 1610, kScreenOther},     {369, "objpanel_icon_chat", 5038, 0},
    {399, "objpanel_icon_map", 1501, 0},                  {428, "objpanel_icon_system", 1302, 0},
};
// HqUi_Draw 10083ad0: the item count +0x5ec reveals, in order, hero (1), separator at 41 (2), warbots,
// builders, cargo (3..5), separator 152 (6), lab (7), separator 205 (8), factory, tower, bunker, other
// (9..12), separator 345 (13), chat, map, system (14..16). kSidebarItem[i] = button index, or −1 − separator.
inline constexpr int kSidebarItem[16] = {0, -1, 1, 2, 3, -2, 4, -3, 5, 6, 7, 8, -4, 9, 10, 11};
inline constexpr float kSidebarSeparators[4] = {41, 152, 205, 345};
// Screen titles (HqUi_DrawTitle 10083e00), by screen id; 0x183d otherwise.
inline constexpr std::uint32_t kScreenTitle[9] = {0x183d, 1600, 1601, 1602, 5080, 1607, 1614, 1608, 1610};
// GetOrderTypeName 100773e0: order type → string id.
std::uint32_t order_type_text(std::uint32_t order_type);

struct ModelView {
    std::shared_ptr<const RenderMesh> mesh;
    float angle = 0;
    double last_ms = -1;
};

class Designer;

class HqController {
public:
    HqController(game::World& world, economy::Economy* eco, std::uint32_t clan, HqHost host);
    ~HqController();
    HqController(const HqController&) = delete;
    HqController& operator=(const HqController&) = delete;

    // --- view states (§0) ---
    int state() const { return states_.back().state; }
    std::uint32_t state_object() const { return states_.back().object; }
    // CViewStateQueue::Push 10063060: refused when the table has no handler for top → s.
    bool push_state(int s, std::uint32_t object = game::kNoObject);
    // IsHqCapable 100773a0: the unit's first turret device found, property 0x75 == 1 (its design's turret flag
    // 0x08000000, UnitStats::hq_turret; notes/device-props.md).
    bool hq_capable(std::uint32_t lid) const;
    // OnUnitRemoved 100755f0 (the registry drops a unit whose clan became 0xfffe): the HQ selection without it
    // (1007563b..10075686); the local hero → the HQ selection cleared (FUN_1007d630); the controlled unit (not the
    // hero) → one pop when the top state is 1, 2, 5 or 7 (jump table 10075a84 / 10075a8c; 3, 4, 6: nothing).
    void on_unit_removed(std::uint32_t lid, bool local_hero, bool controlled);
    bool pop_state();   // the top state's handler back to the one below; false when vetoed (or nothing below)
    void pop_to_hero(); // PopToState 10063180 (sidebar hero button)
    int view_mode() const;                                                           // 1 cockpit, 2 HQ camera
    bool hero_controlled() const { return state() == kStateHero; }                   // FUN_10075440(hero, 1)
    // A unit takes the player's input in the view (Unit_SetPlayerControl(unit, 1)): the hero (0), a piloted
    // warbot (1), a remote-controlled unit (2), a manned building turret (6).
    bool unit_controlled() const {
        return state() == kStateHero || state() == kStatePilot || state() == kStateRemote || state() == kStateTurret;
    }
    bool interface_up() const { return state() >= kStateHqUnit && state() <= kStateInBuilding; } // CHud_UpdateHq
    // CMission::OnUnitEntersOwnBuilding 10062ad0 for the hero: SelectBuilding, then plant / institute →
    // state 5 + its screen, bunker → 4, tower → 6 (unless IsTurretDisabled), generator / mine / storage /
    // hangar → the entering unit is selected.
    void hero_enters_building(std::uint32_t hero, std::uint32_t building);

    // --- input (§2.2, §2.6, §3.2, §10); coordinates on the 640×480 virtual screen ---
    void mouse_move(float vx, float vy);
    void lbutton_down(float vx, float vy, const HqPick& pick); // HqInput_OnLButtonDown 10071960
    void lbutton_up(float vx, float vy, const HqPick& pick);   // HqInput_OnLButtonUp 10071ab0
    void rbutton_down();                                       // HqUi_OnRButtonDown 1008fd50
    bool hotkey(char letter);                                  // HqInput_OnHotkey 10071bb0 (W C B R P)
    bool escape();                                             // OnGameKeyDown 10071240 / ROLLBACK_STATE
    void rotate_ghost(int dir);                                // BASE_ROTLEFT / ROTRIGHT: ±0.05 rad
    // Per frame: HqUi_Update 10084040 (sidebar animation, lists), PlantList_Update 100878c0,
    // ResearchUI_PollCompleted 10089e60, HqPending_Tick 10079b50, Placement_Update 100584c0, the strip.
    // `pick` = what is under the pointer now.
    void update(double now_ms, const HqPick& pick);
    // The projection BoxSelectUnits 10076c70 tests (FUN_1007e3b0): world → virtual screen.
    std::function<std::optional<Vec2f>(Vec3)> project;

    // --- selection (§2.1) ---
    void select_unit(std::uint32_t lid, bool exclusive); // SelectUnit 1007d460
    void clear_units();                                  // ClearUnitSelection 1007d630
    void select_building(std::uint32_t lid);             // SelectBuilding 1007df40
    void clear_building();                               // ClearBuildingSelection 1007e0f0
    const std::vector<std::uint32_t>& selected_units() const { return units_; }
    std::uint32_t selected_building() const { return building_; }
    bool selected(std::uint32_t lid) const;
    int cstate() const { return cstate_; } // g_CStateState 1010b384
    int mode() const { return mode_; }
    int classify(const HqPick& pick) const; // HqUi_ClassifyPointer 1008dec0 past the interface tests
    int cursor() const { return cursor_; }

    // --- commands (§2.3–§2.5) ---
    // HqExecuteCommand 10079680 for `units` (cmd ids of §2.4).
    void execute(int cmd, const std::vector<std::uint32_t>& units, std::uint32_t target = game::kNoObject, Vec3 point = {},
                 std::uint32_t param = 0);
    std::vector<int> order_menu() const; // COrderMenu_Fill / Rebuild: the offered cmd ids, sorted, unique
    void menu_execute(int cmd);          // COrderMenu_Execute 1007bb90
    float menu_x() const { return 100; }
    float menu_y() const;                  // 0x76 + 20 · (unit rows)
    int menu_rows(std::size_t count) const; // COrderMenu_SetPos 1007b5f0: min(count, (454 − y) / 21)
    int menu_scroll = 0;
    // The placement ghost (§4), while a build command is pending.
    struct Ghost {
        std::uint32_t type = 0;
        Vec3 pos{};
        float angle = 0;     // g_BuildPlacementAngle
        int validity = 0;    // 0 red (invalid), 1 green, 2 yellow
        bool on_ground = false;
    };
    const std::optional<Ghost>& ghost() const { return ghost_; }

    // --- screens (§3) ---
    int screen() const { return screen_; }
    void set_screen(int s); // HqUi_SetScreen 10085160
    bool sidebar_expanded() const { return items_ >= 16; }
    void expand_now(); // HqUi_ExpandNow 10084000
    int sidebar_items() const { return items_; }
    float lock_y() const { return lock_y_; }
    bool sidebar_enabled(int button) const; // the "enabled when" column of §3.2
    bool sidebar_selected(int button) const;
    std::vector<std::uint32_t> screen_units() const;     // rows of screens 1–3 (FUN_1007dbb0)
    std::vector<std::uint32_t> screen_buildings() const; // rows of screens 5–8 (HqUi_RebuildBuildingRows)
    bool over_interface(float vx, float vy) const;       // HqUi_HitTest 10084f60 (+ the strip)
    // The tooltip text the widget under the pointer registers this frame (its hit test writing HqTooltip_GetHolder
    // 1009bb60; the last writer wins), 0 = none. notes/cursors-tooltips.md §8–§9.
    std::uint32_t tooltip_at(float vx, float vy) const;
    bool map_open = false;                               // the satellite map (sidebar button / JAMES_SATELLITE_MAP)

    // --- research screen (§8) ---
    std::vector<std::int32_t> research_list() const; // RebuildList 1008a3e0
    bool researching(std::int32_t item) const;       // a lab holds a type-0xe order with that param (10088010)
    bool start_research(std::int32_t item);          // StartResearch 100884c0
    void stop_research(std::int32_t item);
    std::int32_t research_selected = -1;
    int research_scroll = 0;
    double research_batch_flash = -1; // the batch button is white for one frame after a press

    // --- factory screen (§9) ---
    const std::vector<HqPlant>& plants() const { return plants_; }
    HqPlant* current_plant();
    const HqPlant* current_plant() const;
    const std::vector<HqProject>& projects() const { return projects_; }
    void add_project(HqProject p); // the recent list: shift down, the new one at 0, at most 5
    bool can_start(const HqPlant& p) const; // 10087110: idle && !batch
    bool select_project(int i);             // SelectProject 10098240
    void issue_build(bool batch);           // PlantRec_IssueBuildOrder 100873b0 through 10098680
    void stop_build(bool batch);            // 100986c0
    std::string bld_path(std::uint32_t plant) const;
    // The design the plant panel previews: the selected recent project, else the plant's current production
    // (CPlantPanel_ShowCurrentProduction 10098590); nullptr when there is none.
    const mission::Scheme* preview_scheme() const;
    std::uint32_t free_brains() const; // CountFreeBrains 100394a0

    // --- resource strip (§7) ---
    int minerals_shown = 0, energy_shown = 0;
    bool strip_blink = false;

    // Draw-side state.
    float mouse_x = 320, mouse_y = 240, box_x0 = 0, box_y0 = 0;
    double now() const { return now_; }
    const HqHost& host() const { return host_; }
    const game::World& world() const { return world_; }
    const economy::Economy* economy() const { return eco_; }
    std::uint32_t clan() const { return clan_; }
    // Control slot 0xa4: a unit's name from FUN_100761a0 ("%s-%d %s": debug code, the clan's running number, type
    // name; the hero 6230, animals 6253), a building's from GetObjectTypeName 10033a90 (type and size). cp1251.
    std::string display_name(std::uint32_t lid) const;
    int hovered_button() const; // sidebar button under the pointer, −1
    bool hover_blink = false;   // the hovered button's 0.1 s blink phase
    std::map<std::uint32_t, int> drive_mode; // unit+0x9c: 0 manual, 1 autogunner, 2 autopilot (absent: 0)
    std::set<std::uint32_t> self_destruct; // armed units (+0x135) [the explosion is not ported]
    HqCamera camera;
    // The 3D previews (CModelView): the plant panel's (115×115 at (236, 23)) and the research details' (127×127 at
    // (226, 18)); their turntable angle survives a change of model.
    mutable ModelView factory_view, research_view;
    // The in-game menu (view state 7, CGameMenu CHud+0xc; notes/game-menu.md): pushed by GAME_MENU or the sidebar,
    // popped by its Return button or Esc; the host pauses the game while it is open (CGame_SetPaused).
    enum class MenuAction { None, Resume, Save, Load, Exit };
    bool menu_open() const { return state() == kStateMenu; }
    bool open_menu() { return push_state(kStateMenu); }
    // CGameMenu_OnButton 10065ea0 on page 0: Return pops the state here; the others are the host's.
    MenuAction menu_click(float vx, float vy);
    // Save / Load enabled (CUiWidget +0x21; off in multiplayer and launch mode 4). [port] Saving is not ported.
    bool menu_save_enabled = false, menu_load_enabled = true;
    // The unit designer (CHud+8 CConstructorShell): open from the plant panel's constructor button; while open it
    // takes the whole screen and every input.
    Designer* designer() const { return designer_.get(); }
    bool designer_open() const;
    void open_designer();     // CConstructorShell_Open 10056090(1, "r", the plant's size class)
    void key_char(char c);    // CConstructorShell_OnChar 100563d0 (the save dialog's name)
    std::vector<std::string> log; // what the controller did (tests, the app's log)

private:
    struct StateRec {
        int state;
        std::uint32_t object;
    };
    bool click_interface(float vx, float vy); // HqUi_Click 10084580
    void click_world(const HqPick& pick);     // HqUi_ClickWorld 100900d0
    void click_unit_panel(float vx, float vy, std::uint32_t lid);
    void click_research(float vx, float vy);  // 1008a0f0
    void click_factory(float vx, float vy);   // 10097fe0
    bool own(std::uint32_t lid) const;
    bool own_alive(std::uint32_t lid) const;
    std::uint32_t type_of(std::uint32_t lid) const;
    std::uint32_t kind_of(std::uint32_t lid) const; // unit +0x30 (the size class)
    bool hero_selected() const;
    bool all_capturers() const;
    void ack_voice(std::uint32_t lid);
    bool give(std::uint32_t lid, const game::UnitOrder& o, int mode);
    void begin_placement(std::uint32_t type);
    void end_placement();
    bool building_available(std::uint32_t mask) const; // BuildAvailable (§2.5)
    bool menu_available(int cmd) const;                // COrderMenu_IsAvailable 1007c000
    void rebuild_plants();
    void say(std::string line);
    void enter_hq(std::uint32_t building); // ViewTrans_HeroToHqBunker 10064140
    void leave_to_hero();                  // ViewTrans_*ToHero 10064200 / 10064950
    void open_unit_screen(int s);

    game::World& world_;
    economy::Economy* eco_;
    std::uint32_t clan_;
    HqHost host_;
    std::vector<StateRec> states_{{kStateHero, game::kNoObject}};
    std::vector<std::uint32_t> units_;
    std::uint32_t building_ = game::kNoObject;
    int cstate_ = 0, mode_ = kModeFree, cursor_ = kCurArrow, screen_ = kScreenNone;
    int items_ = 0;
    float lock_y_ = 12;
    bool expanded_ = false, animating_ = false;
    double items_t_ = 0, now_ = 0, lbutton_since_ = -1, strip_t_ = 0, blink_t_ = 0, hover_t_ = 0;
    bool lbutton_ = false, boxing_ = false;
    // Pending HQ commands (the per-unit block +0xa8, §2.5; the port keeps one record for the selection).
    struct Pending {
        int mode = 0; // 1 route, 3 build, 4 patrol
        std::uint32_t param = 0;
        std::vector<std::uint32_t> units;
        std::vector<Vec3> waypoints;
        std::uint32_t target_unit = game::kNoObject, target_building = game::kNoObject;
        bool route_done = false, placed = false, unit_flag = false, building_flag = false;
    };
    std::optional<Pending> pending_;
    std::optional<Ghost> ghost_;
    std::vector<HqPlant> plants_;
    std::uint32_t plant_ = game::kNoObject; // the factory screen's plant (+0x48)
    std::vector<HqProject> projects_;
    std::unique_ptr<Designer> designer_;
    bool no_cpu_played_ = false;
    // The per-clan unit counters (player record +0x10, mission+0x734 + clan·0x68) and the numbers they gave, in
    // creation order (placed units in world order, then the new ones as they appear) [inferred order].
    std::map<std::uint32_t, std::int32_t> clan_unit_counter_;
    std::map<std::uint32_t, std::int32_t> unit_number_;
    void number_new_units();
    double plants_busy_ms_ = 0;
};

// The HQ sprites by hq.cfg / compaund.cfg name (HudResources::sprites).
struct HqSprites {
    std::map<std::string, Sprite> by_name;
    const Sprite* get(const std::string& name) const {
        auto it = by_name.find(name);
        return it == by_name.end() ? nullptr : &it->second;
    }
};
HqSprites make_hq_sprites(const std::map<std::string, struct HudSprite>& cfg);

// The software cursors (ui/cursor.cfg; CCursor_LoadFromCfg 10057d00): 4 frames of WIDTH×HEIGHT from
// (OFFSET_X + i·WIDTH, OFFSET_Y) of TEXTURE, hotspot EXTENT, PHASE_DELAY ms per frame. notes/cursors-tooltips.md §2.
struct CursorDef {
    Sprite frame0;
    float hot_x = 0, hot_y = 0;
    float phase_s = 0.25f;
};
// By lower-case object name (arrow, pick, place, target, guard, capture, wrong_place, upgrade).
std::map<std::string, CursorDef> parse_cursor_cfg(std::string_view text);
// The cfg object of an HQ cursor mode (1..10; 7 = the arrow; 8 = the placement ghost: none).
const char* cursor_name(int mode);
// CCursor_DrawSprite 10057500: frame `k` at virtual (x − hot, y − hot), WIDTH×HEIGHT virtual px.
void draw_cursor(OverlayFrame& out, const VirtualScreen& vs, const HudTextures& tex, const CursorDef& c, int frame, float vx, float vy);

// HqTooltip_UpdateAndDraw 1009bbc0 / HqTooltip_DrawBox 1009b910: shows `text` after the pointer rested within ±2 px
// for over 250 ms; a pale box below (or left / above at the edges of) the pointer in TOOL_FONT. Pointer in drawable
// pixels; `software_cursor` picks the 16 (else 12) virtual px offset.
struct TooltipState {
    float x = -1e9f, y = -1e9f;
    double since_ms = 0;
};
void draw_tooltip(OverlayFrame& out, const VirtualScreen& vs, HudFont tool_font, TooltipState& st, const std::string& text, float px,
                  float py, double now_ms, bool software_cursor);

// CModelView (1009df30, 200 bytes): a part or design shown on its own in a rect of the HQ screens — a turntable
// about +Z at 0.75 rad/s (Δt_ms · 0.00075 per draw), the camera 2·r away (r = the bounding radius; 1/sin(π/6),
// so a 60° view [inferred horizontal]) at pitch −0.5 rad (CModelView_FitCamera 1009e290).
// The inset for this frame at virtual rect (x, y, w, h), drawn after the quads already in `out`.
void draw_model_view(OverlayFrame& out, const VirtualScreen& vs, ModelView& mv, float x, float y, float w, float h, double now_ms);

// The ray through a virtual-screen point of the HQ camera (the viewer's projection, horizontal fov).
Vec3 hq_pointer_ray(const HqCamera& cam, float vx, float vy, float aspect);

// The whole HQ interface for this frame (HqUi_Draw 10083ad0): the resource strip, the sidebar (not in
// state 5), the current screen and its title, the order menu, the selection box. `objectives` = the
// objectives screen is up (only the strip is drawn).
void draw_hq(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HqController& hq, bool objectives = false);
// CGameMenu_Draw 10065d00: the in-game menu's page 0 over the dimmed world (view state 7).
void draw_game_menu(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HqController& hq);

} // namespace parkan::ui
