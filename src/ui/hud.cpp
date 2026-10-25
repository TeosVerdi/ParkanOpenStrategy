#include "ui/hud.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace parkan::ui {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

void set_argb(OverlayQuad& q, std::uint32_t argb) {
    q.rgba[0] = std::uint8_t(argb >> 16), q.rgba[1] = std::uint8_t(argb >> 8), q.rgba[2] = std::uint8_t(argb);
    q.rgba[3] = std::uint8_t(argb >> 24);
}

std::int32_t slot(const HudTextures& t, HudPage p) {
    switch (p) {
    case HudPage::Page6: return t.page6;
    case HudPage::Page7: return t.page7;
    case HudPage::Page9: return t.page9;
    case HudPage::UiMenu: return t.ui_menu;
    case HudPage::UiMenu3: return t.ui_menu3;
    case HudPage::Minimap: return t.minimap;
    case HudPage::NewUi1: return t.new_ui1;
    case HudPage::Icons: return t.icons;
    case HudPage::Page1: return t.page1;
    case HudPage::Page3: return t.page3;
    case HudPage::Page4: return t.page4;
    case HudPage::Page5: return t.page5;
    }
    return -1;
}

// A free quad in drawable pixels; corners TL, TR, BR, BL.
void free_quad(OverlayFrame& out, std::int32_t texture, const float (&x)[4], const float (&y)[4], const float (&u)[4],
               const float (&v)[4], std::uint32_t argb) {
    OverlayQuad q{0, 0, 0, 0};
    q.free = true, q.texture = texture;
    for (int k = 0; k < 4; ++k) q.x[k] = x[k], q.y[k] = y[k], q.u[k] = u[k], q.v[k] = v[k];
    set_argb(q, argb);
    out.quads.push_back(q);
}

float line_px(const VirtualScreen& vs) { return std::max(1.f, std::round(std::min(vs.sx(), vs.sy()))); }

// Text helpers: GAME_FONT at virtual (x, y); widths in virtual pixels (GetTextWidth / ScaleX).
float text_w(const HudContext& c, std::string_view t) { return c.font.font ? text_width_px(c.vs, *c.font.font, t) / c.vs.sx() : 0.f; }
float text_h(const HudContext& c) { return c.font.font ? float(c.font.font->height + 1) * c.vs.glyph_scale / c.vs.sy() : 0.f; }
void text(OverlayFrame& out, const HudContext& c, float x, float y, std::string_view t, std::uint32_t argb) {
    draw_text(out, c.vs, c.font, std::round(x), std::round(y), t, argb);
}
// Centred in a box of width `w` starting at `x0` (the cockpit's "(w − width/ScaleX)·0.5 + x0").
void text_centred(OverlayFrame& out, const HudContext& c, float x0, float w, float y, std::string_view t, std::uint32_t argb) {
    text(out, c, std::round((w - text_w(c, t)) * 0.5f + x0), y, t, argb);
}

void fill(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb) {
    fill_rect(out, vs, std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1), argb);
}

// The compaund.cfg skin sprites the cockpit uses (ui_menu; slot numbers of GetUiSkin, notes/hud.md §1.3).
constexpr Sprite kLamp[5] = {{HudPage::UiMenu, 0, 50, 19, 19},   // 0 red
                             {HudPage::UiMenu, 20, 50, 19, 19},  // 1 yellow1
                             {HudPage::UiMenu, 40, 50, 19, 19},  // 2 yellow2
                             {HudPage::UiMenu, 60, 50, 19, 19},  // 3 green
                             {HudPage::UiMenu, 80, 50, 19, 19}}; // 4 black
constexpr Sprite kRayEmitterOff{HudPage::UiMenu, 223, 50, 10, 19}; // slot 5
constexpr Sprite kRayEnding{HudPage::UiMenu, 216, 50, 6, 19};      // slot 8
constexpr Sprite kRayBody{HudPage::UiMenu, 123, 50, 93, 19};       // slot 9
constexpr Sprite kBodyText{HudPage::UiMenu, 100, 50, 22, 19};      // slot 21
constexpr Sprite kEndingText{HudPage::UiMenu, 172, 162, 5, 19};    // slot 25
constexpr Sprite kSeparatorLeft{HudPage::UiMenu, 232, 162, 5, 19}; // slot 32
constexpr Sprite kFrameCorner{HudPage::UiMenu, 1, 238, 8, 8};      // slots 34..37 (rot 0..3)
constexpr Sprite kFrameEdgeH{HudPage::UiMenu, 11, 238, 47, 5};     // slot 39; slot 38 is the same rotated 90°

