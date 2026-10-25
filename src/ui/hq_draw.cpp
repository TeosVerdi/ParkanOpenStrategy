// HqUi_Draw 10083ad0 and the screens it draws, as overlay quads on the 640×480 virtual screen. Layouts read
// from the draw functions named at each part; the compaund.cfg skin cells are the RowCell_* primitives
// (notes/hud.md §1.3, GetSkinSpriteByStyle 100987f0).
#include "ui/hq.hpp"
#include "ui/designer.hpp"

#include "game/mission_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace parkan::ui {

namespace {

struct Ui {
    OverlayFrame& out;
    const HudContext& c;
    const HqSprites& sp;
    const HqController& hq;

    void sprite(const char* name, float x0, float y0, float x1, float y1, std::uint32_t argb = 0xffffffffu) const {
        if (const Sprite* s = sp.get(name)) draw_sprite(out, c.vs, c.tex, *s, x0, y0, x1, y1, argb);
    }
    float sprite_w(const char* name) const {
        const Sprite* s = sp.get(name);
        return s ? s->w : 0.f;
    }
    float text_w(std::string_view t) const { return c.font.font ? text_width_px(c.vs, *c.font.font, t) / c.vs.sx() : 0.f; }
    float text_h() const { return c.font.font ? float(c.font.font->height + 1) * c.vs.glyph_scale / c.vs.sy() : 0.f; }
    void text(float x, float y, std::string_view t, std::uint32_t argb) const {
        draw_text(out, c.vs, c.font, std::round(x), std::round(y), t, argb);
    }
    std::string str(std::uint32_t id) const { return hq.host().text ? hq.host().text(id) : std::string(); }
};

// A row of skin cells (the RowCell_* cursor): from (x, y), to the right, or to the left with each cell
// mirrored over (x, y)-(x − w − 1, y + 19) when `left` (FUN_10099950(0, 1)).
struct Row {
    const Ui& ui;
    float x, y;
    bool left = false;

