// End-of-mission tests (notes/mission-end.md): the end message's layout for each game state, the
// controller's transitions and keys, and campaign progression (dispatcher.ini, next mission) — on
// synthetic data and on a game folder. Linked into parkan-tests.
#include "combat/combat_world.hpp"
#include "core/bytes.hpp"
#include "game/mission_runtime.hpp"
#include "mission/tma.hpp"
#include "res/terrain.hpp"
#include "core/paths.hpp"
#include "res/library.hpp"
#include "res/textres.hpp"
#include "res/tft.hpp"
#include "ui/campaign.hpp"
#include "ui/mission_end.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace parkan;
using namespace parkan::ui;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) < eps; }

TftFont fake_font() { // every glyph 5 px wide, spacing 1 (6 px per character), height 7
    TftFont f;
    f.height = 7, f.spacing = 1, f.max_width = 5, f.v_height = 0.1f;
    for (auto& g : f.glyphs) g.w = 5, g.u1 = 0.1f;
    return f;
}

EndStrings fake_strings() {
    EndStrings s;
    s.complete = "AAAA", s.failed = "FFFF", s.connection_lost = "CCCC", s.leaving = "XXXX";
    s.press_esc = std::string(10, 'E'), s.press_r = std::string(20, 'R'), s.press_l = std::string(14, 'L');
    s.server_warning = std::string(120, 'W');
    return s;
}

void test_layout() {
    const TftFont f = fake_font();
    const EndStrings str = fake_strings();
    const VirtualScreen vs{640, 480, 1};
    // Complete: green title at (320 − 24/2, 50); "Esc" at (320 − 60/2, 75); L = 9; box 30 px around the widest.
    EndLayout l = layout_end_screen(vs, f, f, {GameState::Complete}, str);
    CHECK(l.visible && l.title && l.title->text == "AAAA" && near(l.title->x, 308) && near(l.title->y, 50));
    CHECK(l.title && l.title->argb == kEndTitleGreen);
    CHECK(l.lines.size() == 1 && near(l.lines[0].x, 290) && near(l.lines[0].y, 75) && l.lines[0].argb == 0xfff0f0f0u);
    CHECK(near(l.line_step, 9) && near(l.rect[0], 260) && near(l.rect[1], 35) && near(l.rect[2], 380) && near(l.rect[3], 99));
    CHECK(l.rect_argb == 0x99000000u);
    // Failed: red title, Esc / R / L.
    l = layout_end_screen(vs, f, f, {GameState::Failed}, str);
    CHECK(l.title && l.title->argb == kEndTitleRed && l.title->text == "FFFF");
    CHECK(l.lines.size() == 3 && l.lines[1].text == str.press_r && near(l.lines[1].x, 260) && near(l.lines[2].x, 278));
    CHECK(l.lines.size() == 3 && near(l.lines[1].y, 84) && near(l.lines[2].y, 93));
    CHECK(near(l.rect[0], 230) && near(l.rect[2], 410) && near(l.rect[3], 117));
    // Multiplayer: no R / L; the host gets the red warning and 15 more pixels per line; the box clamps at 0.
    l = layout_end_screen(vs, f, f, {GameState::Failed, true, false}, str);
    CHECK(l.lines.size() == 1 && near(l.line_step, 9));
    l = layout_end_screen(vs, f, f, {GameState::Complete, true, true}, str);
    CHECK(l.lines.size() == 2 && l.lines[1].argb == kEndWarningColour && near(l.line_step, 24) && near(l.lines[1].y, 99));
    CHECK(near(l.lines[1].x, -40) && near(l.rect[0], 0) && near(l.rect[2], 640) && near(l.rect[3], 138));
    // Connection lost: green title and Esc, no multiplayer pruning.
    l = layout_end_screen(vs, f, f, {GameState::ConnectionLost, true, true}, str);
    CHECK(l.title && l.title->text == "CCCC" && l.title->argb == kEndTitleGreen && l.lines.size() == 1);
    // Leaving: the whole screen dimmed, the title only.
    l = layout_end_screen(vs, f, f, {GameState::Leaving}, str);
    CHECK(l.lines.empty() && l.title && near(l.title->x, 308) && near(l.rect[0], 0) && near(l.rect[3], 480));
    CHECK(!layout_end_screen(vs, f, f, {GameState::Running}, str).visible);
    // Retina (the 640 set at glyph scale 2) keeps the virtual layout; 800×600 rounds in screen pixels.
    l = layout_end_screen({1280, 960, 2}, f, f, {GameState::Complete}, str);
    CHECK(l.title && near(l.title->x, 308) && near(l.line_step, 9) && near(l.rect[0], 260));
    l = layout_end_screen({800, 600, 1}, f, f, {GameState::Complete}, str);
    CHECK(l.title && near(l.title->x, 310) && near(l.line_step, 7) && near(l.lines[0].x, 296)); // 24/2.5 → 10, 9/1.25 → 7, 60/2.5 = 24
    // The drawn quads: backdrop first, then 4 title glyphs, then 10 line glyphs.
    OverlayFrame out;
    draw_end_screen(out, vs, {&f, 0}, {&f, 1}, {GameState::Complete}, str);
    CHECK(out.quads.size() == 15 && out.quads[0].texture == -1 && out.quads[0].rgba[3] == 0x99 && out.quads[1].texture == 1 &&
          out.quads[5].texture == 0);
    CHECK(out.quads.size() == 15 && near(out.quads[1].x0, 308) && near(out.quads[1].y0, 50) && out.quads[1].rgba[1] == 0xff &&
          out.quads[1].rgba[0] == 0x64);
}