// FUN_1009ab90: corners, top/bottom edges, left/right edges (frame style 9/10).
void framed_panel(OverlayFrame& out, const HudContext& c, float x0, float y0, float x1, float y1) {
    for (int r = 0; r < 4; ++r) {
        Sprite s = kFrameCorner;
        s.rot = r;
        const float cx = r == 0 || r == 3 ? x0 : x1 - 8, cy = r < 2 ? y0 : y1 - 8;
        draw_sprite(out, c.vs, c.tex, s, cx, cy, cx + 8, cy + 8);
    }
    draw_sprite(out, c.vs, c.tex, kFrameEdgeH, x0 + 8, y0, x1 - 7, y0 + 5);
    draw_sprite(out, c.vs, c.tex, kFrameEdgeH, x0 + 8, y1, x1 - 7, y1 - 5);
    Sprite v = kFrameEdgeH;
    v.rot = 1;
    draw_sprite(out, c.vs, c.tex, v, x0 + 5, y0 + 8, x0, y1 - 7);
    draw_sprite(out, c.vs, c.tex, v, x1 - 5, y0 + 8, x1, y1 - 7);
}

std::string itoa(float v) { return std::to_string(int(std::lround(v))); }

} // namespace

void draw_sprite(OverlayFrame& out, const VirtualScreen& vs, const HudTextures& tex, const Sprite& s, float x0, float y0,
                 float x1, float y1, std::uint32_t argb) {
    const std::int32_t t = slot(tex, s.page);
    if (t < 0) return;
    const float u0 = (s.u + 0.5f) / 256.f, u1 = (s.u + s.w) / 256.f, v0 = (s.v + 0.5f) / 256.f, v1 = (s.v + s.h) / 256.f;
    // CHudSprite_SetSource: L = [TL, BL, BR, TR]; the screen corners TL, TR, BR, BL take L[r], L[r+3], L[r+2], L[r+1].
    const float lu[4] = {u0, u0, u1, u1}, lv[4] = {v0, v1, v1, v0};
    float u[4], v[4];
    for (int k = 0; k < 4; ++k) {
        static constexpr int kOffset[4] = {0, 3, 2, 1};
        const int i = (s.rot + kOffset[k]) & 3;
        u[k] = lu[i], v[k] = lv[i];
    }
    const float X0 = x0 * vs.sx(), X1 = x1 * vs.sx(), Y0 = y0 * vs.sy(), Y1 = y1 * vs.sy();
    free_quad(out, t, {X0, X1, X1, X0}, {Y0, Y0, Y1, Y1}, u, v, argb);
}

void draw_line(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb) {
    const float X0 = x0 * vs.sx(), Y0 = y0 * vs.sy(), X1 = x1 * vs.sx(), Y1 = y1 * vs.sy();
    float dx = X1 - X0, dy = Y1 - Y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) dx = 1, dy = 0;
    else dx /= len, dy /= len;
    const float h = line_px(vs) * 0.5f, nx = -dy * h, ny = dx * h;
    const float ex = dx * h, ey = dy * h; // extend the ends so outlines close
    free_quad(out, -1, {X0 - ex + nx, X1 + ex + nx, X1 + ex - nx, X0 - ex - nx}, {Y0 - ey + ny, Y1 + ey + ny, Y1 + ey - ny, Y0 - ey - ny},
              {0, 0, 0, 0}, {0, 0, 0, 0}, argb);
}

void draw_rect_outline(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb) {
    draw_line(out, vs, x0, y0, x1, y0, argb);
    draw_line(out, vs, x1, y0, x1, y1, argb);
    draw_line(out, vs, x1, y1, x0, y1, argb);
    draw_line(out, vs, x0, y1, x0, y0, argb);
}

void draw_circle(OverlayFrame& out, const VirtualScreen& vs, float cx, float cy, float r, std::uint32_t argb) {
    if (r <= 0) return;
    constexpr int kSegments = 40;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 2 * kPi * float(i) / kSegments, a1 = 2 * kPi * float(i + 1) / kSegments;
        draw_line(out, vs, cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), argb);
    }
}