    void cell(const char* name, float w) {
        if (left) ui.sprite(name, x, y, x - w - 1, y + 19), x -= w;
        else ui.sprite(name, x, y, x + w, y + 19), x += w;
    }
    float x0(float w) const { return left ? x - w : x; } // the cell's left edge before it is drawn
    void centred(float cx, float w, std::string_view t, std::uint32_t argb) const {
        ui.text(std::round((w - ui.text_w(t)) * 0.5f + cx), std::round(y + (19 - ui.text_h()) / 2), t, argb);
    }
    void ending(int n) { cell(n ? "ccres_ending_text" : "ccres_ending_stub", 5); }         // RowCell_Ending (style 0)
    void lamp_ending(int n) { // FUN_10099b00 (style 2: lamp_text_ending off / normal / pressed)
        static const char* k[3] = {"ccres_lamp_text_ending_off", "ccres_lamp_text_ending_normal", "ccres_lamp_text_ending_pressed"};
        cell(k[std::clamp(n, 0, 2)], 10);
    }
    void separator() { cell("ccres_separator_left_text", 5); } // RowCell_Separator (style 3)
    void emitter(int n) {                                     // RowCell_Emitter (style 5)
        static const char* k[3] = {"ccres_ray_emitter_off", "ccres_ray_emitter_normal", "ccres_ray_emitter_pressed"};
        cell(k[std::clamp(n, 0, 2)], 10);
    }
    void ray_end() { cell("ccres_ray_ending", 6); } // RowCell_RayEnd (style 7)
    void text_cell(float w, std::string_view t, std::uint32_t argb = 0xffffffffu) { // RowCell_Text (body_text)
        const float cx = x0(w);
        cell("ccres_body_text", w);
        centred(cx, w, t, argb);
    }
    // RowCell_Bar 1009a320: the ray body stretched to w, a fill of pct (< 0: none) 2 px in, 3 px inset, the label.
    void bar(float w, int pct, std::string_view label, std::uint32_t argb = 0xffffffffu) {
        const float cx = x0(w), edge = x;
        cell("ccres_ray_body", w);
        if (pct > 0) {
            pct = std::min(pct, 100);
            const std::uint32_t fill = pct < 20 ? 0x80800000u : pct < 80 ? 0x80808000u : 0x80008000u;
            const float len = std::round(float(pct) * w * 0.01f);
            if (left) fill_rect(ui.out, ui.c.vs, edge - len, y + 3, edge - 2, y + 16, fill);
            else fill_rect(ui.out, ui.c.vs, edge + 2, y + 3, edge + len, y + 16, fill);
        }
        centred(cx, w, label, argb);
    }
    // FUN_1009a740: a 19×19 body_text cell with a 15×15 icon 2 px in.
    void icon_cell(const Sprite* icon, std::uint32_t argb) {
        const float cx = x0(19);
        cell("ccres_body_text", 19);
        if (icon) draw_sprite(ui.out, ui.c.vs, ui.c.tex, *icon, cx + 2, y + 2, cx + 17, y + 17, argb);
    }
    // FUN_1009aaa0 (style 12 short_button, 35 px; icon at +10, +2) and FUN_1009a980 (style 13 long_button, 50 px;
    // icon at +10, +2, 30×15) and FUN_1009a200 (style 11 exit_button; icon at +15, +3, 13×13).
    void button(const char* kind, int n, const char* icon, std::uint32_t argb = 0xffffffffu) {
        static const char* suffix[3] = {"_off", "_normal", "_pressed"};
        const std::string frame = std::string("ccres_") + kind + suffix[std::clamp(n, 0, 2)];
        const float w = ui.sprite_w(frame.c_str()), cx = x0(w);
        cell(frame.c_str(), w);
        const Sprite* s = ui.sp.get(icon);
        if (!s) return;
        const float ix = std::string_view(kind) == "exit_button" ? 15.f : 10.f, iy = std::string_view(kind) == "exit_button" ? 3.f : 2.f;
        draw_sprite(ui.out, ui.c.vs, ui.c.tex, *s, cx + ix, y + iy, cx + ix + s->w, y + iy + s->h, argb);
    }
};

// FUN_1009ab90 (frame style 9/10): corners, top/bottom edges, left/right edges.
void framed_panel(const Ui& ui, float x0, float y0, float x1, float y1) {
    static const char* corner[4] = {"ccres_frame_corner_1", "ccres_frame_corner_2", "ccres_frame_corner_3", "ccres_frame_corner_4"};
    for (int r = 0; r < 4; ++r) {
        const float cx = r == 0 || r == 3 ? x0 : x1 - 8, cy = r < 2 ? y0 : y1 - 8;
        ui.sprite(corner[r], cx, cy, cx + 8, cy + 8);
    }
    ui.sprite("ccres_frame_edge_h", x0 + 8, y0, x1 - 7, y0 + 5);
    ui.sprite("ccres_frame_edge_h", x0 + 8, y1, x1 - 7, y1 - 5);
    ui.sprite("ccres_frame_edge_v", x0 + 5, y0 + 8, x0, y1 - 7);
    ui.sprite("ccres_frame_edge_v", x1 - 5, y0 + 8, x1, y1 - 7);
}

std::string pct_text(int v) { return std::to_string(v) + "%"; }

// GetUnitIcons 10077570: the class icon (ui_menu 15×15 at (49 | 81 | 65, 110)) tinted by the unit kind.
std::optional<Sprite> unit_class_icon(std::uint32_t type) {
    switch (type) {
    case game::kTypeWarbot: case game::kTypeHq: case game::kTypeHero: return Sprite{HudPage::UiMenu, 49, 110, 15, 15};
    case game::kTypeTransport: return Sprite{HudPage::UiMenu, 81, 110, 15, 15};
    case game::kTypeBuilder: return Sprite{HudPage::UiMenu, 65, 110, 15, 15};
    default: return std::nullopt;
    }
}
std::uint32_t unit_kind_tint(std::uint32_t kind) {
    switch (kind) {
    case 1: return 0xffffe7ffu;
    case 2: return 0xffff8080u;
    case 3: return 0xff80ff80u;
    case 4: return 0xff8080ffu;
    default: return 0xffffffffu;
    }
}
// GetUnitIcons 10077570: the second icon by attribute 0x207 (chassis type 1..4 at u = 97/49/65/81, v = 94).
std::optional<Sprite> unit_chassis_icon(std::int32_t chassis) {
    static constexpr float kU[4] = {97, 49, 65, 81};
    if (chassis < 1 || chassis > 4) return std::nullopt;
    return Sprite{HudPage::UiMenu, kU[chassis - 1], 94, 15, 15};
}

// DrawUnitStats 10070090 at (x, y): the title (yellow, at y + 1), then weight, max speed, defence, combat and radar
// lines from y + 1 + LH + 3, step LH = round((font height + 2) / ScaleY): the label green at x, the value at
// x + 80 + round(52 − value width) (right-aligned; 100e6454 = 52.0, the width is the value string's, 1007035c) in 0xffb4b4ff (the weight red when the free payload is 0), the unit green at
// x + 135. Defence and combat as % normalised with iron_3d.ini [TEMP] (NORMALIZE, DEFENCE_MIN/MAX,
// OFFENCE_MIN/MAX; a value ≤ MIN prints the original's leftover register — the port prints 0).
void draw_unit_stats(const Ui& ui, float x, float y, const economy::UnitStats& st, const std::string& title) {
    const HqStatNormalize& nm = ui.hq.host().stat_norm;
    const float lh = ui.c.font.font ? std::round(float(ui.c.font.font->height + 2) * ui.c.vs.glyph_scale / ui.c.vs.sy()) : 10.f;
    if (!title.empty()) ui.text(x, y + 1, title, 0xffffff00u);
    float yl = y + 1 + lh + 3;
    auto line = [&](std::uint32_t label, const std::string& value, std::uint32_t value_ink, const std::string& unit) {
        const std::string l = ui.str(label);
        ui.text(x, yl, l, 0xff00ff00u);
        ui.text(x + 80 + std::round(52 - ui.text_w(value)), yl, value, value_ink); // right-aligned at x + 132
        ui.text(x + 135, yl, unit, 0xff00ff00u);
        yl += lh;
    };
    auto fmt = [](const char* f, double a, double b = 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, f, a, b);
        return std::string(buf);
    };
    auto pct = [&](float v, float lo, float hi) {
        if (!nm.normalize) return fmt("%f", v);
        if (v >= hi) return std::string("100");
        if (v > lo) return std::to_string(int(std::lround(100.f / (hi - lo) * (v - lo))));
        return std::string("0");
    };
    line(5069, fmt("%-.f / %-.f", st.mass * 0.001, st.payload_free * 0.001), st.payload_free > 0 ? 0xffb4b4ffu : 0xffff0000u, ui.str(6206));
    line(5070, fmt("%-.f", st.speed * 3.6), 0xffb4b4ffu, ui.str(6176));
    line(5071, pct(st.defence, nm.defence_min, nm.defence_max), 0xffb4b4ffu, "%");
    line(5072, pct(st.offence, nm.offence_min, nm.offence_max), 0xffb4b4ffu, "%");
    line(5073, fmt("%-.f", st.radar.value_or(0.f)), 0xffb4b4ffu, ui.str(6178));
}

