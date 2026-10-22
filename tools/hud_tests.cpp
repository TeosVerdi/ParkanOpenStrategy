// In-mission HUD tests (notes/hud.md): the cockpit layout for known states on the 640×480 virtual
// screen, and the HUD's textures, sprite tables and strings on a game folder. Linked into parkan-tests.
#include "core/paths.hpp"
#include "res/library.hpp"
#include "res/textres.hpp"
#include "ui/hud.hpp"
#include "ui/hud_resources.hpp"

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

// Distinct texture slots so a quad's page can be told from its texture index.
HudTextures slots() { return {0, 1, 2, 3, 4, 5}; }

// The free quad drawn with texture `t` whose first u is `u0` (a sprite's source x), or nullptr.
const OverlayQuad* find_sprite(const OverlayFrame& f, std::int32_t t, float u, float v) {
    for (const auto& q : f.quads) {
        if (!q.free || q.texture != t) continue;
        for (int k = 0; k < 4; ++k)
            if (near(q.u[k], (u + 0.5f) / 256.f) && near(q.v[k], (v + 0.5f) / 256.f)) return &q;
    }
    return nullptr;
}
float min_x(const OverlayQuad& q) { return std::min(std::min(q.x[0], q.x[1]), std::min(q.x[2], q.x[3])); }
float max_x(const OverlayQuad& q) { return std::max(std::max(q.x[0], q.x[1]), std::max(q.x[2], q.x[3])); }
float min_y(const OverlayQuad& q) { return std::min(std::min(q.y[0], q.y[1]), std::min(q.y[2], q.y[3])); }
float max_y(const OverlayQuad& q) { return std::max(std::max(q.y[0], q.y[1]), std::max(q.y[2], q.y[3])); }

TftFont fake_font() { // every glyph 5 px wide, spacing 1, height 7
    TftFont f;
    f.height = 7, f.spacing = 1, f.max_width = 5, f.v_height = 0.1f;
    for (auto& g : f.glyphs) g.w = 5, g.u1 = 0.1f;
    return f;
}

void test_sprite_rule() {
    // CHudSprite_SetSource / CHudSprite_Draw: (u + 0.5)/256 … (u + w)/256; x1 < x0 mirrors; rot turns clockwise.
    const VirtualScreen vs{1280, 960, 2};
    const HudTextures t = slots();
    OverlayFrame f;
    draw_sprite(f, vs, t, {HudPage::Page9, 10, 20, 30, 40}, 100, 50, 70, 90);
    const OverlayQuad& q = f.quads.at(0);
    CHECK(q.free && q.texture == 2);
    CHECK(near(q.x[0], 200) && near(q.x[1], 140) && near(q.y[0], 100) && near(q.y[2], 180)); // ×2, mirrored in x
    CHECK(near(q.u[0], 10.5f / 256) && near(q.u[1], 40.f / 256) && near(q.v[0], 20.5f / 256) && near(q.v[2], 60.f / 256));
    f.quads.clear();
    draw_sprite(f, vs, t, {HudPage::Page9, 0, 0, 8, 8, 1}, 0, 0, 8, 8); // rot 1: the texture's TL lands at TR
    const OverlayQuad& r = f.quads.at(0);
    CHECK(near(r.u[1], 0.5f / 256) && near(r.v[1], 0.5f / 256)); // TR corner shows texel (0,0)
    CHECK(near(r.u[0], 0.5f / 256) && near(r.v[0], 8.f / 256));   // TL shows the bottom-left texel
}