std::uint32_t relation_colour(bool own_clan, int kind, int relation) {
    if (own_clan) return 0xff8080ffu;
    if (kind == 0) return 0xffffff00u;
    if (kind == 3) return 0xffa0a0a0u;
    if (relation == 1) return 0xffff00ffu;
    if (relation == 2) return 0xff00ffffu;
    if (relation == 0) return 0xffff0000u;
    return 0xffffff00u;
}

std::optional<Vec2f> project_to_virtual(const float eye[3], const float fwd[3], const float up[3], float fov_x, float aspect,
                                        const float p[3]) {
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float fl = std::sqrt(dot(fwd, fwd));
    if (fl < 1e-6f) return std::nullopt;
    const float f[3] = {fwd[0] / fl, fwd[1] / fl, fwd[2] / fl};
    // The viewer's look_at: s = f × up, u = s × f.
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const float sl = std::sqrt(dot(s, s));
    if (sl < 1e-6f) return std::nullopt;
    for (float& c : s) c /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
    const float z = dot(d, f);
    if (z < 0.1f) return std::nullopt;
    const float t = std::tan(fov_x / 2);
    const float nx = dot(d, s) / z / t, ny = dot(d, u) / z / t * aspect;
    return Vec2f{320 + 320 * nx, 240 - 240 * ny};
}