// FUN_10076a30 / FUN_10070b90: a design's name "%s-%s %s" (debug code, "X", type name) in yellow.
std::string design_title(const Ui& ui, const economy::UnitStats& st, std::uint32_t type) {
    return economy::unit_debug_code(st.size_class, st.chassis_type, type) + "-X " + ui.str(economy::unit_type_name_id(type));
}

// FUN_100346a0: the building icon (ui_menu 15×15) by type, tinted by +0x30 (1 white, 2 red, 3 green, 4/5 blue).
std::optional<Sprite> building_icon(std::uint32_t type) {
    switch (type) {
    case game::kTypeInstitute: return Sprite{HudPage::UiMenu, 49, 126, 15, 15};
    case game::kTypeHangar: return Sprite{HudPage::UiMenu, 97, 126, 15, 15};
    case game::kTypePlant: return Sprite{HudPage::UiMenu, 129, 94, 15, 15};
    case game::kTypeGenerator: return Sprite{HudPage::UiMenu, 113, 126, 15, 15};
    case game::kTypeMine: return Sprite{HudPage::UiMenu, 129, 126, 15, 15};
    case game::kTypeStorage: return Sprite{HudPage::UiMenu, 81, 110, 15, 15};
    case game::kTypeBunkerSmall: case game::kTypeBunkerMedium: case game::kTypeBunkerLarge: return Sprite{HudPage::UiMenu, 65, 126, 15, 15};
    case game::kTypeTowerMedium: case game::kTypeTowerLarge: return Sprite{HudPage::UiMenu, 81, 126, 15, 15};
    default: return std::nullopt; // teleports use another page (not drawn)
    }
}

std::string unit_label(const Ui& ui, std::uint32_t lid) {
    // name + " [" + GetOrderTypeName(first order) + "]" (DAT_101043cc / DAT_10102f5c).
    const game::WorldObject* o = ui.hq.world().find(lid);
    const std::string name = ui.hq.display_name(lid);
    return name + " [" + ui.str(order_type_text(o ? o->current_order() : 0)) + "]";
}

float health(const Ui& ui, std::uint32_t lid) { return ui.hq.host().health ? ui.hq.host().health(lid) : 100.f; }

// CResourceStrip_Draw 1006d9a0: two rows right to left from x = 640 at y = 0 and 21.
void draw_strip(const Ui& ui) {
    const HqController& hq = ui.hq;
    const bool m0 = hq.minerals_shown == 0, e0 = hq.energy_shown == 0;
    // [inferred] one text colour for the strip: red while the energy value is 0, else 0xff80ff80 (the decompile
    // sets it once before both rows).
    const std::uint32_t ink = e0 ? 0xffff3232u : 0xff80ff80u;
    auto row = [&](float y, int v, bool zero, std::uint32_t title) {
        const bool shown = !zero || hq.strip_blink;
        Row r{ui, 640, y, true};
        r.ending(1);
        r.text_cell(35, shown ? pct_text(v) : std::string(), ink);
        r.separator();
        r.emitter(0);
        r.bar(205, shown ? v : -1, shown ? ui.str(title) : std::string(), ink); // [inferred] the fill is the value
        r.ray_end();
    };
    row(0, hq.minerals_shown, m0, 5092);
    row(21, hq.energy_shown, e0, 5093);
}