void test_sight() {
    const VirtualScreen vs{640, 480, 1};
    const HudTextures t = slots();
    const HudStrings strings;
    const HudContext c{vs, {}, t, strings};
    HudState s;
    s.fwd_x = 0, s.fwd_y = 1; // facing north
    OverlayFrame f;
    draw_sight(f, c, s);
    const OverlayQuad* circle = find_sprite(f, 2, 77, 0);
    CHECK(circle && near(min_x(*circle), 288) && near(max_x(*circle), 352) && near(min_y(*circle), 208) && near(max_y(*circle), 272));
    CHECK(circle && circle->rgba[0] == 0x37 && circle->rgba[1] == 0xff && circle->rgba[2] == 0x37);
    // The north dot: 24 px above the centre when facing +Y, 2×2.
    const OverlayQuad& dot = f.quads.back();
    CHECK(!dot.free && near(dot.x0, 319) && near(dot.x1, 321) && near(dot.y0, 215) && near(dot.y1, 217));
    f.quads.clear();
    s.fwd_x = 1, s.fwd_y = 0; // facing east: north is to the left
    draw_sight(f, c, s);
    CHECK(near(f.quads.back().x0, 295) && near(f.quads.back().y0, 239));
}

void test_radar() {
    const VirtualScreen vs{640, 480, 1};
    const HudTextures t = slots();
    const HudStrings strings;
    const TftFont font = fake_font();
    const HudContext c{vs, {&font, 9}, t, strings};
    HudState s;
    s.radar_range = 300, s.fwd_x = 0, s.fwd_y = 1;
    s.blips.push_back({0, 150, 0xffff0000u, BlipKind::Ground}); // straight ahead at half range
    OverlayFrame f;
    draw_radar(f, c, s);
    // The dish halves meet at x = 320.
    const OverlayQuad* left = find_sprite(f, 0, 156, 0);
    CHECK(left && near(min_x(*left), 221) && near(max_x(*left), 320) && near(min_y(*left), 327) && near(max_y(*left), 480));
    // The blip: 30 px above the centre (60 px = the range), a 3×3 fill.
    bool found = false;
    for (const auto& q : f.quads)
        if (!q.free && q.texture < 0 && q.rgba[0] == 0xff && q.rgba[1] == 0 && near(q.x0, 319) && near(q.y0, 365) && near(q.x1, 322))
            found = true;
    CHECK(found);
    // The blue chevron sits at the top of the ring when facing north.
    const OverlayQuad* chevron = nullptr;
    for (const auto& q : f.quads)
        if (q.free && q.texture == 0 && q.rgba[2] == 0xe1 && q.rgba[0] == 0x0a) chevron = &q;
    CHECK(chevron && near((chevron->x[0] + chevron->x[1]) / 2, 320, 0.01f) && near((chevron->y[0] + chevron->y[1]) / 2, 396 - 68 * std::cos(0.0873f), 0.01f));
    // The range under the dish: "300" (18 px) centred in 37 px from x 303 → 313, y 467, green.
    bool text = false;
    for (const auto& q : f.quads)
        if (q.texture == 9 && near(q.x0, 313) && near(q.y0, 467) && q.rgba[1] == 0xff && q.rgba[0] == 0) text = true;
    CHECK(text);
    // Facing east, a unit to the north shows on the left at the same distance.
    f.quads.clear();
    s.fwd_x = 1, s.fwd_y = 0;
    draw_radar(f, c, s);
    found = false;
    for (const auto& q : f.quads)
        if (!q.free && q.texture < 0 && q.rgba[0] == 0xff && q.rgba[1] == 0 && near(q.x0, 289) && near(q.y0, 395)) found = true;
    CHECK(found);
    // Altitude and speed arcs: 0 m → half full (h = 52), the speed arc empty at 0 km/h.
    CHECK(find_sprite(f, 0, 121, 52) != nullptr);
    CHECK(find_sprite(f, 0, 121, 105) != nullptr);
}