void test_controller() {
    MissionEndController c;
    CHECK(c.state() == GameState::Running && c.simulating() && c.hud_visible() && !c.message_visible());
    CHECK(c.on_escape() == EndAction::None && c.on_char('r') == EndAction::None); // Esc in play is the game menu's
    CHECK(std::string(c.mission_complete()) == kVoiceMissionComplete && c.won() && c.state() == GameState::Complete);
    CHECK(c.mission_complete() == nullptr); // no second voice
    CHECK(!c.simulating() && !c.hud_visible() && c.message_visible());
    CHECK(c.on_char('r') == EndAction::None && c.on_char('l') == EndAction::None); // only after a failure
    CHECK(c.on_escape() == EndAction::ToShell && c.state() == GameState::Leaving && c.won() && c.pending_action() == EndAction::ToShell);

    MissionEndController h;
    CHECK(std::string(h.hero_killed()) == kVoiceMissionFail && h.state() == GameState::Failed && !h.won());
    CHECK(h.mission_failed() == nullptr);
    CHECK(h.on_char('x') == EndAction::None && h.on_char(0xca) == EndAction::Restart && h.state() == GameState::Leaving);
    MissionEndController l;
    l.mission_failed();
    CHECK(l.on_char('L') == EndAction::LoadGame && l.on_char('R') == EndAction::None); // Leaving: no more chars
    CHECK(l.on_escape() == EndAction::ToShell); // …but Esc still rewrites the launch mode, as in the original

    // The guards let a later opposite result win (the scripts keep running behind the message).
    MissionEndController f;
    f.mission_failed();
    CHECK(f.mission_complete() != nullptr && f.won() && f.state() == GameState::Complete);
    CHECK(f.mission_failed() != nullptr && !f.won());

    MissionEndController b;
    b.briefing_only_finished();
    CHECK(b.won() && b.state() == GameState::Leaving && b.pending_action() == EndAction::ToShell);

    MissionEndController mp(true, true);
    mp.mission_failed();
    CHECK(mp.simulating() && mp.on_char('r') == EndAction::None); // the world runs on; no restart online
    mp.connection_lost();
    CHECK(!mp.simulating() && mp.screen().state == GameState::ConnectionLost);
    MissionEndController demo(false, false, true);
    CHECK(demo.on_escape() == EndAction::ToShell);

    const auto voices = parse_voice_table("object sounds\n library = \"sounds.lib\"\n CHECKBOX = \"i_chk.wav\"\nend\n"
                                          "object voices\n library = \"voices.lib\"\n VOICE_MISSION_FAIL = \"vc_mis_fail.wav\"\nend\n");
    CHECK(voices.size() == 1 && voices.count("VOICE_MISSION_FAIL") && voices.at("VOICE_MISSION_FAIL") == "vc_mis_fail.wav");
}