// The sidebar (HqUi_Draw's first part; widgets FUN_1009be40).
void draw_sidebar(const Ui& ui) {
    const HqController& hq = ui.hq;
    ui.sprite("objpanel_head", 0, 0, 46, 12);
    const int hover = hq.hovered_button();
    for (int k = 0; k < hq.sidebar_items(); ++k) {
        const int item = kSidebarItem[k];
        if (item < 0) {
            const float y = kSidebarSeparators[-item - 1];
            ui.sprite("objpanel_separator", 0, y, 46, y + 24);
            continue;
        }
        const HqSidebarButton& b = kSidebar[item];
        const bool enabled = hq.sidebar_enabled(item);
        int frame = !enabled ? 0 : hq.sidebar_selected(item) ? 2 : (hover == item ? 1 + int(hq.hover_blink) : 1);
        static const char* kFrame[3] = {"objpanel_button1", "objpanel_button2", "objpanel_button3"};
        ui.sprite(kFrame[frame], 0, b.y, 46, b.y + 29);
        ui.sprite(b.icon, 16, b.y + 3, 16 + 23, b.y + 3 + 23, frame == 0 ? 0xff646464u : 0xffffffffu);
    }
    // The lock (same widget draw): disabled while animating.
    const float ly = hq.lock_y();
    const bool lock_hover = hq.mouse_x >= 0 && hq.mouse_x < 46 && hq.mouse_y >= ly && hq.mouse_y < ly + 24;
    const int lf = hq.sidebar_items() % 16 != 0 ? 0 : lock_hover ? 1 + int(hq.hover_blink) : 1;
    static const char* kLock[3] = {"objpanel_lock1", "objpanel_lock2", "objpanel_lock3"};
    ui.sprite(kLock[lf], 0, ly, 46, ly + 24);
    ui.sprite("objpanel_lock_icon", 18, ly + 7, 18 + 13, ly + 7 + 13, lf == 0 ? 0xff646464u : 0xffffffffu);
}

// HqUi_DrawTitle 10083e00: ending, a 278-px text cell, the close box.
void draw_title(const Ui& ui) {
    Row r{ui, 51, 0};
    r.ending(1);
    r.text_cell(0x116, ui.str(kScreenTitle[std::clamp(ui.hq.screen(), 0, 8)]));
    r.button("exit_button", 2, "exit_icon");
}

// CUnitRow_Draw 10095b20.
void draw_unit_row(const Ui& ui, std::uint32_t lid, float y) {
    const bool sel = ui.hq.selected(lid);
    const game::WorldObject* o = ui.hq.world().find(lid);
    Row r{ui, 51, y};
    r.lamp_ending(sel);
    const auto icon = unit_class_icon(o ? o->type : 0);
    r.icon_cell(icon ? &*icon : nullptr, unit_kind_tint(o ? o->size_class : 0));
    const auto st = ui.hq.host().unit_stats ? ui.hq.host().unit_stats(lid) : std::nullopt;
    const auto chassis = unit_chassis_icon(st ? st->chassis_type : 0);
    r.icon_cell(chassis ? &*chassis : nullptr, unit_kind_tint(o ? o->size_class : 0));
    r.separator();
    r.emitter(sel);
    r.bar(248, int(health(ui, lid)), unit_label(ui, lid), sel ? 0xffffffffu : 0xff808080u);
    r.ray_end();
}

// CBuildingRow_Draw 10096190.
void draw_building_row(const Ui& ui, std::uint32_t lid, float y) {
    const bool sel = ui.hq.selected_building() == lid;
    const game::WorldObject* o = ui.hq.world().find(lid);
    const std::uint32_t t = o ? o->type : 0;
    const bool bunker = t == game::kTypeBunkerSmall || t == game::kTypeBunkerMedium || t == game::kTypeBunkerLarge;
    const bool turret = bunker || t == game::kTypeTowerMedium || t == game::kTypeTowerLarge;
    Row r{ui, 51, y};
    r.lamp_ending(sel);
    const auto icon = building_icon(t);
    r.icon_cell(icon ? &*icon : nullptr, 0xffffffffu);
    r.separator();
    if (bunker) r.button("short_button", sel, "buildscreen_direct_icon");
    if (turret) r.button("short_button", sel, "buildscreen_hq_icon"); // greyed when IsTurretDisabled [port: never]
    const float w = 0x162 - r.x; // the bar runs to 354 from the emitter's start
    r.emitter(sel);
    r.bar(w, int(health(ui, lid)), ui.hq.display_name(lid), sel ? 0xffffffffu : 0xff808080u);
    r.ray_end();
}