void test_weapon_rows() {
    const VirtualScreen vs{640, 480, 1};
    const HudTextures t = slots();
    const HudStrings strings;
    const HudContext c{vs, {}, t, strings};
    HudState s;
    s.weapons.push_back({"A", 120, 0, true, 1.f, 0});  // ready, selected: green lamp
    s.weapons.push_back({"B", -1, 0, false, 0.5f, 0}); // not selected: black lamp
    OverlayFrame f;
    draw_weapon_rows(f, c, s);
    // Row 0: ending 640→635, number 635→623, lamp 623→604 drawn mirrored over (623,0)-(603,19).
    const OverlayQuad* green = find_sprite(f, 3, 60, 50);
    CHECK(green && near(green->x[0], 623) && near(green->x[1], 603) && near(min_y(*green), 0) && near(max_y(*green), 19));
    const OverlayQuad* black = find_sprite(f, 3, 80, 50);
    CHECK(black && near(min_y(*black), 19) && near(max_y(*black), 38));
    // The ray body is stretched to 120 from x 565, the row ends at 439.
    const OverlayQuad* body = find_sprite(f, 3, 123, 50);
    CHECK(body && near(body->x[0], 565) && near(body->x[1], 444));
    const OverlayQuad* end = find_sprite(f, 3, 216, 50);
    CHECK(end && near(end->x[0], 445) && near(end->x[1], 438));
    // The energy fill: 100 % → 120 px of green at 50 % alpha, 2 px in from 565.
    bool fill = false;
    for (const auto& q : f.quads)
        if (!q.free && q.texture < 0 && q.rgba[3] == 0x80 && q.rgba[1] == 0x80 && near(q.x1, 563) && near(q.x0, 445)) fill = true;
    CHECK(fill);
    // The satellite map hides the rows.
    f.quads.clear();
    s.map_visible = true;
    draw_weapon_rows(f, c, s);
    CHECK(f.quads.empty());
    // Lock brackets: k = 1 frames the target at ±20 px; k = 0 puts them in the screen corners.
    s.map_visible = true;
    s.lock_point = Vec2f{320, 240};
    s.weapons[0].lock = 1;
    draw_weapon_rows(f, c, s);
    const OverlayQuad* tl = find_sprite(f, 2, 0, 32);
    CHECK(tl && near(min_x(*tl), 300) && near(min_y(*tl), 220) && near(max_x(*tl), 309));
    CHECK(tl && tl->rgba[1] == 0xff && tl->rgba[0] == 0x14);
    f.quads.clear();
    s.weapons[0].lock = 0.001f;
    draw_weapon_rows(f, c, s);
    tl = find_sprite(f, 2, 0, 32);
    CHECK(tl && near(min_x(*tl), 30, 1) && near(min_y(*tl), 30, 1));
}

void test_panels_and_lamps() {
    const VirtualScreen vs{640, 480, 1};
    const HudTextures t = slots();
    const HudStrings strings;
    const HudContext c{vs, {}, t, strings};
    HudUnitPanel own;
    own.health = 50;
    OverlayFrame f;
    draw_unit_panel(f, c, own, 1, std::nullopt);
    // Health 50 % → 47 px empty: ui_menu3 (151, 129, 40, 47) over (491, 361)-(531, 408), colour 0xff19ffaf.
    const OverlayQuad* health = find_sprite(f, 4, 151, 82 + 47);
    CHECK(health && near(health->x[0], 491) && near(health->x[1], 531) && near(min_y(*health), 361) && near(max_y(*health), 408));
    CHECK(health && health->rgba[0] == 0x19 && health->rgba[2] == 0xaf);
    const OverlayQuad* back = find_sprite(f, 4, 0, 82);
    CHECK(back && near(back->x[0], 640) && near(back->x[1], 490)); // mirrored on the right
    // Status lamps: frame per state at x 222…402, y 459.
    HudState s;
    s.lamps = {1, 0, 0, 2, 0, 0, 0, 0};
    f.quads.clear();
    draw_status_lamps(f, c, s);
    const OverlayQuad* lit = find_sprite(f, 0, 220, 154);
    CHECK(lit && near(min_x(*lit), 222) && near(min_y(*lit), 459) && near(max_x(*lit), 240));
    const OverlayQuad* red = find_sprite(f, 0, 238, 154);
    CHECK(red && near(min_x(*red), 280));
    const OverlayQuad* icon = find_sprite(f, 3, 223, 142); // lamp 3's icon, red
    CHECK(icon && icon->rgba[0] == 0xff && icon->rgba[1] == 0 && near(min_x(*icon), 281));
}