void draw_radar(OverlayFrame& out, const HudContext& c, const HudState& s) {
    const VirtualScreen& vs = c.vs;
    // The dish halves (page6 155/156,0) and the two side icons.
    draw_sprite(out, vs, c.tex, {HudPage::Page6, 156, 0, 99, 153}, 320, 327, 221, 480);
    draw_sprite(out, vs, c.tex, {HudPage::Page6, 155, 0, 100, 153}, 320, 327, 421, 480);
    draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 215, 0, 15, 28}, 230, 383, 245, 411);
    draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 199, 0, 15, 28}, 396, 383, 411, 411);
    // CRadarPage_DrawAltitude: the number and a hatched arc filling upward over −100..100 m.
    {
        const int alt = int(std::lround(s.altitude));
        const std::string t = std::to_string(alt);
        text_centred(out, c, 226, 28, 446, t, 0xffffffffu);
        const int pct = std::clamp((alt + 100) * 100 / 200, 0, 100), h = (100 - pct) * 105 / 100;
        draw_sprite(out, vs, c.tex, {HudPage::Page6, 121, float(h), 31, float(105 - h)}, 271, float(347 + h), 240, 452);
    }
    // CRadarPage_DrawSpeed: km/h of the unit's speed (property 0x28) against its maximum (0x90).
    {
        const int v = int(std::lround(s.speed_kmh)), vmax = int(std::lround(s.max_speed_kmh));
        text_centred(out, c, 388, 28, 446, std::to_string(v), 0xffffffffu);
        const int pct = std::clamp(vmax != 0 ? v * 100 / vmax : 0, 0, 100), h = (100 - pct) * 105 / 100;
        draw_sprite(out, vs, c.tex, {HudPage::Page6, 121, float(h), 31, float(105 - h)}, 370, float(347 + h), 401, 452, 0xffffb450u);
    }
    // The camera's field of view: a fan textured with page7's glow, always pointing up (view-up radar).
    if (c.tex.page7 >= 0) {
        const float f = s.fov * 0.5f, R = 75;
        auto rim = [&](float a, float a_uv, float& x, float& y, float& u, float& v) {
            x = (kRadarX + R * std::cos(a)) * vs.sx(), y = (kRadarY - R * std::sin(a)) * vs.sy();
            u = (192 + 64 * std::cos(a_uv)) / 256.f, v = (64 - 64 * std::sin(a_uv)) / 256.f;
        };
        float x1, y1, u1, v1, x3, y3, u3, v3;
        rim(kPi / 2 - f, -4.71228f - f, x1, y1, u1, v1);
        rim(kPi / 2 + f, f - 4.71228f, x3, y3, u3, v3);
        const float xc = kRadarX * vs.sx(), yc = kRadarY * vs.sy();
        free_quad(out, c.tex.page7, {x1, xc, x3, x3}, {y1, yc, y3, y3}, {u1, 0.75f, u3, u3}, {v1, 0.25f, v3, v3}, 0xffffffffu);
    }
    const float heading = std::atan2(s.fwd_y, s.fwd_x);
    // North chevrons on the ring 56..68: blue at the bearing of +Y, red opposite.
    if (s.has_camera) {
        const float A = heading - kPi / 2, d = 0.08726647f;
        for (int k = 0; k < 2; ++k) {
            const float a = A + float(k) * kPi;
            auto px = [&](float r, float ang) { return (kRadarX + r * std::sin(ang)) * vs.sx(); };
            auto py = [&](float r, float ang) { return (kRadarY - r * std::cos(ang)) * vs.sy(); };
            const std::int32_t t = c.tex.page6;
            if (t < 0) break;
            const float u0 = (192 + 0.5f) / 256.f, u1 = (192 + 9.f) / 256.f, v0 = (154 + 0.5f) / 256.f, v1 = (154 + 11.f) / 256.f;
            free_quad(out, t, {px(68, a - d), px(68, a + d), px(56, a + d), px(56, a - d)},
                      {py(68, a - d), py(68, a + d), py(56, a + d), py(56, a - d)}, {u0, u1, u1, u0}, {v0, v0, v1, v1},
                      k == 0 ? 0xff0a0ae1u : 0xffe10a0au);
        }
    }
    // The ping: a ring growing to the full radius over the period (0x8c009b00).
    if (s.sweep) draw_circle(out, vs, kRadarX, kRadarY, kRadarRadius * *s.sweep, 0x8c009b00u);
    // Blips, scaled so the radar range is 60 px, rotated so the view points up.
    const float inv_range = s.radar_range > 0 ? 1.f / s.radar_range : 1.f;
    for (const HudBlip& b : s.blips) {
        const float dist = std::sqrt(b.dx * b.dx + b.dy * b.dy), phi = std::atan2(b.dy, b.dx) - heading + kPi / 2;
        const float r = dist * inv_range * kRadarRadius;
        const float x = kRadarX + r * std::cos(phi), y = kRadarY - r * std::sin(phi);
        if (b.kind == BlipKind::Flyer) { // a 4×4 cross
            const float x0 = std::round(x) - 2, y0 = std::round(y) - 2;
            draw_line(out, vs, x0, std::round(y), x0 + 4, std::round(y), b.argb);
            draw_line(out, vs, std::round(x), y0, std::round(x), y0 + 4, b.argb);
            if (b.selected) draw_rect_outline(out, vs, x0 - 2, y0 - 2, x0 + 6, y0 + 6, b.argb);
        } else { // buildings 5×5, ground units 3×3
            const float size = b.kind == BlipKind::Building ? 5.f : 3.f, half = std::floor(size / 2);
            const float x0 = std::round(x - half), y0 = std::round(y - half);
            fill(out, vs, x0, y0, x0 + size, y0 + size, b.argb);
            if (b.selected) draw_rect_outline(out, vs, x0 - 2, y0 - 2, x0 + size + 1, y0 + size + 1, b.argb);
        }
        if (b.wingman) draw_circle(out, vs, std::round(x), std::round(y), 5, 0xffff00ffu);
    }
    text_centred(out, c, 303, 37, 467, itoa(s.radar_range), 0xff00ff00u);
}