// HqUi_DrawUnitPanel 10085c70 at (51, 20).
void draw_unit_panel(const Ui& ui, std::uint32_t lid) {
    const float x = 51, y = 20;
    const game::WorldObject* o = ui.hq.world().find(lid);
    if (!o) return;
    const std::uint32_t tint = unit_kind_tint(o->size_class);
    if (const auto icon = unit_class_icon(o->type)) draw_sprite(ui.out, ui.c.vs, ui.c.tex, *icon, x + 9, y + 7, x + 24, y + 22, tint);
    const auto st = ui.hq.host().unit_stats ? ui.hq.host().unit_stats(lid) : std::nullopt;
    if (const auto ch = unit_chassis_icon(st ? st->chassis_type : 0)) draw_sprite(ui.out, ui.c.vs, ui.c.tex, *ch, x + 26, y + 7, x + 41, y + 22, tint);
    ui.text(x + 50, y + 10, unit_label(ui, lid), 0xffffff00u);
    // DrawUnitStats(x + 71, y + 7, unit, "") (call at 1008633f).
    if (st) draw_unit_stats(ui, x + 71, y + 7, *st, {});
    const bool usable = o->alive && o->current_order() != 0x18;
    const std::uint32_t ink = usable ? 0xffffffffu : 0xff808080u;
    auto button = [&](float bx, const char* icon, std::uint32_t argb) {
        ui.sprite("short_button_frame_off", x + bx, y + 68, x + bx + 21, y + 89, argb);
        ui.sprite(icon, x + bx + 3, y + 71, x + bx + 18, y + 86, argb);
    };
    button(9, "buildscreen_hq_icon", ink);           // 1502 manual control
    button(33, "botscreen_autogunner_icon", ink);    // 1519 autogunner
    button(57, "botscreen_autodriver_icon", ink);    // 1518 autopilot
    if (o->type == game::kTypeHq) button(87, "buildscreen_direct_icon", 0xff808080u); // 1517 [IsHqCapable not modelled]
    const bool armed = ui.hq.self_destruct.count(lid) != 0;
    ui.sprite(armed ? "long_button_frame_on" : "long_button_frame_off", x + 273, y + 68, x + 305, y + 89);
    ui.sprite("self_destruction_icon", x + 275, y + 70, x + 303, y + 87, armed ? 0xff808080u : 0xffffffffu);
}

// HqUi_DrawUnitScreen 10085910 + COrderMenu_Draw 1007b630.
void draw_unit_screen(const Ui& ui) {
    const HqController& hq = ui.hq;
    framed_panel(ui, 51, 20, 369, 117);
    const auto& sel = hq.selected_units();
    if (sel.empty()) {
        ui.text(71, 30, ui.str(5076), 0xffffffffu); // [inferred position]
    } else if (sel.size() == 1) {
        draw_unit_panel(ui, sel.front());
    } else {
        char buf[160];
        std::snprintf(buf, sizeof buf, ui.str(5077).c_str(), int(sel.size()));
        ui.text(71, 30, buf, 0xffffffffu); // [inferred position]
    }
    const auto rows = hq.screen_units();
    for (std::size_t i = 0; i < rows.size(); ++i) draw_unit_row(ui, rows[i], 118 + 20.f * float(i));
    // The order menu: header (ending, 158-px "Приказы", scroll buttons, mirrored ending), then its rows.
    const auto menu = hq.order_menu();
    const float mx = hq.menu_x(), my = hq.menu_y();
    Row h{ui, mx, my};
    h.ending(0);
    h.text_cell(0x9e, ui.str(0x13dc));
    h.button("long_button", 0, "scroll_up_icon");
    h.button("long_button", 0, "scroll_down_icon");
    h.x += 5, h.left = true;
    h.ending(0);
    const int n = hq.menu_rows(menu.size());
    for (int k = 0; k < n; ++k) {
        const std::size_t i = std::size_t(hq.menu_scroll + k);
        if (i >= menu.size()) break;
        std::uint32_t text = 0;
        for (const HqMenuItem& m : kOrderMenu)
            if (m.cmd == menu[i]) text = m.text;
        Row r{ui, mx, my + 20 + 20.f * float(k)};
        r.ending(0); // FUN_1009c7d0 with index −1: ending, emitter, 247-px bar
        r.emitter(1);
        r.bar(247, -1, ui.str(text));
        r.ray_end();
    }
}