std::vector<Campaign> fake_tree() {
    std::vector<Campaign> t(3);
    for (int c = 0; c < 3; ++c) {
        t[c].path = "missions/campaign/campaign.0" + std::to_string(c);
        for (int m = 1; m <= (c == 1 ? 1 : 2); ++m) t[c].missions.push_back({t[c].path + "/mission.0" + std::to_string(m) + "/", "", false, false});
    }
    return t;
}

void test_campaign() {
    CHECK(mission_key("missions/campaign/campaign.00/mission.01/") == "missions_campaign_campaign_00_mission_01_");
    // set_mission_complete over the shipped files' shapes.
    CHECK(set_mission_complete("[COMPLETE]", "a/b") == "[COMPLETE]\r\na_b=1\r\n");
    CHECK(set_mission_complete("[COMPLETE]\r\n", "a/b") == "[COMPLETE]\r\na_b=1\r\n");
    CHECK(set_mission_complete("[COMPLETE]\r\na_b=0\r\nc=1\r\n", "a/b") == "[COMPLETE]\r\na_b=1\r\nc=1\r\n");
    CHECK(set_mission_complete("[COMMON]\r\nALL_AVAILABLE=0\r\n[COMPLETE]\r\nc=1\r\n\r\n[X]\r\ny=2\r\n", "a/b") ==
          "[COMMON]\r\nALL_AVAILABLE=0\r\n[COMPLETE]\r\nc=1\r\na_b=1\r\n\r\n[X]\r\ny=2\r\n");
    CHECK(set_mission_complete("[COMMON]\nALL_AVAILABLE=0", "a/b") == "[COMMON]\nALL_AVAILABLE=0\r\n[COMPLETE]\r\na_b=1\r\n");
    CampaignProgress p = parse_dispatcher_ini("[COMMON]\r\nALL_AVAILABLE=0\r\n[COMPLETE]\r\nx=1\r\ny=0\r\n");
    CHECK(!p.all_available && p.complete.size() == 1 && p.complete.count("x"));
    CHECK(parse_dispatcher_ini("[common]\nall_available = 1\n").all_available);

    std::vector<Campaign> t = fake_tree();
    apply_progress(t, {});
    CHECK(t[0].available && !t[0].complete && t[0].missions[0].available && !t[0].missions[1].available && !t[1].available);
    // Completing missions in order unlocks each next one, across campaigns.
    std::string ini = "[COMPLETE]";
    std::optional<std::string> at = t[0].missions[0].path;
    std::vector<std::string> order;
    while (at) {
        order.push_back(*at);
        ini = set_mission_complete(ini, *at);
        t = fake_tree();
        apply_progress(t, parse_dispatcher_ini(ini));
        at = next_mission(t, *at);
    }
    CHECK(order.size() == 5 && order[2] == "missions/campaign/campaign.01/mission.01/" && order[4] == "missions/campaign/campaign.02/mission.02/");
    CHECK(t[2].complete && t[1].complete);
    // A locked successor is not offered; ALL_AVAILABLE opens everything.
    t = fake_tree();
    apply_progress(t, {});
    CHECK(!next_mission(t, t[0].missions[0].path));
    CHECK(!next_mission(t, "missions/single.01/"));
    p = {};
    p.all_available = true;
    apply_progress(t, p);
    CHECK(t[2].available && t[2].missions[1].available && t[2].complete && next_mission(t, t[0].missions[1].path) == t[1].missions[0].path);

    // The shell after a game.
    ShellTarget s = shell_target_after_game("missions/campaign/campaign.03/mission.02/", 1, true);
    CHECK(s.form == ShellTarget::CampaignMissionList && s.campaign == 3 && s.mission == 1);
    CHECK(shell_target_after_game("missions/campaign/campaign.03/mission.02/", 3, true).form == ShellTarget::LoadGame);
    CHECK(shell_target_after_game("missions/campaign/campaign.03/mission.02/", 1, false).form == ShellTarget::LoadGame);
    CHECK(shell_target_after_game("missions/single.02/", 1, true).form == ShellTarget::SingleList);
    CHECK(shell_target_after_game("missions/multi.01/", 1, true).form == ShellTarget::MainMenu);
}