void draw_unit_panel(OverlayFrame& out, const HudContext& c, const HudUnitPanel& p, int side, const std::optional<HudTargetMarker>& marker) {
    const VirtualScreen& vs = c.vs;
    const bool own = side != 0;
    // CTargeter_Draw: the back (mirrored on the right), the life and energy icons.
    if (!own) draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 0, 82, 150, 174}, 0, 306, 150, 480);
    else draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 0, 82, 150, 174}, 640, 306, 490, 480);
    const Sprite life{HudPage::UiMenu, 49, 94, 15, 15}, energy{HudPage::UiMenu, 113, 126, 15, 15};
    if (!own) draw_sprite(out, vs, c.tex, life, 129, 411, 144, 426), draw_sprite(out, vs, c.tex, energy, 5, 411, 20, 426);
    else draw_sprite(out, vs, c.tex, life, 496, 411, 511, 426), draw_sprite(out, vs, c.tex, energy, 620, 411, 635, 426);
    // CShieldDisplay_Draw: six sides, red → green by charge.
    if (p.shields) {
        struct Side {
            Sprite s;
            float x0, y0, x1, y1;
        };
        static constexpr Side kSides[6] = {
            {{HudPage::UiMenu3, 185, 59, 61, 19}, 44, 321, 105, 340},   // front (frwd_shld)
            {{HudPage::UiMenu3, 151, 194, 89, 29}, 30, 409, 119, 438},  // back
            {{HudPage::UiMenu3, 192, 82, 26, 79}, 16, 334, 42, 413},    // left
            {{HudPage::UiMenu3, 192, 82, 26, 79}, 133, 334, 107, 413},  // right (left_shld mirrored)
            {{HudPage::UiMenu3, 115, 59, 69, 19}, 40, 308, 109, 327},   // top
            {{HudPage::UiMenu3, 150, 224, 105, 31}, 22, 420, 127, 451}, // bottom
        };
        const float dx = own ? 491.f : 0.f;
        for (int i = 0; i < 6; ++i) {
            const auto c8 = std::uint32_t(std::lround(std::clamp((*p.shields)[std::size_t(i)], 0.f, 1.f) * 255));
            const std::uint32_t argb = 0xff000000u | (255 - c8) << 16 | c8 << 8;
            const Side& sd = kSides[i];
            draw_sprite(out, vs, c.tex, sd.s, sd.x0 + dx, sd.y0, sd.x1 + dx, sd.y1, argb);
        }
    }
    // The target's box on the screen: half-size 200·s, flying in from the centre for 1 s.
    if (!own && marker) {
        const float t = std::clamp(marker->since_change_s, 0.f, 1.f), k = 1 - t;
        const float x = marker->x + (320 - marker->x) * k, y = marker->y + (240 - marker->y) * k;
        const float sz = std::clamp(marker->size, 0.025f, 1.1f), size = (sz + (1.1f - sz) * k) * 200;
        draw_rect_outline(out, vs, x - size, y - size, x + size, y + size, marker->argb);
    }
    // CTargeter_DrawGaugesAndLabels: health (inner arc) and energy (outer arc), 94 px tall.
    auto arc = [&](float pct_f, float x0, float x1, std::uint32_t argb) {
        const int pct = std::clamp(int(pct_f), 0, 100), h = (100 - pct) * 94 / 100;
        draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 151, float(82 + h), 40, float(94 - h)}, x0, float(314 + h), x1, 408, argb);
    };
    if (!own) arc(p.health, 149, 109, 0xff19ffafu), arc(std::lround(p.energy * 100.f), 0, 40, 0xffffb450u);
    else arc(p.health, 491, 531, 0xff19ffafu), arc(std::lround(p.energy * 100.f), 640, 600, 0xffffb450u);
    const float bx = own ? 498.f : 4.f, by = 459;
    text_centred(out, c, bx, 138, by + 1, p.name, 0xffc8c8c8u);
    if (!p.info.empty()) text_centred(out, c, bx, 138, by + 10, p.info, p.info_argb);
    if (!own) {
        draw_sprite(out, vs, c.tex, {HudPage::UiMenu3, 0, 51, 39, 16}, 108, 440, 147, 456);
        if (p.distance > 0) text_centred(out, c, 112, 31, 444, itoa(p.distance) + " " + c.strings.metres, 0xff00ff00u);
    }
}

void draw_status_lamps(OverlayFrame& out, const HudContext& c, const HudState& s) {
    static constexpr float kX[8] = {222, 240, 258, 280, 344, 366, 384, 402};
    static constexpr Sprite kIcon[8] = {
        {HudPage::UiMenu, 97, 126, 15, 15},  {HudPage::UiMenu, 238, 222, 15, 15}, {HudPage::UiMenu, 239, 126, 15, 15},
        {HudPage::UiMenu, 223, 142, 15, 15}, {HudPage::UiMenu3, 180, 27, 15, 15}, {HudPage::UiMenu, 113, 94, 15, 15},
        {HudPage::UiMenu, 113, 110, 15, 15}, {HudPage::UiMenu, 81, 94, 15, 15},
    };
    for (std::size_t i = 0; i < 8; ++i) {
        const int st = std::clamp(s.lamps[i], 0, 2);
        const float x = kX[i];
        draw_sprite(out, c.vs, c.tex, {HudPage::Page6, float(202 + 18 * st), 154, 17, 20}, x, 459, x + 18, 484);
        static constexpr std::uint32_t kColour[3] = {0xff808080u, 0xffffffffu, 0xffff0000u};
        draw_sprite(out, c.vs, c.tex, kIcon[i], x + 1, 463, x + 16, 478, kColour[st]);
    }
}