// The research screen (FUN_10089cb0; details 10089090, rows 10088a00).
void draw_research(const Ui& ui) {
    const HqController& hq = ui.hq;
    const economy::Research* res = hq.economy() ? hq.economy()->research(hq.clan()) : nullptr;
    framed_panel(ui, 51, 20, 369, 158);
    const auto list = hq.research_list();
    const std::int32_t sel = hq.research_selected;
    const mission::ResearchItem* item = res ? res->item(sel) : nullptr;
    if (!item || (res->state(sel) & economy::kResResearched)) {
        ui.text(71, 30, ui.str(5078), 0xffffffffu); // "Запчасть не выбрана"
    } else {
        ui.text(71, 30, item->name, 0xffffffffu);
        ui.text(71, 48, ui.str(5079), 0xffffffffu); // "ПОТОМКИ"
        const auto& unlocks = res->unlocks(sel);
        const float h = ui.text_h() + 1;
        for (std::size_t i = 0; i < unlocks.size() && i < 4; ++i)
            if (const mission::ResearchItem* u = res->item(std::int32_t(unlocks[i]))) ui.text(86, 60 + float(i) * h, u->name, 0xffffffffu);
        if (unlocks.size() > 4) ui.text(86, 60 + 4 * h, "...", 0xffffffffu);
        // The item's display model in a 127×127 CModelView at (226, 18) (CResearchDetails 10088d90 [layout from
        // hq-ui.md §8]).
        if (const mission::ResearchTree& tree = res->tree(); hq.host().part_mesh && item->model_index() < tree.models.size())
            hq.research_view.mesh = hq.host().part_mesh(tree.models[item->model_index()]);
        draw_model_view(ui.out, ui.c.vs, hq.research_view, 226, 18, 127, 127, hq.now());
    }
    // The batch button (6243): white for one frame after a press, else 0xff80ff80.
    const std::uint32_t batch_ink = hq.research_batch_flash >= 0 && hq.now() - hq.research_batch_flash < 34 ? 0xffffffffu : 0xff80ff80u;
    ui.sprite("long_button_frame_off", 170, 127, 202, 148);
    ui.sprite("batch_research_icon", 172, 129, 200, 146, batch_ink);
    // Header row y = 159: ending, body to 264, scroll up / down, mirrored ending.
    const int count = int(list.size());
    Row h{ui, 51, 159};
    h.ending(1);
    h.text_cell(208, "");
    h.button("long_button", hq.research_scroll > 0 ? 1 : 0, "scroll_up_icon");
    h.button("long_button", hq.research_scroll + 12 < count ? 1 : 0, "scroll_down_icon");
    h.x += 5, h.left = true;
    h.ending(1);
    for (int k = 0; k < 12; ++k) {
        const int i = hq.research_scroll + k;
        if (i >= count) break;
        const std::int32_t id = list[std::size_t(i)];
        const mission::ResearchItem* it = res ? res->item(id) : nullptr;
        std::string label = it ? it->name : std::string();
        if (it && !it->code.empty()) label += " (" + it->code + ")";
        const bool running = hq.researching(id);
        Row r{ui, 51, 179 + 20.f * float(k)};
        r.ending(1);
        r.button("short_button", 1, running ? "resbutton_stop" : "resbutton_start", running ? 0xffff8080u : 0xffffffffu);
        r.emitter(id == sel);
        r.bar(262, int(std::lround((res ? res->progress(id) : 0.f) * 100)), label, id == sel ? 0xffffffffu : 0xffc0c0c0u);
        r.ray_end();
    }
}

// The plant panel (FUN_10097430 at (51, 21)).
void draw_factory(const Ui& ui) {
    const HqController& hq = ui.hq;
    const HqPlant* p = hq.current_plant();
    if (!p) return;
    const float x = 51, y = 21, x1 = x + 318;
    const economy::Economy* eco = hq.economy();
    const auto info = eco ? eco->plant(p->lid) : std::nullopt;
    const bool start = hq.can_start(*p), brain = hq.free_brains() > 0, free_bots = info && info->free_bots > 0;
    framed_panel(ui, x, y, x1, y + 128);
    Row r{ui, x, y + 129};
    r.ending(0);
    r.button("long_button", 1, "constructor_icon"); // 1511 the designer
    std::string progress;
    if (!start) {
        int pct = int(std::lround((info ? info->progress : 0.f) * 100));
        if (p->batch && pct == 100) pct = 0;
        progress = pct_text(pct);
    }
    r.emitter(0);
    r.bar(136, -1, progress);
    r.x += 11, r.left = true;
    r.emitter(0);
    r.x += 10, r.left = false;
    const bool project = p->project >= 0 || !start;
    if (!start) {
        r.button("long_button", p->batch && project ? 1 : 0, "batch_stop_build_icon"); // 6239
        r.button("long_button", !p->batch && project ? 1 : 0, "stop_build_icon");      // 3070
    } else {
        r.button("long_button", project && brain ? 1 : 0, "batch_build_icon"); // 6238
        r.button("long_button", project && brain ? 1 : 0, "build_icon");       // 1553
    }
    r.x += 5, r.left = true;
    r.ending(0);
    // The preview: name and stats at (x + 10, y + 10) [stats and the 3D view deferred]; no projects → 6240 / 6241.
    if (hq.projects().empty() && p->project < 0) {
        ui.text(x + 20, y + 10, ui.str(6240), 0xffffffffu);
        ui.text(x + 20, y + 10 + ui.text_h() + 1, ui.str(6241), 0xffffffffu);
    } else if (const mission::Scheme* sc = hq.preview_scheme()) {
        // The preview object's name (FUN_10070b90) and DrawUnitStats(x + 10, y + 10, preview, "") (10097974).
        if (const auto st = hq.host().design_stats ? hq.host().design_stats(*sc) : std::nullopt) {
            ui.text(x + 10, y + 10, design_title(ui, *st, sc->flags), 0xffffff00u);
            draw_unit_stats(ui, x + 10, y + 10, *st, {});
        }
        // CModelView at (x + 0xb9, y + 2), 0x73 × 0x73 (CPlantPanel_SelectProject 10098240), drawn last (10097430).
        if (hq.host().design_mesh) hq.factory_view.mesh = hq.host().design_mesh(*sc);
        draw_model_view(ui.out, ui.c.vs, hq.factory_view, x + 0xb9, y + 2, 0x73, 0x73, hq.now());
    }
    // The brain icon: 0xff80ff80 with a free brain, else 0xffc80000 blinking every 0.5 s; the free count beside.
    const bool blink_on = brain || std::fmod(hq.now(), 1000.0) < 500;
    if (blink_on) {
        ui.sprite("brain_icon", x1 - 45, y + 106, x1 - 30, y + 121, brain ? 0xff80ff80u : 0xffc80000u);
        ui.text(x1 - 25, y + 108, std::to_string(hq.free_brains()), 0xffffffffu); // [inferred position]
    }
    ui.sprite(free_bots ? "free_bots_icon" : "no_free_bots_icon", x + 0xe4, y + 0x6a, x + 0x102, y + 0x79);
    // The current production button while producing (white when shown, else green).
    if (!start) {
        ui.sprite("long_button_frame_off", x + 0x85, y + 0x66, x + 0xa5, y + 0x7b);
        ui.sprite("active_project", x + 0x87, y + 0x68, x + 0xa3, y + 0x79, p->project == -1 ? 0xffffffffu : 0xff80ff80u);
    }
    for (int i = 0; i < int(hq.projects().size()); ++i) {
        const float sx = x + 8 + 23.f * float(i);
        ui.sprite("short_button_frame_off", sx, y + 0x66, sx + 23, y + 0x7b);
        ui.sprite("project_icon", sx + 2, y + 0x68, sx + 0x15, y + 0x79, i == p->project ? 0xffffffffu : 0xff80ff80u);
    }
}

} // namespace

