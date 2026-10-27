// HQ interface tests (notes/hq-ui.md, src/ui/hq*): the view-state table and transitions, the HQ camera's
// integration, selection and the HqUi_ClassifyPointer / HqUi_ClickWorld orders on a hand-made world, the
// sidebar and screen layout as drawn quads, and on a game folder the HQ sprite tables, strings and the
// prebuild projects. Linked into parkan-tests.
#include "core/paths.hpp"
#include "economy/economy.hpp"
#include "game/mission_runtime.hpp"
#include "res/textres.hpp"
#include "mission/research_tree.hpp"
#include "ui/designer.hpp"
#include "ui/hq.hpp"
#include "ui/hud_resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace parkan;
using namespace parkan::ui;
using game::kNoObject;

namespace {

// Esc in the shipped bindings: the HQ part of OnGameKeyDown, else ROLLBACK_STATE (a vetoed pop still takes the key).
bool esc_key(parkan::ui::HqController& hq) {
    if (!hq.escape()) hq.pop_state();
    return true;
}

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

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

// A small base: the player's bunker (clan 0) at (500, 500), a plant, two warbots (size 4 and 2), a builder,
// an enemy warbot and an enemy bunker.
struct Base {
    game::World world;
    std::map<std::uint32_t, FakeControl> controls;
    std::vector<std::string> voices;
    Base() {
        auto add = [&](std::uint32_t lid, std::uint32_t clan, std::uint32_t type, Vec3 p, std::uint32_t size = 0) {
            game::WorldObject o;
            o.logic_id = lid, o.clan = clan, o.type = type, o.placed = p, o.size_class = size;
            world.add(o);
            if (!(lid & game::kBuildingBit)) {
                controls[lid].pos = p;
                world.attach(lid, &controls[lid]);
            }
        };
        add(0x80000001, 0, game::kTypeBunkerMedium, {500, 500, 10});
        add(0x80000002, 0, game::kTypePlant, {600, 500, 10});
        add(0x10, 0, game::kTypeHero, {500, 500, 10});
        add(0x11, 0, game::kTypeWarbot, {520, 560, 10}, 4);
        add(0x12, 0, game::kTypeWarbot, {540, 560, 10}, 2);
        add(0x13, 0, game::kTypeBuilder, {560, 560, 10}, 2);
        add(0x20, 1, game::kTypeWarbot, {700, 700, 10}, 3);
        add(0x80000010, 1, game::kTypeBunkerSmall, {800, 800, 10});
    }
    HqHost host() {
        HqHost h;
        h.text = [](std::uint32_t id) { return "T" + std::to_string(id); };
        h.voice = [this](const std::string& k, std::uint32_t) { voices.push_back(k); };
        return h;
    }
};

void test_tables() {
    // g_ViewStateTransitions 10103b18: 0 → 4 / 5 exist, 5 → 4 does not, 4 → 0 and 5 → 0 do.
    CHECK(view_transition_exists(0, 4) && view_transition_exists(0, 5) && view_transition_exists(4, 4));
    CHECK(!view_transition_exists(5, 4) && !view_transition_exists(0, 2) && !view_transition_exists(0, 3));
    CHECK(view_transition_exists(4, 0) && view_transition_exists(5, 0) && view_transition_exists(2, 5));
    // UpdateHqCursor 10058be0.
    CHECK(hq_cursor_for(1) == kCurPlace && hq_cursor_for(3) == kCurTarget && hq_cursor_for(4) == kCurCapture);
    CHECK(hq_cursor_for(7) == kCurPick && hq_cursor_for(9) == kCurGuard && hq_cursor_for(2) == kCurWrong && hq_cursor_for(0x11) == kCurArrow);
    CHECK(order_type_text(2) == 0x1828 && order_type_text(0x18) == 0x1834 && order_type_text(0x42) == 0x1835);
    CHECK(kOrderMenu[0].text == 5000 && kOrderMenu[21].cmd == 23 && kOrderMenu[21].text == 5047 && kOrderMenu[6].mask == 0x1002000);
}

void test_camera() {
    HqCamera cam;
    cam.set_pose({500, 500, 0});
    // Rot(0, π/2 − 1.0, π/2): looking along +y, 0.57 rad below the horizon.
    const Vec3 f = cam.forward();
    CHECK(near(f.x, 0, 1e-5f) && near(f.y, std::cos(0.5707964f)) && near(f.z, -std::sin(0.5707964f)));
    auto flat = [](float, float) { return 10.f; };
    // The first update lifts the camera to ground + 36.
    cam.update(0, 0, 320, 240, true, flat, 2000);
    CHECK(near(cam.pos.z, 46));
    // Forward held: 2·t·S over the first 0.5 s, then S = 125 m/s (Δy summed per 0.01 s step).
    cam.key(HqCamera::Forward, true, 0);
    double t = 0;
    for (int i = 0; i < 100; ++i) t += 0.01, cam.update(t, 0.01f, 320, 240, true, flat, 2000);
    // Σ 250·tᵢ·0.01 over the first 50 steps (the speed at the step's end) + 125·0.5 = 31.875 + 62.5.
    CHECK(near(cam.pos.y - 500, 94.375f, 1e-3f));
    CHECK(near(cam.pos.x, 500, 1e-3f));
    cam.key(HqCamera::Forward, false, t);
    const float y_stop = cam.pos.y;
    for (int i = 0; i < 10; ++i) t += 0.01, cam.update(t, 0.01f, 320, 240, true, flat, 2000);
    CHECK(cam.pos.y - y_stop < 2.f); // a released axis decays ×(0.5 − dt) per step
    // The pointer at the left edge turns left (yaw +), at the top tilts toward the horizon (tilt +).
    const float yaw0 = cam.yaw, tilt0 = cam.tilt;
    for (int i = 0; i < 10; ++i) t += 0.01, cam.update(t, 0.01f, 2, 2, true, flat, 2000);
    CHECK(cam.yaw > yaw0 && cam.tilt > tilt0);
    // Anchored: |x − bx| ≤ 200.
    HqCamera a;
    a.set_pose({500, 500, 46});
    a.anchor({500, 500, 10});
    a.key(HqCamera::Right, true, 0); // yaw π/2: right = +x
    t = 0;
    for (int i = 0; i < 400; ++i) t += 0.01, a.update(t, 0.01f, 320, 240, true, flat, 2000);
    CHECK(near(a.pos.x, 700, 1e-3f));
    // Zoom: 0.1 per frame toward 0.2.
    a.toggle_zoom();
    for (int i = 0; i < 3; ++i) a.update(t, 0, 320, 240, true, flat, 2000);
    CHECK(near(a.fov(), 1.04f - 0.3f, 1e-4f));
}

void test_game_menu() {
    // View state 7 (view-states.md §7, game-menu.md): over the hero and over the HQ (view mode 2 kept).
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    CHECK(hq.open_menu() && hq.menu_open() && !hq.interface_up() && !hq.hero_controlled());
    CHECK(!hq.open_menu()); // 7 → 7 has no handler
    CHECK(hq.menu_click(320, 247) == HqController::MenuAction::None && hq.menu_open()); // Save disabled
    CHECK(hq.menu_click(320, 222) == HqController::MenuAction::Resume && hq.state() == kStateHero);
    hq.hero_enters_building(0x10, 0x80000001);
    hq.camera.pos.x = 777;
    CHECK(hq.open_menu() && hq.view_mode() == 2);
    CHECK(hq.menu_click(320, 272) == HqController::MenuAction::Load && hq.menu_click(320, 297) == HqController::MenuAction::Exit);
    CHECK(esc_key(hq) && hq.state() == kStateHqBunker && near(hq.camera.pos.x, 777)); // the HQ view continues
}

void test_pilot_state() {
    // View state 1 (view-states.md §3): the game side decides (transition), the warbot becomes the selection,
    // the exit can be vetoed (no exit point) — also under PopToState.
    Base b;
    HqHost h = b.host();
    std::vector<std::pair<int, int>> calls;
    bool allow_exit = false;
    h.transition = [&](int from, int to, std::uint32_t) {
        calls.push_back({from, to});
        return to != kStateHero || allow_exit;
    };
    HqController hq(b.world, nullptr, 0, h);
    CHECK(hq.push_state(kStatePilot, 0x11) && hq.state() == kStatePilot && hq.unit_controlled() && hq.view_mode() == 1);
    CHECK(hq.selected_units().size() == 1 && hq.selected_units()[0] == 0x11);
    CHECK(hq.open_menu() && esc_key(hq) && hq.state() == kStatePilot); // 7 → 1: no game-side call
    CHECK(calls.size() == 1);
    CHECK(esc_key(hq) && hq.state() == kStatePilot); // vetoed
    hq.pop_to_hero();                                // vetoed, no endless loop
    CHECK(hq.state() == kStatePilot);
    allow_exit = true;
    CHECK(esc_key(hq) && hq.state() == kStateHero && hq.selected_units().empty());
    CHECK(calls.size() == 4 && calls.back() == std::make_pair(int(kStatePilot), int(kStateHero)));
}

void test_remote_state() {
    // View state 2 (view-states.md §4): from the bunker HQ; back to it with the camera where it was.
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    hq.hero_enters_building(0x10, 0x80000001);
    hq.camera.pos.x = 640;
    CHECK(hq.push_state(kStateRemote, 0x12) && hq.state() == kStateRemote && hq.view_mode() == 1 && hq.unit_controlled());
    CHECK(!hq.interface_up() && hq.selected_units().size() == 1 && hq.selected_units()[0] == 0x12);
    CHECK(esc_key(hq) && hq.state() == kStateHqBunker && near(hq.camera.pos.x, 640) && hq.selected_units().empty());
    CHECK(!hq.push_state(kStateRemote, 0x12) || hq.state() == kStateRemote);
}

void test_hq_unit_state() {
    // View state 3 (view-states.md §5): HQ through a mobile HQ unit; the HQ camera follows it (HqCam_FollowTarget /
    // FollowStep: d walks to 8 × radius + 1, the camera flies behind the unit at 125 m/s, dead zone 3 m).
    Base b;
    game::WorldObject o;
    o.logic_id = 0x14, o.clan = 0, o.type = game::kTypeHq, o.placed = {700, 500, 10}, o.size_class = 3;
    b.world.add(o);
    b.controls[0x14].pos = o.placed;
    b.world.attach(0x14, &b.controls[0x14]);
    HqHost h = b.host();
    h.radius = [](std::uint32_t) { return 2.f; };
    h.unit_stats = [](std::uint32_t lid) {
        economy::UnitStats st;
        st.hq_turret = lid == 0x14; // device property 0x75
        return std::optional<economy::UnitStats>(st);
    };
    HqController hq(b.world, nullptr, 0, h);
    CHECK(!hq.push_state(kStateHqUnit, 0x12)); // not HQ-capable
    hq.hero_enters_building(0x10, 0x80000001);
    CHECK(hq.push_state(kStateHqUnit, 0x14) && hq.state() == kStateHqUnit && hq.view_mode() == 2 && hq.interface_up());
    CHECK(hq.camera.follow_pos && near(hq.camera.pos.x, 700) && hq.selected_units().size() == 1);
    for (int i = 0; i < 300; ++i) hq.camera.update(i * 33.0, 0.033f, 320, 240, true, [](float, float) { return 0.f; }, 4000);
    CHECK(near(hq.camera.follow_dist, 17, 1.01f));
    const float dd = std::sin(hq.camera.tilt) * hq.camera.follow_dist;
    CHECK(std::fabs(hq.camera.pos.x - (700 - std::cos(hq.camera.yaw) * dd)) <= 3.01f);
    CHECK(!hq.push_state(kStateHqUnit, 0x14) && hq.state() == kStateHqUnit); // already the top: nothing
    CHECK(esc_key(hq) && hq.state() == kStateHqBunker && !hq.camera.follow_pos);
}

void test_turret_state() {
    // View state 6 (view-states.md §6): from the hero and from the bunker HQ; back to the HQ anchored, no new pose.
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    CHECK(hq.push_state(kStateTurret, 0x80000001) && hq.view_mode() == 6 && hq.unit_controlled() && !hq.interface_up());
    CHECK(esc_key(hq) && hq.state() == kStateHero);
    hq.hero_enters_building(0x10, 0x80000001);
    hq.camera.pos.x = 610;
    CHECK(hq.push_state(kStateTurret, 0x80000001) && hq.state() == kStateTurret);
    CHECK(esc_key(hq) && hq.state() == kStateHqBunker && near(hq.camera.pos.x, 610));
}

void test_wingman_menu() {
    // CWingmanMenu (hud-leftovers.md §2.3 / §7): ~ selects all → orders; Shift+~ → pick with 1..9; a digit issues row
    // n − 1; a new cockpit unit resets the menu; no selected wingman left → the order menu closes.
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    const std::vector<std::uint32_t> list{0x11, 0x12, 0x13};
    hq.wingman_sync(0x10, list);
    hq.wingman_command(list, false);
    CHECK(hq.wingman.state == 2 && hq.wingman.selected.size() == 3);
    hq.wingman_key('1', list, 0x10, kNoObject); // Стоять
    CHECK(hq.wingman.state == 0 && hq.wingman.selected.empty());
    CHECK(!b.controls[0x11].orders.empty() && !b.controls[0x13].orders.empty());
    hq.wingman_command(list, true);
    CHECK(hq.wingman.state == 1);
    hq.wingman_key('2', list, 0x10, kNoObject);
    hq.wingman_key('9', list, 0x10, kNoObject); // out of range: ignored
    CHECK(hq.wingman.selected == std::vector<std::uint32_t>{0x12});
    hq.wingman_command(list, false);
    CHECK(hq.wingman.state == 2);
    CHECK(!hq.wingman_order_enabled(4, hq.wingman_units(list), kNoObject)); // attack needs a target
    CHECK(hq.wingman_order_enabled(4, hq.wingman_units(list), 0x20));       // an enemy unit
    hq.wingman_sync(0x10, {0x11});                                          // 0x12 gone → closes
    CHECK(hq.wingman.state == 0);
    hq.wingman_command(list, false);
    hq.wingman_sync(0x11, list); // another cockpit unit: a new menu
    CHECK(hq.wingman.state == 0 && hq.wingman.unit == 0x11);
}

void test_states_and_selection() {
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    CHECK(hq.state() == kStateHero && !hq.interface_up() && hq.sidebar_expanded());
    // The hero walks into the bunker: SelectBuilding + Push(4).
    hq.hero_enters_building(0x10, 0x80000001);
    CHECK(hq.state() == kStateHqBunker && hq.view_mode() == 2 && hq.interface_up() && !hq.hero_controlled());
    CHECK(hq.cstate() == 2 && hq.selected_building() == 0x80000001);
    CHECK(near(hq.camera.pos.x, 500) && near(hq.camera.pos.y, 500) && near(hq.camera.yaw, 1.5707964f));
    // Sidebar: warbots, builders enabled; cargo, lab, towers disabled; bunker and plant enabled.
    CHECK(hq.sidebar_enabled(1) && hq.sidebar_enabled(2) && !hq.sidebar_enabled(3) && !hq.sidebar_enabled(4));
    CHECK(hq.sidebar_enabled(5) && !hq.sidebar_enabled(6) && hq.sidebar_enabled(7) && !hq.sidebar_enabled(9));
    // The warbots button opens screen 1 and selects the first warbot; the order menu of a size-4 warbot has no
    // "find and capture" (kind 1/2 only).
    hq.lbutton_down(20, 75, {});
    CHECK(hq.screen() == kScreenWarbots && hq.selected_units().size() == 1 && hq.selected_units()[0] == 0x11);
    CHECK(hq.screen_units().size() == 2);
    const auto menu = hq.order_menu();
    CHECK((menu == std::vector<int>{0, 1, 3, 6, 7}));
    CHECK(near(hq.menu_y(), 0x76 + 40));
    // A row click selects that unit; now kind 2: the capture order is offered.
    hq.lbutton_down(200, 118 + 20 + 5, {});
    CHECK(hq.selected_units().size() == 1 && hq.selected_units()[0] == 0x12);
    CHECK((hq.order_menu() == std::vector<int>{0, 1, 2, 3, 6, 7}));
    // The first menu row ("Стоять" at y = menu + 20) gives STAYGROUND with insert mode 3.
    hq.lbutton_down(200, hq.menu_y() + 25, {});
    CHECK(!b.controls[0x12].orders.empty() && b.controls[0x12].orders.back().first.type == 0x15 && b.controls[0x12].orders.back().second == 3);
    CHECK(!b.voices.empty() && b.voices.back().ends_with("_S")); // kind 2: the _S acknowledgement set
    // World clicks: ground → GO (1), an enemy unit → ATTACK (3), an enemy building → capture (4) for kind ≤ 2.
    HqPick ground;
    ground.ground = Vec3{650, 650, 10};
    CHECK(hq.classify(ground) == 1);
    hq.lbutton_down(500, 300, ground);
    const auto& go = b.controls[0x12].orders.back().first;
    CHECK(go.type == 2 && go.target_kind == 0x202 && near(go.place.x, 650) && near(go.place.y, 650));
    HqPick enemy{0x20, Vec3{700, 700, 10}};
    CHECK(hq.classify(enemy) == 3);
    hq.lbutton_down(500, 300, enemy);
    CHECK(b.controls[0x12].orders.back().first.type == 3 && b.controls[0x12].orders.back().first.target_id == 0x20);
    HqPick fort{0x80000010, Vec3{800, 800, 10}};
    CHECK(hq.classify(fort) == 4);
    hq.lbutton_down(500, 300, fort);
    CHECK(b.controls[0x12].orders.back().first.type == 5 && b.controls[0x12].orders.back().first.target_id == 0x80000010);
    // An own unit → select (7); the only selected one → open (0x11); an own building → guard (10).
    CHECK(hq.classify({0x11, Vec3{}}) == 7 && hq.classify({0x12, Vec3{}}) == 0x11 && hq.classify({0x80000002, Vec3{}}) == 10);
    // Over the interface (the screen panel, the strip) nothing reaches the world.
    CHECK(hq.over_interface(100, 50) && hq.over_interface(500, 10) && !hq.over_interface(500, 300));
    // Box selection (cursor 7 after 0.35 s of LMB): additive, projected positions.
    hq.project = [](Vec3 p) -> std::optional<Vec2f> { return Vec2f{p.x - 120, p.y - 300}; };
    hq.update(1000, {});
    hq.lbutton_down(390, 240, {});
    hq.update(1400, {});
    hq.mouse_move(460, 280);
    CHECK(hq.cursor() == kCurBox);
    hq.lbutton_up(460, 280, {});
    CHECK(hq.selected_units().size() == 3); // 0x12 kept, 0x11 (400, 260) and 0x13 (440, 260) added; not the hero
    // Right click: clears the unit selection (state 1 in view mode 2) and closes the unit screen.
    hq.rbutton_down();
    CHECK(hq.selected_units().empty() && hq.screen() == kScreenNone);
    // The hero button leaves: PopToState(0).
    hq.lbutton_down(20, 22, {});
    CHECK(hq.state() == kStateHero && !hq.interface_up());
}

void test_plant_state() {
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    // The hero rides the plant's lift: state 5 with the factory screen, no sidebar.
    hq.hero_enters_building(0x10, 0x80000002);
    CHECK(hq.state() == kStateInBuilding && hq.screen() == kScreenPlants && hq.view_mode() == 1);
    CHECK(hq.current_plant() && hq.current_plant()->lid == 0x80000002);
    // A bunker callback in state 5 has no 5 → 4 handler.
    CHECK(!hq.push_state(kStateHqBunker, 0x80000001) && hq.state() == kStateInBuilding);
    // The close box leaves the building.
    hq.lbutton_down(350, 10, {});
    CHECK(hq.state() == kStateHero && hq.screen() == kScreenNone);
    // 0 → 6 has a handler (the game side may veto it through HqHost::transition).
    CHECK(hq.push_state(kStateTurret, 0x80000001) && hq.state() == kStateTurret);
}

void test_placement() {
    // A builder's "Склад" entry (cmd 11): BuildAvailable (no storage owned, the level-0 scheme exists, no
    // research tree here), the ghost follows the terrain point with the placement test's colour, the left
    // click confirms and the builder gets BUILD {7, 0x80000008, 0x206 matrix} with insert mode 3.
    Base b;
    economy::EconomyData data;
    data.build_table.levels[game::kTypeStorage] = {"UNITS\\BUILDS\\STORAGE\\sstore01.dat"};
    economy::SchemeCache sc("");
    mission::Scheme store;
    store.flags = game::kTypeStorage, store.root.lib = "objects.rlb", store.root.entry = "bu_s_store", store.root.one = 1;
    sc.put("UNITS\\BUILDS\\STORAGE\\sstore01.dat", store);
    economy::Economy eco(b.world, data, sc, {});
    HqController hq(b.world, &eco, 0, b.host());
    hq.hero_enters_building(0x10, 0x80000001);
    hq.set_screen(kScreenBuilders);
    CHECK(hq.selected_units().size() == 1 && hq.selected_units()[0] == 0x13);
    const auto menu = hq.order_menu();
    CHECK(std::find(menu.begin(), menu.end(), 11) != menu.end());  // storage
    CHECK(std::find(menu.begin(), menu.end(), 12) == menu.end());  // a plant is owned (and has no scheme here)
    hq.menu_execute(11);
    CHECK(hq.mode() == kModePlaceBuilding && hq.cursor() == kCurGhost && hq.ghost() && hq.ghost()->type == game::kTypeStorage);
    HqPick free_spot;
    free_spot.ground = Vec3{900, 300, 10};
    hq.update(100, free_spot);
    CHECK(hq.ghost()->validity == 1 && near(hq.ghost()->pos.x, 900));
    HqPick on_bunker;
    on_bunker.ground = Vec3{505, 505, 10};
    hq.update(150, on_bunker);
    CHECK(hq.ghost()->validity == 0);
    // An invalid spot does not place; BASE_ROTLEFT turns the ghost 0.05 rad.
    hq.lbutton_down(500, 300, on_bunker);
    CHECK(hq.ghost().has_value());
    hq.rotate_ghost(1);
    hq.update(200, free_spot);
    hq.lbutton_down(500, 300, free_spot);
    hq.update(250, free_spot);
    CHECK(!hq.ghost() && hq.mode() == kModeFree);
    const auto& orders = b.controls[0x13].orders;
    CHECK(!orders.empty() && orders.back().first.type == 7 && orders.back().first.param == game::kTypeStorage &&
          orders.back().first.target_kind == 0x206 && near(orders.back().first.angle, 0.05f) && orders.back().second == 3);
    // Right click during placement cancels with pager 6207.
    std::string pager;
    HqHost h = b.host();
    h.pager = [&](const std::string&, const std::string& body) { pager = body; };
    HqController hq2(b.world, &eco, 0, h);
    hq2.hero_enters_building(0x10, 0x80000001);
    hq2.set_screen(kScreenBuilders);
    hq2.menu_execute(11);
    hq2.rbutton_down();
    CHECK(!hq2.ghost() && hq2.mode() == kModeFree && pager == "T6207");
}

void test_sidebar_animation() {
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    hq.hero_enters_building(0x10, 0x80000001);
    hq.update(0, {});
    // A click on the lock (y 457) while expanded collapses one item per 0.05 s; at 0 the lock moves to 12.
    hq.lbutton_down(20, 460, {});
    for (int i = 1; i <= 17; ++i) hq.update(51.0 * i, {});
    CHECK(hq.sidebar_items() == 0 && near(hq.lock_y(), 12) && !hq.sidebar_expanded());
    // Hovering the lock while collapsed expands it again.
    hq.mouse_move(20, 20);
    for (int i = 18; i <= 36; ++i) hq.update(51.0 * i, {});
    CHECK(hq.sidebar_items() == 16 && near(hq.lock_y(), 457));
}

void test_draw() {
    Base b;
    HqController hq(b.world, nullptr, 0, b.host());
    hq.hero_enters_building(0x10, 0x80000001);
    hq.set_screen(kScreenWarbots);
    hq.update(0, {});
    std::map<std::string, HudSprite> cfg;
    cfg["objpanel_button2"] = {"ui_menu", 47, 182, 46, 29};
    cfg["objpanel_button3"] = {"ui_menu", 94, 182, 46, 29};
    cfg["objpanel_icon_warbots"] = {"ui_menu", 25, 70, 23, 23};
    cfg["ccres_exit_button_pressed"] = {"ui_menu", 74, 142, 35, 19};
    cfg["exit_icon"] = {"ui_menu", 241, 70, 13, 13};
    cfg["ccres_ray_body"] = {"ui_menu", 123, 50, 93, 19};
    const HqSprites sp = make_hq_sprites(cfg);
    const VirtualScreen vs{640, 480, 1};
    const HudTextures tex{0, 1, 2, 3, 4, 5};
    const HudStrings strings;
    const HudContext c{vs, {}, tex, strings};
    OverlayFrame f;
    draw_hq(f, c, sp, hq);
    auto find = [&](float u, float v) -> const OverlayQuad* {
        for (const auto& q : f.quads)
            if (q.free && q.texture == 3 && near(q.u[0], (u + 0.5f) / 256) && near(q.v[0], (v + 0.5f) / 256)) return &q;
        return nullptr;
    };
    // The warbots button is selected (objpanel_button3) at (0, 65)-(46, 94), its icon at (16, 68).
    const OverlayQuad* sel = find(94, 182);
    CHECK(sel && near(sel->x[0], 0) && near(sel->y[0], 65) && near(sel->x[2], 46) && near(sel->y[2], 94));
    const OverlayQuad* icon = find(25, 70);
    CHECK(icon && near(icon->x[0], 16) && near(icon->y[0], 68));
    // The close box after the 278-px title cell: 334..369, its icon at 349, 3.
    const OverlayQuad* exit = find(74, 142);
    CHECK(exit && near(exit->x[0], 334) && near(exit->x[2], 369));
    const OverlayQuad* x = find(241, 70);
    CHECK(x && near(x->x[0], 349) && near(x->y[0], 3));
    // The resource strip's bars: 205 px ending at 640 − 5 − 35 − 5 − 10 = 585, mirrored.
    int bars = 0;
    for (const auto& q : f.quads)
        if (q.free && q.texture == 3 && near(q.u[0], (123 + 0.5f) / 256) && near(q.x[0], 585) && near(q.x[1], 379)) ++bars;
    CHECK(bars == 2);
}

void test_designer_helpers() {
    // OverlayMountNameOnModel 10090a10 / StrPrefixMatchWildcardNoCase 10090b50 (designer.md §3.2, §3.5).
    CHECK(overlay_mount_name("e_tur_tt_01", "e_tur_tb") == "e_tur_tb_01");
    CHECK(prefix_match("R_T_02", "r_t") && prefix_match("r", "r_l") && !prefix_match("r_l_01", "r_t"));
    CHECK(prefix_match("e_gun_?c_01", "e_gun_tc"));
    mission::ResearchItem it;
    it.kind = 9u << 24, it.extra = 0x20;
    CHECK(part_kind(it) == 0);
    it.extra = 0x21, it.kind = (9u << 24) | (4u << 16);
    CHECK(part_kind(it) == 1 && part_type_mask(it) == 0x1004000);
    it.kind = 8u << 24, it.extra = 0x5111;
    CHECK(part_kind(it) == 5);
    it.extra = (2u << 16) | 0x11;
    CHECK(part_type_mask(it) == 0x80020000);
    it.kind = 0xau << 24;
    CHECK(part_kind(it) == 4);
}

// The designer on full.trf: the chassis list of a class-4 plant, a design built by clicks and the shipped
// prebuild scheme loaded back (same tree, same stats as unit-stats.md §9).
void test_designer_corpus(const std::filesystem::path& game) {
    AssetLibrary lib(game);
    auto trf = resolve_path(game, "MISSIONS/SCRIPTS/full.trf");
    economy::SchemeCache sc(game);
    const mission::Scheme* pre = sc.get("UNITS\\UNITS\\PREBLD\\scr_pre1.dat");
    CHECK(trf && pre);
    if (!trf || !pre) return;
    game::World world;
    economy::Economy eco(world, game, nullptr);
    eco.set_research(0, economy::Research(mission::load_research_tree(*trf)));
    HqHost h;
    h.text = [](std::uint32_t id) { return "T" + std::to_string(id); };
    h.library = &lib;
    h.described = [&](const std::string& m) { return lib.find("objects.dlb", m) != nullptr; };
    h.design_stats = [&](const mission::Scheme& s) { return economy::design_stats(lib, s); };
    h.full_research_tree = true;
    HqController hq(world, &eco, 0, h);
    Designer& d = *hq.designer();
    d.open_for_plant(4);
    d.update(0);
    const auto& chassis = d.parts().cats[Designer::kChassis].rows;
    CHECK(!chassis.empty());
    for (const auto& r : chassis) CHECK(r.slot == "r");
    // Chassis → turret → weapon by the model operations.
    CHECK(!d.has_model());
    std::int32_t first = -1;
    for (const auto& r : chassis)
        if (r.item >= 0) {
            first = r.item;
            break;
        }
    d.add_part(first);
    CHECK(d.has_model() && !d.model().cats[Designer::kTurret].rows.empty());
    // The prebuild scheme loaded back gives the same tree and stats.
    d.load_scheme(*pre);
    bool ok = false;
    const auto back = d.build_scheme(&ok);
    CHECK(back.has_value());
    if (!back) return;
    std::function<bool(const mission::SchemeNode&, const mission::SchemeNode&)> same = [&](const auto& a, const auto& b) {
        auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        if (lower(a.entry) != lower(b.entry) || a.children.size() != b.children.size()) return false;
        for (const auto& ca : a.children) {
            bool found = false;
            for (const auto& cb : b.children) found = found || (ca.category == cb.category && same(ca, cb));
            if (!found) return false;
        }
        return true;
    };
    CHECK(same(back->root, pre->root));
    CHECK(back->flags == pre->flags);
    const auto st = economy::design_stats(lib, *back);
    CHECK(st && std::fabs(st->mass - 5556.4f) < 0.5f && std::fabs(st->offence - 600.f) < 0.5f);
    if (st) std::printf("  designer: scr_pre1 reloaded: mass %.1f offence %.1f header %08x\n", st->mass, st->offence, back->flags);
    // Removing the chassis resets the design.
    d.remove_part(Designer::kChassis, 0);
    CHECK(!d.has_model());
}

void test_corpus(const std::filesystem::path& game) {
    AssetLibrary lib(game);
    const HudResources r = load_hud_resources(lib);
    // hq.cfg: the upper-case objects (self_destruction_icon …) and the commented building_placement_icon.
    auto has = [&](const char* n) { return r.sprites.count(n) != 0; };
    CHECK(has("objpanel_lock_icon") && has("self_destruction_icon") && has("batch_research_icon") && has("resbutton_stop"));
    CHECK(!has("building_placement_icon"));
    const HqSprites sp = make_hq_sprites(r.sprites);
    const Sprite* sd = sp.get("self_destruction_icon");
    CHECK(sd && sd->page == HudPage::UiMenu3 && near(sd->u, 84) && near(sd->v, 43) && near(sd->w, 30));
    CHECK(sp.get("ccres_long_button_normal") && near(sp.get("ccres_long_button_normal")->w, 50));
    // The strings the HQ draws (iron3d.dll).
    if (auto p = resolve_path(game, "iron3d.dll")) {
        const TextRes t = TextRes::from_pe(read_file(*p));
        for (std::uint32_t id : {1600u, 1601u, 1602u, 1607u, 1608u, 1610u, 1614u, 5080u, 5084u, 5092u, 5093u, 5000u, 5047u, 5076u, 5077u, 5078u,
                                 5079u, 6207u, 6240u, 6241u, 0x1824u, 0x1835u})
            CHECK(!t.get(id).empty());
    } else {
        CHECK(!"iron3d.dll missing");
    }
    // The placement ghosts' frame objects (GetBuildingFrameName 100339f0) resolve to a mesh.
    for (const char* frame : {"fr_l_mine", "fr_l_store", "fr_l_plant", "fr_l_angar", "fr_l_inst", "fr_l_towL", "fr_l_towH"}) {
        const auto proto = economy::load_prototype(lib, frame);
        CHECK(proto && !proto->mesh.empty());
        if (!proto) std::printf("  ghost %s does not resolve\n", frame);
    }
    // Single.01's prebuild projects exist as schemes (CConstructor_LoadPrebuildProjects).
    economy::SchemeCache sc(game);
    CHECK(sc.get("UNITS\\UNITS\\PREBLD\\scr_pre1.dat") && sc.get("UNITS\\UNITS\\PREBLD\\scr_pre2.dat"));
}

} // namespace

int hq_unit_tests() {
    g_fail = 0;
    test_tables();
    test_camera();
    test_states_and_selection();
    test_plant_state();
    test_placement();
    test_sidebar_animation();
    test_draw();
    test_designer_helpers();
    test_game_menu();
    test_pilot_state();
    test_remote_state();
    test_hq_unit_state();
    test_turret_state();
    test_wingman_menu();
    return g_fail;
}

int hq_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("hq\n");
    test_corpus(game);
    test_designer_corpus(game);
    return g_fail;
}