void draw_weapon_rows(OverlayFrame& out, const HudContext& c, const HudState& s) {
    const VirtualScreen& vs = c.vs;
    float bracket = 0; // CWeaponsPage_Draw's per-frame counter: +4 per bracket set
    for (std::size_t i = 0; i < s.weapons.size(); ++i) {
        const HudWeapon& w = s.weapons[i];
        // Lock-on brackets: four 9×9 corners flying in from the screen corners as k grows to 1.
        if (s.lock_point && w.selected && (w.state == 0 || w.state == 1) && w.lock > 0 && w.ammo != 0) {
            const float k = std::min(w.lock, 1.f), px = s.lock_point->x, py = s.lock_point->y, n = bracket;
            const float L = std::round((px - 50) * k + 30 - n), R = std::round((px - 610) * k + 630 + n);
            const float T = std::round((py - 50) * k + 30 - n), B = std::round((py - 430) * k + 450 + n);
            const auto g = std::uint32_t(std::clamp(0xcd + int(std::lround(k * 50)), 0, 255));
            const std::uint32_t argb = 0xff140014u | g << 8;
            for (int r = 0; r < 4; ++r) {
                const float x0 = r == 0 || r == 3 ? L : R - 9, y0 = r < 2 ? T : B - 9;
                draw_sprite(out, vs, c.tex, {HudPage::Page9, r < 2 ? 0.f : -1.f, r < 2 ? 32.f : 31.f, 9, 9, r}, x0, y0, x0 + 9, y0 + 9, argb);
            }
            bracket += 4;
        }
        if (s.map_visible) continue; // the satellite map covers the rows
        // CWeaponRow_DrawWeapon: cells right to left from x = 640, each mirrored over (x, y)-(x − w − 1, y + 19).
        const float y = kRowHeight * float(i);
        float x = 640;
        auto cell = [&](const Sprite& sp, float wdt) {
            draw_sprite(out, vs, c.tex, sp, x, y, x - wdt - 1, y + kRowHeight);
            x -= wdt;
        };
        const float th = text_h(c), ty = std::round(y + (kRowHeight - th) / 2);
        auto text_cell = [&](float wdt, const std::string& t, std::uint32_t argb) {
            const float right = x;
            cell(kBodyText, wdt);
            text_centred(out, c, right - wdt, wdt, ty, t, argb);
        };
        // The lamp and the name by gun state (jump table 1009d86c); unselected guns are black and grey.
        int lamp = 0;
        bool out_of_zone = false;
        switch (w.state) {
        case 0: lamp = 3; break;
        case 1: case 3: lamp = 2; break;
        case 4: case 6: lamp = 1; break;
        case 2: case 7: case 8: lamp = 2, out_of_zone = true; break;
        default: lamp = 0; break;
        }
        if (!w.selected) lamp = 4;
        const bool active = true; // the wingman menu is not ported: the row background is always "active"
        const std::uint32_t ink = active ? 0xffffffffu : 0xff808080u;
        const std::uint32_t name_ink = !active ? 0xff808080u : (w.selected && out_of_zone) ? 0xffff5c5cu : 0xffffffffu;
        cell(kEndingText, 5);
        text_cell(12, std::to_string(i + 1), ink);
        cell(kLamp[lamp], 19);
        text_cell(24, w.ammo < 0 || s.infinite_ammo ? c.strings.inf : std::to_string(w.ammo), ink);
        cell(kSeparatorLeft, 5);
        cell(kRayEmitterOff, 10);
        // RowCell_Bar: the ray body stretched to 120, the energy fill 2 px in and 3 px off the edges.
        {
            const float right = x, wdt = 120;
            cell(kRayBody, wdt);
            const int pct = std::clamp(int(std::lround(w.energy * 100)), 0, 100);
            if (pct > 0) {
                const std::uint32_t argb = pct < 20 ? 0x80800000u : pct < 80 ? 0x80808000u : 0x80008000u;
                fill(out, vs, right - 2, y + 3, right - float(pct) * wdt * 0.01f, y + kRowHeight - 3, argb);
            }
            const std::string& label = w.selected && out_of_zone && !c.strings.out_of_zone.empty() ? c.strings.out_of_zone : w.name;
            text_centred(out, c, right - wdt, wdt, ty, label, name_ink);
        }
        cell(kRayEnding, 6);
    }
}