std::map<std::string, CursorDef> parse_cursor_cfg(std::string_view text) {
    std::map<std::string, CursorDef> out;
    for (const auto& o : game::parse_cfg(text)) {
        auto num = [&](const char* k, float fallback) {
            const std::string* v = o.get(k);
            return v ? float(std::atof(v->c_str())) : fallback;
        };
        const std::string* tex = o.get("texture");
        if (!tex) continue;
        std::string t = *tex, name = o.name;
        std::erase(t, '"');
        for (char& ch : t) ch = char(std::tolower((unsigned char)ch));
        for (char& ch : name) ch = char(std::tolower((unsigned char)ch));
        CursorDef d;
        d.frame0 = {t == "new_ui1" ? HudPage::NewUi1 : HudPage::UiMenu, num("offset_x", 0), num("offset_y", 0), num("width", 16), num("height", 16)};
        d.hot_x = num("extent_x", 0), d.hot_y = num("extent_y", 0);
        d.phase_s = o.get("phase_delay") ? num("phase_delay", 250) * 0.001f : 0.25f;
        out[name] = d;
    }
    return out;
}

const char* cursor_name(int mode) {
    switch (mode) {
    case kCurArrow: case kCurBox: return "arrow";
    case kCurPick: return "pick";
    case kCurPlace: return "place";
    case kCurTarget: return "target";
    case kCurGuard: return "guard";
    case kCurCapture: return "capture";
    case kCurWrong: return "wrong_place";
    case kCurUpgrade: return "upgrade";
    default: return nullptr; // 8: the placement ghost
    }
}

void draw_cursor(OverlayFrame& out, const VirtualScreen& vs, const HudTextures& tex, const CursorDef& c, int frame, float vx, float vy) {
    Sprite s = c.frame0;
    s.u += s.w * float(frame & 3);
    const float x = vx - c.hot_x, y = vy - c.hot_y;
    draw_sprite(out, vs, tex, s, x, y, x + s.w, y + s.h);
}

void draw_tooltip(OverlayFrame& out, const VirtualScreen& vs, HudFont tool_font, TooltipState& st, const std::string& text, float px,
                  float py, double now_ms, bool software_cursor) {
    if (text.empty() || !tool_font.font) return;
    if (std::abs(px - st.x) < 3 && std::abs(py - st.y) < 3) {
        if (now_ms - st.since_ms <= 250) return;
    } else {
        st.since_ms = now_ms, st.x = px, st.y = py;
        return;
    }
    // Screen pixels: box = text + 8 each way, text at +5, +4; below the pointer by 16 (12 hw) virtual px, flipped
    // left / above at the right / bottom edge (no clamp at the left / top).
    const float bw = text_width_px(vs, *tool_font.font, text) + 8, bh = float(tool_font.font->height) * vs.glyph_scale + 8;
    const float dy = std::round(vs.sy() * (software_cursor ? 16.f : 12.f));
    const float x0 = px + bw < vs.width ? px : px - bw - 1, y0 = py + dy + bh < vs.height ? py + dy : py - bh - 1;
    const float sx = vs.sx(), sy = vs.sy();
    fill_rect(out, vs, x0 / sx, y0 / sy, (x0 + bw) / sx, (y0 + bh) / sy, 0xfff5f596u);
    draw_rect_outline(out, vs, x0 / sx, y0 / sy, (x0 + bw) / sx, (y0 + bh) / sy, 0xff000000u);
    draw_text(out, vs, tool_font, (x0 + 5) / sx, (y0 + 4) / sy, text, 0xff000000u);
}