std::optional<TftFont> load_font(AssetLibrary& lib, const char* name) {
    try {
        if (const NResEntry* e = lib.archive("UI/font.lib").find_name(name)) return decode_tft(e->data);
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

void test_corpus_end(const std::filesystem::path& game) {
    AssetLibrary lib(game);
    // The strings, both languages (RU 1049, ENG 1033 under the same ids).
    const auto dll = resolve_path(game, "iron3d.dll");
    CHECK(dll.has_value());
    if (!dll) return;
    const TextRes t = TextRes::from_pe(read_file(*dll));
    const EndStrings str = load_end_strings(t);
    const bool ru = t.get(1012) == "МИССИЯ УСПЕШНО ЗАВЕРШЕНА!";
    CHECK(ru || t.get(1012) == "MISSION COMPLETE !");
    CHECK(ru ? t.get(1013) == "МИССИЯ НЕ ВЫПОЛНЕНА..." : t.get(1013) == "MISSION FAILED...");
    CHECK(ru ? t.get(5082) == "Нажмите Esc для продолжения" : t.get(5082) == "Press 'Esc' to continue");
    CHECK(ru ? t.get(5083) == "Выход..." : t.get(5083) == "Exiting...");
    for (const std::string* s : {&str.connection_lost, &str.press_r, &str.press_l, &str.server_warning}) CHECK(!s->empty());
    CHECK(str.press_r.find("'R'") != std::string::npos && str.press_l.find("'L'") != std::string::npos);
    CHECK(str.complete == utf8_to_cp1251(t.get(1012)));

    // The real fonts at 640×480: centred, inside the box, the box symmetric about x = 320.
    const auto gf = load_font(lib, "gf_640.tft"), mf = load_font(lib, "mf_640.tft");
    CHECK(gf && mf);
    if (gf && mf) {
        const VirtualScreen vs{640, 480, 1};
        for (GameState st : {GameState::Complete, GameState::Failed, GameState::Leaving}) {
            const EndLayout l = layout_end_screen(vs, *gf, *mf, {st}, str);
            CHECK(l.title.has_value());
            if (!l.title) continue;
            const float tw = float(mf->text_width(l.title->text));
            CHECK(near(l.title->x, 320 - std::nearbyint(tw / 2)));
            CHECK(near(l.rect[0] + l.rect[2], 640));
            for (const EndText& e : l.lines) CHECK(e.x >= l.rect[0] && e.x + float(gf->text_width(e.text)) <= l.rect[2] && e.y < l.rect[3]);
            std::printf("  state %d: title x %.0f (w %.0f); %zu lines, L %.0f; box (%.0f, %.0f)-(%.0f, %.0f)\n", int(st), l.title->x, tw,
                        l.lines.size(), l.line_step, l.rect[0], l.rect[1], l.rect[2], l.rect[3]);
        }
        const EndLayout f = layout_end_screen(vs, *gf, *mf, {GameState::Failed}, str);
        CHECK(f.lines.size() == 3 && near(f.line_step, float(gf->height + 2)) && near(f.lines[2].y, 75 + 2 * f.line_step));
    }

    // The end voices resolve through ui/game_resources.cfg into voices.lib.
    if (const auto cfg = resolve_path(game, "UI/game_resources.cfg")) {
        const Bytes b = read_file(*cfg);
        const auto v = parse_voice_table({reinterpret_cast<const char*>(b.data()), b.size()});
        for (const char* key : {kVoiceMissionComplete, kVoiceMissionFail}) {
            const auto it = v.find(key);
            CHECK(it != v.end());
            if (it != v.end()) CHECK(lib.find("voices.lib", it->second) != nullptr);
        }
        CHECK(v.count(kVoiceMissionComplete) && v.at(kVoiceMissionComplete) == "vc_mis_cpl.wav");
    } else {
        CHECK(!"UI/game_resources.cfg missing");
    }
}

// The hero's death (notes/mission-end.md §2.1, weapons-damage.md §5): Tut_2's hero (class 3) shot down by
// a second robot's laser (property 0xb4 = 7: 7 × the bullet's HP punches through its shield). AddPartHP
// (10010f30) gives it clan 0xfffe at the killing blow and nothing more: no removal, part 0 not emptied, the
// other parts keep their HP and stay on (DetachPartDebris skips class 3), it stays in collision. iron3d drops it from its registry in the same frame
// (1007daa0 → OnUnitRemoved): VOICE_UNIT_LOST, then the failure (VOICE_MISSION_FAIL), the world stops.
void test_corpus_hero_death(const std::filesystem::path& game) {
    std::printf("mission end: the hero's death in Tut_2\n");
    AssetLibrary lib(game);
    const std::string mission = "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02";
    const mission::Tma tma = mission::decode_tma(read_file(*resolve_path(*resolve_path(game, mission), "data.tma")));
    std::string map_dir = tma.land_path;
    map_dir.resize(map_dir.find_last_of("\\/"));
    const Terrain terrain = decode_terrain(read_file(*resolve_path(game, map_dir + "/Land.msh")));
    const TerrainGround ground(terrain);
    const mission::TmaObject* hero_o = nullptr;
    for (const auto& o : tma.objects)
        if (o.identity == 1) hero_o = &o;
    CHECK(hero_o);
    if (!hero_o) return;
    auto hero = LiveUnit::load(lib, *hero_o, ground), foe = LiveUnit::load(lib, *hero_o, ground);
    const Mat4 m = hero->frame.matrix;
    Mat4 t2 = m;
    for (int r = 0; r < 3; ++r) t2.at(r, 0) = -m.at(r, 0), t2.at(r, 1) = -m.at(r, 1);
    t2.at(0, 3) += m.at(0, 1) * 40, t2.at(1, 3) += m.at(1, 1) * 40;
    foe->motion->start(0, t2);
    foe->frame = foe->motion->interpolate(0);
    combat::CombatWorld world(lib, ground, nullptr);
    const std::size_t hi = world.add_unit(*hero, 1, 0, true);
    const std::size_t fi = world.add_unit(*foe, 2, 1, false);
    combat::Combatant& h = world.unit(hi);
    combat::Combatant& f = world.unit(fi);
    f.selected.assign(f.guns.size(), false); // the laser only
    f.selected[2] = true;
    f.guns[2].damage_mult = 7;
    f.set_trigger(true);
    MissionEndController end;
    float now = 0, killed = -1, hp_at_kill = 0;
    std::size_t removed = 0, hits_after = 0;
    while (now < 30000) {
        hero->pose_parts();
        foe->pose_parts();
        f.aim_point = h.aim_centre();
        world.update(now += 33);
        for (const auto& e : world.take_events()) {
            if (e.type == combat::CombatEvent::Type::Killed && e.unit == std::int32_t(hi)) {
                killed = now, hp_at_kill = h.damage.total;
                CHECK(std::string(end.hero_killed()) == kVoiceMissionFail);
            }
            if (e.type == combat::CombatEvent::Type::Removed && e.unit == std::int32_t(hi)) ++removed;
            if (killed >= 0 && now > killed && e.type == combat::CombatEvent::Type::Hit && e.unit == std::int32_t(hi)) ++hits_after;
        }
        if (killed >= 0 && now > killed + 10000) break;
    }
    std::size_t with_hp = 0;
    for (const auto& p : h.damage.parts) with_hp += p.present && p.hp > 0;
    std::printf("  killed at %.2f s, %.0f HP left in %zu parts (of %.0f), %zu removals, %zu hits on it later; state %d\n", killed / 1000,
                hp_at_kill, with_hp, h.damage.initial, removed, hits_after, int(end.state()));
    CHECK(killed > 0 && h.damage.clan == combat::kKilledClan && !h.alive() && !h.removed && removed == 0);
    CHECK(!h.damage.removal_at && hp_at_kill > 0 && with_hp > 0 && h.damage.parts[h.damage.root].destroyed());
    for (bool d : hero->detached) CHECK(!d); // nothing thrown off
    CHECK(end.state() == GameState::Failed && !end.simulating());
    // Still in collision: the foe's shots go on hitting it (in the game the world has stopped by then).
    CHECK(hits_after > 0);
    const auto tr = world.trace(foe->position() + Vec3{0, 0, 3}, h.aim_centre(), std::int32_t(fi));
    CHECK(tr && tr->unit == std::int32_t(hi));
    // The voices: VOICE_UNIT_LOST (OnUnitRemoved, own unit) then VOICE_MISSION_FAIL, both in voices.lib.
    if (const auto cfg = resolve_path(game, "UI/game_resources.cfg")) {
        const Bytes b = read_file(*cfg);
        std::map<std::string, std::string> voices;
        for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()}))
            if (o.name == "voices")
                for (const auto& [k, v] : o.entries) voices[k] = v;
        CHECK(voices.count("VOICE_UNIT_LOST") && lib.find("voices.lib", voices["VOICE_UNIT_LOST"]));
    }
}