void draw_sight(OverlayFrame& out, const HudContext& c, const HudState& s) {
    const VirtualScreen& vs = c.vs;
    constexpr std::uint32_t kGreen = 0xff37ff37u;
    draw_sprite(out, vs, c.tex, {HudPage::Page9, 77, 0, 64, 64}, 288, 208, 352, 272, kGreen);
    const Sprite arc{HudPage::Page9, 54, 28, 23, 23};
    draw_sprite(out, vs, c.tex, arc, 279, 220, 300, 199, kGreen);
    draw_sprite(out, vs, c.tex, arc, 279, 260, 300, 281, kGreen);
    draw_sprite(out, vs, c.tex, arc, 361, 220, 340, 199, kGreen);
    draw_sprite(out, vs, c.tex, arc, 361, 260, 340, 281, kGreen);
    const Sprite ticks{HudPage::Page9, 168, 0, 82, 15};
    draw_sprite(out, vs, c.tex, ticks, 232, 233, 314, 248, kGreen);
    draw_sprite(out, vs, c.tex, ticks, 408, 233, 326, 248, kGreen);
    // The north dot: (320 − 24·fwd.x, 240 − 24·fwd.y), 2×2.
    if (s.has_camera) {
        const float bx = s.fwd_x * 24, by = s.fwd_y * 24;
        fill(out, vs, 320 - std::round(bx - 1), 240 - std::round(by - 1), 320 - std::round(bx + 1), 240 - std::round(by + 1), 0xff00ff00u);
    }
}

int building_map_icon(std::uint32_t type) {
    switch (type) {
    case 0x80000002u: return 0;
    case 0x80000004u: return 1;
    case 0x80000008u: return 2;
    case 0x80000010u: return 3;
    case 0x80010000u: case 0x80020000u: case 0x80040000u: return 4;
    case 0x80000040u: return 5;
    case 0x80000400u: return 7;
    case 0x80100000u: case 0x80200000u: return 8;
    case 0x80000200u: return 9;
    default: return -1;
    }
}