void draw_model_view(OverlayFrame& out, const VirtualScreen& vs, ModelView& mv, float x, float y, float w, float h, double now_ms) {
    if (!mv.mesh) return;
    if (mv.last_ms >= 0) mv.angle += float(now_ms - mv.last_ms) * 0.00075f;
    mv.last_ms = now_ms;
    const Vec3 c = (mv.mesh->bounds_min + mv.mesh->bounds_max) * 0.5f, ext = mv.mesh->bounds_max - mv.mesh->bounds_min;
    const float r = std::max(0.1f, 0.5f * std::sqrt(dot(ext, ext)));
    OverlayInset in;
    in.mesh = mv.mesh;
    // The object turns about +Z through its centre.
    Mat4 rz = Mat4::identity();
    rz.at(0, 0) = std::cos(mv.angle), rz.at(0, 1) = -std::sin(mv.angle), rz.at(1, 0) = std::sin(mv.angle), rz.at(1, 1) = std::cos(mv.angle);
    const Mat4 spin = Mat4::translation(c) * rz * Mat4::translation(Vec3{} - c);
    for (const Mat4& t : mv.mesh->transforms) in.transforms.push_back(spin * t);
    const float pitch = -0.5f, dist = 2 * r;
    in.forward = Vec3{0, std::cos(pitch), std::sin(pitch)}; // [inferred] looking along +Y, down 0.5 rad
    in.eye = c - in.forward * dist;
    in.up = Vec3{0, 0, 1};
    in.fov_x = 1.0471976f;
    in.znear = std::max(0.01f, dist - 2 * r), in.zfar = dist + 4 * r;
    in.x0 = x * vs.sx(), in.y0 = y * vs.sy(), in.x1 = (x + w) * vs.sx(), in.y1 = (y + h) * vs.sy();
    in.after_quads = out.quads.size();
    out.insets.push_back(std::move(in));
}

void draw_game_menu(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HqController& hq) {
    // CGameMenu_Draw 10065d00 (notes/game-menu.md): the dimmed world, DrawFramedPanel 1009af40 over the page rect,
    // then CGameMenuMainPage_Draw 10066c80. The multiplayer warning (3077) and the save page are not ported.
    if (!hq.menu_open()) return;
    const Ui ui{out, c, sp, hq};
    fill_rect(out, c.vs, 0, 0, 640, 480, 0x99000000u);
    framed_panel(ui, 200, 150, 440, 330);
    fill_rect(out, c.vs, 205, 155, 435, 325, 0xcc328032u);
    Row title{ui, 210, 160};
    title.ending(1);
    title.text_cell(215, ui.str(5085));
    title.left = true, title.x = 430;
    title.ending(1);
    // CGameMenuButton_DrawLampRow 10066250 for Return, Save, Load, Exit.
    const bool enabled[4] = {true, hq.menu_save_enabled, hq.menu_load_enabled, true};
    for (int k = 0; k < 4; ++k) {
        const float y0 = 210 + 25.f * float(k);
        const bool hover = hq.mouse_x >= 210 && hq.mouse_x < 430 && hq.mouse_y >= y0 && hq.mouse_y < y0 + 25;
        const int lamp = hover ? 2 : 1;
        const std::uint32_t argb = !enabled[k] ? 0xff808080u : hover ? 0xffffffffu : 0xff80ff80u;
        Row r{ui, 210, y0};
        r.lamp_ending(lamp);
        r.text_cell(210, ui.str(3080u + std::uint32_t(k)), argb);
        r.left = true, r.x = 430;
        r.lamp_ending(lamp);
    }
}

void draw_hq(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HqController& hq, bool objectives) {
    if (!hq.interface_up()) return;
    if (hq.designer_open()) { // CHud_Draw: the constructor shell instead of the HQ interface
        hq.designer()->draw(out, c, sp, hq.now());
        return;
    }
    const Ui ui{out, c, sp, hq};
    draw_strip(ui);
    if (objectives) return;
    const bool in_building = hq.state() == kStateInBuilding;
    if (!in_building) {
        draw_sidebar(ui);
        if (!hq.sidebar_expanded()) return;
    }
    switch (hq.screen()) {
    case kScreenWarbots: case kScreenTransports: case kScreenBuilders:
        draw_title(ui);
        draw_unit_screen(ui);
        break;
    case kScreenResearch:
        draw_title(ui);
        draw_research(ui);
        break;
    case kScreenPlants: {
        draw_title(ui);
        draw_factory(ui);
        if (!in_building) {
            const auto rows = hq.screen_buildings();
            for (std::size_t i = 0; i < rows.size(); ++i) draw_building_row(ui, rows[i], 171 + 20.f * float(i));
        }
        break;
    }
    case kScreenTowers: case kScreenBunkers: case kScreenOther: {
        draw_title(ui);
        const auto rows = hq.screen_buildings();
        for (std::size_t i = 0; i < rows.size(); ++i) draw_building_row(ui, rows[i], 20 + 20.f * float(i));
        break;
    }
    default: break;
    }
    // The selection box (cursor 7): a 0xff19b419 rectangle.
    if (hq.cursor() == kCurBox) draw_rect_outline(out, c.vs, hq.box_x0, hq.box_y0, hq.mouse_x, hq.mouse_y, 0xff19b419u);
}

} // namespace parkan::ui
