// The in-mission HUD: the cockpit pages of iron3d's CHud (radar, target and own-unit panels, status
// lamps, weapon rows, sight) and the satellite map, as overlay quads on the 640×480 virtual screen.
// The game fills a plain HudState every frame; nothing here reads the simulation. Spec: notes/hud.md.
//
// Port decisions:
// - Coordinates are virtual throughout. The original hands some GUI primitives (the sight's north dot,
//   the radar sweep ring) 640-based numbers without scaling them; the port scales them like the rest.
// - GUI lines and outlines are max(1, round(scale)) drawable pixels thick (D3D drew 1-pixel lines).
// - Sprites use the original UV rule (u + 0.5)/256 … (u + w)/256 and are drawn as free quads, so
//   mirrored (x1 < x0) and rotated sprites come out as in CHudSprite_Draw.
#pragma once

#include "ui/hud_text.hpp"

#include <array>
#include <optional>

namespace parkan::ui {

// Overlay texture slots of the HUD pages (ViewerOptions::overlay_textures); −1 = not loaded (skipped).
struct HudTextures {
    std::int32_t page6 = -1, page7 = -1, page9 = -1, ui_menu = -1, ui_menu3 = -1, minimap = -1;
    // The HQ's other pages: the software cursors (new_ui1, cursor.cfg) and the unit designer's (icons, page1/3/4/5).
    std::int32_t new_ui1 = -1, icons = -1, page1 = -1, page3 = -1, page4 = -1, page5 = -1;
};
enum class HudPage { Page6, Page7, Page9, UiMenu, UiMenu3, Minimap, NewUi1, Icons, Page1, Page3, Page4, Page5 };

// A source rectangle on a 256×256 page (CHudSprite_SetSource 1008fa80); rot = 90° clockwise steps.
struct Sprite {
    HudPage page;
    float u, v, w, h;
    int rot = 0;
};

// CHudSprite_Draw 1008fbc0: `s` over virtual (x0, y0)-(x1, y1); x1 < x0 or y1 < y0 mirror it.
void draw_sprite(OverlayFrame& out, const VirtualScreen& vs, const HudTextures& tex, const Sprite& s, float x0, float y0,
                 float x1, float y1, std::uint32_t argb = 0xffffffffu);
// GUI-server primitives in virtual coordinates.
void draw_line(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb);
void draw_rect_outline(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb);
void draw_circle(OverlayFrame& out, const VirtualScreen& vs, float cx, float cy, float r, std::uint32_t argb);

// GetClanRelationColour 100658a0. `kind` = the other clan's record kind (0, 3 special), `relation` = its
// relation towards the local player (0 hostile, 1, 2).
std::uint32_t relation_colour(bool own_clan, int kind, int relation);

// Projects a world point through a camera (the viewer's: horizontal fov, `aspect` = width / height) to
// the virtual screen; nullopt behind the eye.
struct Vec2f {
    float x = 0, y = 0;
};
std::optional<Vec2f> project_to_virtual(const float eye[3], const float fwd[3], const float up[3], float fov_x, float aspect,
                                        const float p[3]);

struct HudWeapon {
    std::string name;       // cp1251
    std::int32_t ammo = -1; // current; −1 = unlimited ("INF")
    std::uint32_t state = 0; // GunState (combat/gun.hpp)
    bool selected = false;
    float energy = 1; // 0..1: the bar (device property 0x62)
    float lock = 0;   // lock-on progress k (0 = no brackets, 1 = locked)
};

enum class BlipKind { Ground, Flyer, Building };
struct HudBlip {
    float dx = 0, dy = 0; // world offset from the own unit (m)
    std::uint32_t argb = 0xffff0000u;
    BlipKind kind = BlipKind::Ground;
    bool selected = false, wingman = false;
};

struct HudUnitPanel {
    std::string name, info; // cp1251
    std::uint32_t info_argb = 0xffc8c8c8u;
    float health = 100; // percent
    float energy = 1;   // 0..1 (unit property 0x73)
    std::optional<std::array<float, 6>> shields; // charges 0..1: front, back, left, right, top, bottom
    float distance = 0; // target panel: metres; 0 = not shown
};

// The target's on-screen box (CTargeter_Draw): the projected centre, the size factor s (half-size 200·s)
// and the time since the target changed (the 1 s fly-in from the screen centre).
struct HudTargetMarker {
    float x = 320, y = 240, size = 0.1f;
    std::uint32_t argb = 0xffff0000u;
    float since_change_s = 10;
};

struct HudMapDot {
    float x = 0, y = 0; // map pixels: 0..256 from the bottom-left corner of the map
    std::uint32_t argb = 0xffffffffu;
};

struct HudState {
    bool cockpit = true; // JAMES_COCKPIT_OFF
    // The rendering camera: its forward vector's world x, y (the sight's north dot, the radar heading)
    // and horizontal field of view.
    float fwd_x = 0, fwd_y = 1, fov = 1.3f;
    bool has_camera = true;
    // Radar (page 0).
    float radar_range = 300;
    std::optional<float> sweep; // 0..1 of the ping period, when the unit has a radar device
    std::vector<HudBlip> blips;
    float altitude = 0, speed_kmh = 0, max_speed_kmh = 1;
    // Status lamps (page 5): 0 off, 1 lit, 2 red.
    std::array<int, 8> lamps{};
    // Weapon rows (page 6).
    std::vector<HudWeapon> weapons;
    bool infinite_ammo = false;      // building turret: every ammo cell says INF
    std::optional<Vec2f> lock_point; // the current target on screen (lock brackets)
    // Target (page 1) and own unit (page 2).
    HudUnitPanel own;
    std::optional<HudUnitPanel> target;
    std::optional<HudTargetMarker> marker;
    // Satellite map.
    bool map_visible = false;
    int map_alpha = 0x80;
    bool map_alpha_label = false;
    std::vector<HudMapDot> map_dots;
};

// iron3d strings the HUD prints (cp1251).
struct HudStrings {
    std::string inf = "INF";         // 5094
    std::string out_of_zone;         // 6250 "ВНЕ ЗОНЫ ЗАХВАТА"
    std::string metres;              // 6178 "м"
};

struct HudContext {
    const VirtualScreen& vs;
    HudFont font; // GAME_FONT
    const HudTextures& tex;
    const HudStrings& strings;
};

// The pages, each as the original draws it (also used by the tests).
void draw_radar(OverlayFrame& out, const HudContext& c, const HudState& s);                // page 0
void draw_unit_panel(OverlayFrame& out, const HudContext& c, const HudUnitPanel& p, int side, // pages 1/2
                     const std::optional<HudTargetMarker>& marker);
void draw_status_lamps(OverlayFrame& out, const HudContext& c, const HudState& s);         // page 5
void draw_weapon_rows(OverlayFrame& out, const HudContext& c, const HudState& s);          // page 6
void draw_sight(OverlayFrame& out, const HudContext& c, const HudState& s);                // page 8
void draw_satellite_map(OverlayFrame& out, const HudContext& c, const HudState& s);
// CHud_Draw's cockpit part: every page in id order, then the satellite map.
void draw_hud(OverlayFrame& out, const HudContext& c, const HudState& s);

// Layout constants tests and the game share.
inline constexpr float kRadarX = 320, kRadarY = 396, kRadarRadius = 60; // CRadarPage +0xc/+0x10; blip scale
inline constexpr float kRowHeight = 19;                                  // weapon rows (CWeaponsPage_Draw)
inline constexpr int kMapAlphaMin = 30, kMapAlphaMax = 255, kMapAlphaStep = 12; // CSatelliteMap_IncAlpha
inline constexpr float kZoomFov = 0.2f, kZoomStep = 0.1f;                // CUnit_Update zoom (rad, per frame)

} // namespace parkan::ui