void test_map_and_misc() {
    const VirtualScreen vs{640, 480, 1};
    const HudTextures t = slots();
    const HudStrings strings;
    const HudContext c{vs, {}, t, strings};
    HudState s;
    s.map_visible = true, s.map_alpha = 0x80;
    OverlayFrame f;
    draw_satellite_map(f, c, s);
    const OverlayQuad* map = find_sprite(f, 5, 0, 0);
    CHECK(map && near(min_x(*map), 379) && near(max_x(*map), 635) && near(min_y(*map), 5) && near(max_y(*map), 261));
    CHECK(map && map->rgba[3] == 0x80 && map->rgba[0] == 0x37);
    CHECK(relation_colour(true, 1, 0) == 0xff8080ffu && relation_colour(false, 1, 0) == 0xffff0000u);
    CHECK(relation_colour(false, 0, 0) == 0xffffff00u && relation_colour(false, 3, 2) == 0xffa0a0a0u);
    // The viewer's projection: the axis lands on the centre, the horizontal fov edge on x = 640.
    const float eye[3] = {0, 0, 0}, fwd[3] = {0, 1, 0}, up[3] = {0, 0, 1};
    const float ahead[3] = {0, 10, 0}, edge[3] = {10 * std::tan(0.65f), 10, 0}, behind[3] = {0, -5, 0};
    const auto a = project_to_virtual(eye, fwd, up, 1.3f, 4.f / 3, ahead), b = project_to_virtual(eye, fwd, up, 1.3f, 4.f / 3, edge);
    CHECK(a && near(a->x, 320) && near(a->y, 240));
    CHECK(b && near(b->x, 640, 0.01f));
    CHECK(!project_to_virtual(eye, fwd, up, 1.3f, 4.f / 3, behind));
    const auto sprites = parse_sprite_cfg("object a_b\n TEXTURE = \"ui_menu\"\n OFFSET_X = 3\n offset_y = 4\n width = 5\n height = 6\n"
                                          " rotate = 270\nend\n## object c\n");
    CHECK(sprites.size() == 1 && sprites.count("a_b") && sprites.at("a_b").texture == "ui_menu" && sprites.at("a_b").rot == 3 &&
          sprites.at("a_b").h == 6);
}