void draw_satellite_map(OverlayFrame& out, const HudContext& c, const HudState& s) {
    if (!s.map_visible) return;
    // CSatelliteMap_DrawPanel: piloting views the panel (374,0)-(640,266); the HQ layout (10073c80) (374,63)-(640,329)
    // inside its outer (374,43)-(640,350) [inferred: the outer rect is the frame]; the map 5 px inside.
    const float x0 = 374, y0 = s.map_hq ? 63.f : 0.f, x1 = 640, y1 = s.map_hq ? 329.f : 266.f;
    const float ix0 = x0 + 5, iy0 = y0 + 5, ix1 = x1 - 5, iy1 = y1 - 5;
    framed_panel(out, c, x0, s.map_hq ? 43.f : y0, x1, s.map_hq ? 350.f : y1);
    const auto alpha = std::uint32_t(std::clamp(s.map_alpha, kMapAlphaMin, kMapAlphaMax));
    draw_sprite(out, c.vs, c.tex, {HudPage::Minimap, 0, 0, 256, 256}, ix0, iy0, ix1, iy1, alpha << 24 | 0x37ff37u);
    // CSatelliteMap_DrawObjects 10074670 (hud-leftovers.md §3): buildings first, as 20×20 icons centred on the point
    // (CBuilding_DrawMapIcon 100349b0; the `icons` texture, LoadMapIconSprites 10065370), then the units
    // (CUnit_DrawMapIcon 10077ae0): a flyer is a 4×4 "+", the others a 3×3 square; the selected one (white) and
    // the hero (green) get a box. [port] Sizes in virtual pixels like the rest; the heading line and the selected
    // unit's route are not drawn.
    static constexpr float kIconUv[11][2] = {{0, 24},  {24, 24},  {48, 0},   {72, 24},  {96, 24}, {120, 24},
                                             {144, 24}, {48, 24}, {168, 24}, {216, 24}, {216, 24}};
    for (int pass = 0; pass < 2; ++pass)
        for (const HudMapDot& d : s.map_dots) {
            if (d.building != (pass == 0)) continue;
            const float x = std::round(ix0 + d.x), y = std::round(iy1 - d.y);
            if (x < ix0 || x > ix1 || y < iy0 || y > iy1) continue;
            if (d.building) {
                if (d.icon >= 0 && d.icon < 11)
                    draw_sprite(out, c.vs, c.tex, {HudPage::Icons, kIconUv[d.icon][0], kIconUv[d.icon][1], 24, 24}, x - 10, y - 10, x + 10, y + 10,
                                d.argb);
                continue;
            }
            if (d.flyer) {
                draw_line(out, c.vs, x - 2, y, x + 2, y, d.argb);
                draw_line(out, c.vs, x, y - 2, x, y + 2, d.argb);
                if (d.selected) draw_rect_outline(out, c.vs, x - 4, y - 4, x + 4, y + 4, 0xffffffffu);
                continue;
            }
            const float bx0 = x - 1, by0 = y - 1, bx1 = bx0 + 3, by1 = by0 + 3; // w = h = 3, from (x − w/2, y − h/2)
            fill(out, c.vs, bx0, by0, bx1, by1, d.argb);
            if (d.selected || d.hero) draw_rect_outline(out, c.vs, bx0 - 2, by0 - 2, bx1 + 1, by1 + 1, d.selected ? 0xffffffffu : 0xff00ff00u);
        }
    // The camera marker (views 2 / 4): a 4×4 box with an X and a direction stub 2..7 px, yellow.
    if (s.map_camera) {
        const auto& m = *s.map_camera;
        const float x = ix0 + std::round(m[0]), y = iy1 - std::round(m[1]), dx = m[2], dy = -m[3];
        draw_rect_outline(out, c.vs, x - 2, y - 2, x + 2, y + 2, 0xffffff00u);
        draw_line(out, c.vs, x - 2, y - 2, x + 2, y + 2, 0xffffff00u);
        draw_line(out, c.vs, x - 2, y + 2, x + 2, y - 2, 0xffffff00u);
        draw_line(out, c.vs, x + 2 * dx, y + 2 * dy, x + 7 * dx, y + 7 * dy, 0xffffff00u);
    }
    draw_sprite(out, c.vs, c.tex, {HudPage::Page6, 132, 106, 21, 42}, ix1 - 21 - 1, iy1 - 42 - 1, ix1 - 1, iy1 - 1);
    // CSatelliteMap_DrawAlphaLabel 10074a90: "NN%" (rounded down to 5) for 1 s after a change, at X = x0 + 6,
    // ty = Y − (fontH + 1) with Y = y1 − 6, on a 0xff007300 box.
    if (s.map_alpha_label) {
        const int pct = int(alpha) * 100 / 255;
        const std::string t = std::to_string(pct - pct % 5) + "%";
        const float w = text_w(c, t), h = text_h(c);
        const float X = x0 + 6, ty = std::round(y1 - 6 - h);
        fill(out, c.vs, X - 3, ty - 3, std::round(X + 2 + w), std::round(ty + 4 + h - 1), 0xff007300u);
        text(out, c, X, ty, t, 0xff37ff37u);
    }
}

void draw_hud(OverlayFrame& out, const HudContext& c, const HudState& s) {
    if (s.cockpit) {
        draw_radar(out, c, s);                                  // page 0
        if (s.target) draw_unit_panel(out, c, *s.target, 0, s.marker); // page 1
        draw_unit_panel(out, c, s.own, 1, std::nullopt);        // page 2
        draw_status_lamps(out, c, s);                           // page 5
        draw_weapon_rows(out, c, s);                            // page 6
        if (s.has_camera) draw_sight(out, c, s);                // page 8
    }
    draw_satellite_map(out, c, s);
}

} // namespace parkan::ui