void test_corpus_campaign(const std::filesystem::path& game) {
    std::vector<Campaign> tree = scan_campaigns(game);
    CHECK(tree.size() == 6);
    static constexpr std::size_t kMissions[] = {4, 4, 4, 4, 2, 2};
    std::size_t total = 0;
    for (std::size_t c = 0; c < tree.size() && c < 6; ++c) {
        CHECK(tree[c].missions.size() == kMissions[c] && !tree[c].title.empty());
        for (const auto& m : tree[c].missions) CHECK(!m.title.empty() && resolve_path(game, m.path + "mission.cfg"));
        total += tree[c].missions.size();
    }
    CHECK(total == 20);
    if (!tree.empty()) CHECK(tree[0].title == utf8_to_cp1251("ТАРА. БАЗА СЛУЖБЫ ИЗЫСКАНИЙ") || tree[0].title == "TARA. THE HOME BASE");
    // The shipped dispatcher.ini: an empty [COMPLETE].
    std::string ini;
    if (auto p = resolve_path(game, "missions/dispatcher.ini")) {
        const Bytes b = read_file(*p);
        ini.assign(reinterpret_cast<const char*>(b.data()), b.size());
    }
    CHECK(ini.find("[COMPLETE]") != std::string::npos);
    CampaignProgress prog = parse_dispatcher_ini(ini);
    CHECK(!prog.all_available && prog.complete.empty());
    std::vector<Campaign> t = tree;
    apply_progress(t, prog);
    int available = 0;
    for (const auto& c : t)
        for (const auto& m : c.missions) available += m.available;
    CHECK(available == 1 && t[0].missions[0].available);
    // Win every mission in order: each win unlocks exactly the next; the epilogue is the last.
    std::optional<std::string> at = t[0].missions[0].path;
    int played = 0;
    while (at && played < 30) {
        ++played;
        const ShellTarget s = shell_target_after_game(*at, 1, true);
        CHECK(s.form == ShellTarget::CampaignMissionList && s.campaign >= 0 && s.mission >= 0);
        if (s.campaign >= 0 && s.mission >= 0 && std::size_t(s.campaign) < t.size() && std::size_t(s.mission) < t[s.campaign].missions.size())
            CHECK(t[s.campaign].missions[s.mission].path == *at);
        ini = set_mission_complete(ini, *at);
        t = tree;
        apply_progress(t, parse_dispatcher_ini(ini));
        at = next_mission(t, *at);
    }
    CHECK(played == 20);
    for (const auto& c : t) CHECK(c.complete);
    std::printf("  %zu campaigns, %zu missions; the epilogue %s\n", tree.size(), total,
                t.empty() || t.back().missions.empty() ? "?" : t.back().missions.back().path.c_str());
}

} // namespace

int end_unit_tests() {
    g_fail = 0;
    test_layout();
    test_controller();
    test_campaign();
    return g_fail;
}

int end_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("mission end\n");
    test_corpus_end(game);
    test_corpus_hero_death(game);
    test_corpus_campaign(game);
    return g_fail;
}