void test_corpus_resources(const std::filesystem::path& game) {
    AssetLibrary lib(game);
    const HudResources r = load_hud_resources(lib);
    CHECK(r.pages.size() == std::size(kHudPages));
    for (const auto& [name, t] : r.pages) CHECK(t.width == 256 && t.height == 256 && t.format == 4444 && t.mips_rgba.size() == 1);
    // The named sprites the cockpit hardcodes (hud.cpp) against hq.cfg / compaund.cfg.
    struct Expect {
        const char* name;
        const char* texture;
        int x, y, w, h, rot;
    };
    static constexpr Expect kExpect[] = {
        {"targeter_back", "ui_menu3", 0, 82, 150, 174, 0}, {"targeter_range", "ui_menu3", 0, 51, 39, 16, 0},
        {"targeter_life", "ui_menu", 49, 94, 15, 15, 0},   {"targeter_energy", "ui_menu", 113, 126, 15, 15, 0},
        {"left_shld", "ui_menu3", 192, 82, 26, 79, 0},     {"frwd_shld", "ui_menu3", 185, 59, 61, 19, 0},
        {"back_shld", "ui_menu3", 151, 194, 89, 29, 0},    {"top_shld", "ui_menu3", 115, 59, 69, 19, 0},
        {"bott_shld", "ui_menu3", 150, 224, 105, 31, 0},   {"map_compass_icon", "page6", 132, 106, 21, 42, 0},
        {"ccres_red_lamp", "ui_menu", 0, 50, 19, 19, 0},   {"ccres_green_lamp", "ui_menu", 60, 50, 19, 19, 0},
        {"ccres_black_lamp", "ui_menu", 80, 50, 19, 19, 0}, {"ccres_ray_body", "ui_menu", 123, 50, 93, 19, 0},
        {"ccres_ray_ending", "ui_menu", 216, 50, 6, 19, 0}, {"ccres_ray_emitter_off", "ui_menu", 223, 50, 10, 19, 0},
        {"ccres_body_text", "ui_menu", 100, 50, 22, 19, 0}, {"ccres_ending_text", "ui_menu", 172, 162, 5, 19, 0},
        {"ccres_separator_left_text", "ui_menu", 232, 162, 5, 19, 0}, {"ccres_frame_corner_3", "ui_menu", 1, 238, 8, 8, 2},
        {"ccres_frame_edge_v", "ui_menu", 11, 238, 47, 5, 1},
    };
    for (const Expect& e : kExpect) {
        const auto it = r.sprites.find(e.name);
        const bool ok = it != r.sprites.end() && it->second.texture == e.texture && it->second.x == e.x && it->second.y == e.y &&
                        it->second.w == e.w && it->second.h == e.h && it->second.rot == e.rot;
        if (!ok) std::printf("  sprite %s differs\n", e.name);
        CHECK(ok);
    }
    // Every texture a sprite names is a loaded page or another ui.lib page.
    const auto cfg = resolve_path(game, "UI/game_resources.cfg");
    CHECK(cfg.has_value());
    if (cfg) {
        const Bytes b = read_file(*cfg);
        const TexturePageTable pages = parse_texture_pages({reinterpret_cast<const char*>(b.data()), b.size()});
        for (const auto& [name, sp] : r.sprites) CHECK(pages.index.count(sp.texture));
    }
    // Every mission's satellite map loads (mission.cfg `object minimap` → ui\minimap.lib).
    int maps = 0, missions = 0;
    if (auto root = resolve_path(game, "MISSIONS"))
        for (const auto& e : std::filesystem::recursive_directory_iterator(*root)) {
            if (!e.is_regular_file() || e.path().filename() != "mission.cfg") continue;
            ++missions;
            try {
                if (auto m = load_minimap(lib, e.path().parent_path()); m && m->width == 256 && m->height == 256) ++maps;
                else std::printf("  no minimap for %s\n", e.path().parent_path().string().c_str());
            } catch (const std::exception& ex) {
                std::printf("  minimap %s: %s\n", e.path().parent_path().string().c_str(), ex.what());
            }
        }
    CHECK(missions > 0 && maps == missions);
    // The HUD's iron3d strings (both languages carry the same ids).
    if (auto p = resolve_path(game, "iron3d.dll")) {
        const TextRes s = TextRes::from_pe(read_file(*p));
        for (std::uint32_t id : {3071u, 3072u, 3073u, 3074u, 5094u, 6178u, 6250u, 6255u}) CHECK(!s.get(id).empty());
        CHECK(s.get(5094) == "INF");
    } else {
        CHECK(!"iron3d.dll missing");
    }
    std::printf("  %zu pages, %zu sprites, %d/%d mission maps\n", r.pages.size(), r.sprites.size(), maps, missions);
}

} // namespace

int hud_unit_tests() {
    g_fail = 0;
    test_sprite_rule();
    test_sight();
    test_radar();
    test_weapon_rows();
    test_panels_and_lamps();
    test_map_and_misc();
    return g_fail;
}

int hud_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("hud\n");
    test_corpus_resources(game);
    return g_fail;
}
