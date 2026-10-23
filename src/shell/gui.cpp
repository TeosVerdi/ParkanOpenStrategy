#include "shell/gui.hpp"

#include "shell/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace parkan::shell {

namespace {

void set_argb(OverlayQuad& q, std::uint32_t argb) {
    q.rgba[0] = std::uint8_t(argb >> 16), q.rgba[1] = std::uint8_t(argb >> 8), q.rgba[2] = std::uint8_t(argb), q.rgba[3] = std::uint8_t(argb >> 24);
}

float rnd(float v) { return std::nearbyint(v); }

// The text's top in virtual units: centred by its screen-pixel height (the original's 640 formula).
float text_y(const GuiEnv& env, const ui::HudFont& f, float y, float h) {
    return y + std::floor((h * env.sy() - GuiEnv::font_h(f, env.vs)) / 2) / env.sy();
}

// CGuiTextButton_FitText 10016680: drop trailing characters until the text is narrower than 95 %.
std::string fit_text(const GuiEnv& env, const ui::HudFont& f, std::string t, float span) {
    const float avail = rnd(env.sx() * span), limit = avail - std::floor(avail / 20);
    if (env.text_w(f, t) > limit)
        while (!t.empty() && env.text_w(f, t) >= limit) t.pop_back();
    return t;
}

// Centred text in [l, r] (virtual) the way the button and checkbox place it.
void draw_centred(OverlayFrame& out, const GuiEnv& env, const ui::HudFont& f, std::string_view t, float l, float r, float y, float h,
                  std::uint32_t argb) {
    const float tw_s = rnd(env.sx() * (r - l));
    const float xoff = std::trunc((tw_s - env.text_w(f, t)) / 2);
    ui::draw_text(out, env.vs, f, rnd(xoff / env.sx() + l), text_y(env, f, y, h), t, argb);
}

const char* kLamp[3] = {"lamp_highlight", "lamp_normal", "lamp_off"};
const char* kLamp1[3] = {"lamp1_highlight", "lamp1_normal", "lamp1_off"};
const char* kTile[3] = {"tile_highlight", "tile", "tile_disabled"};
const char* kStub[3] = {"stub_on", "stub_normal", "stub_off"};
const char* kTileNoText[3] = {"tile_not_text_highlight", "tile_not_text", "tile_not_text_disabled"};
const char* kArrowL[3] = {"arrow_left_on_selected", "arrow_left_off_active", "arrow_left_disable_active"};
const char* kArrowR[3] = {"arrow_right_on_selected", "arrow_right_off_active", "arrow_right_disable_active"};
const char* kLight[7] = {"green_light_on", "green_light_normal", "green_light_off", "light_neytral",
                         "red_light_off",  "red_light_normal",   "red_light_on"};

} // namespace

void draw_texture(OverlayFrame& out, const GuiEnv& env, std::int32_t slot, float u, float v, float w, float h, float x0, float y0,
                  float x1, float y1, std::uint32_t argb) {
    if (slot < 0) return;
    const float u0 = (u + 0.5f) / 256.f, u1 = (u + w) / 256.f, v0 = (v + 0.5f) / 256.f, v1 = (v + h) / 256.f;
    OverlayQuad q;
    q.free = true;
    q.texture = slot;
    const float X0 = x0 * env.sx(), X1 = x1 * env.sx(), Y0 = y0 * env.sy(), Y1 = y1 * env.sy();
    const float xs[4] = {X0, X1, X1, X0}, ys[4] = {Y0, Y0, Y1, Y1}, us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    for (int k = 0; k < 4; ++k) q.x[k] = xs[k], q.y[k] = ys[k], q.u[k] = us[k], q.v[k] = vs[k];
    set_argb(q, argb);
    out.quads.push_back(q);
}

void draw_named_sprite(OverlayFrame& out, const GuiEnv& env, std::string_view name, float x0, float y0, float x1, float y1) {
    if (!env.ctrls || !env.texture) return;
    const auto it = env.ctrls->sprites.find(std::string(name));
    if (it == env.ctrls->sprites.end()) return;
    const ui::HudSprite& s = it->second;
    const std::int32_t slot = env.texture(s.texture);
    if (slot < 0) return;
    if (s.rot == 0) {
        draw_texture(out, env, slot, float(s.x), float(s.y), float(s.w), float(s.h), x0, y0, x1, y1);
        return;
    }
    // CHudSprite_SetSource: L = [TL, BL, BR, TR]; the screen corners TL, TR, BR, BL take L[r], L[r+3], L[r+2], L[r+1].
    const float u0 = (float(s.x) + 0.5f) / 256.f, u1 = float(s.x + s.w) / 256.f, v0 = (float(s.y) + 0.5f) / 256.f, v1 = float(s.y + s.h) / 256.f;
    const float lu[4] = {u0, u0, u1, u1}, lv[4] = {v0, v1, v1, v0};
    OverlayQuad q;
    q.free = true;
    q.texture = slot;
    const float X0 = x0 * env.sx(), X1 = x1 * env.sx(), Y0 = y0 * env.sy(), Y1 = y1 * env.sy();
    const float xs[4] = {X0, X1, X1, X0}, ys[4] = {Y0, Y0, Y1, Y1};
    static constexpr int kOffset[4] = {0, 3, 2, 1};
    for (int k = 0; k < 4; ++k) {
        const int i = (s.rot + kOffset[k]) & 3;
        q.x[k] = xs[k], q.y[k] = ys[k], q.u[k] = lu[i], q.v[k] = lv[i];
    }
    out.quads.push_back(q);
}

float sprite_w(const GuiEnv& env, std::string_view name) {
    if (!env.ctrls) return 0;
    const auto it = env.ctrls->sprites.find(std::string(name));
    return it == env.ctrls->sprites.end() ? 0.f : float(it->second.w);
}

// ---- Widget ----

void Widget::enable(bool on) {
    enabled = on;
    if (!on) state = 2;
    else if (state == 2) state = 1;
}

void Widget::notify(GuiEnv& env, Command c) {
    if (parent) parent->on_command(env, c);
}

void Widget::set_focus(GuiEnv&) { state = enabled ? 0 : 2; }
void Widget::kill_focus(GuiEnv&) { state = enabled ? 1 : 2; }

void Widget::draw_tooltip(OverlayFrame& out, GuiEnv& env) const {
    // CGuiFlashWidget_DrawTooltip 100147b0 (screen pixels): a pale yellow box with a black frame and black
    // TOOL_FONT text, 16 px (×ScaleY) below the cursor, flipped at the screen edges.
    if (tooltip.empty() || !env.tool_font.font) return;
    const float sx = env.sx(), sy = env.sy();
    const float bw = env.text_w(env.tool_font, tooltip) + 8 * env.vs.glyph_scale, bh = GuiEnv::font_h(env.tool_font, env.vs) + 8 * env.vs.glyph_scale;
    const float mx = env.mouse_x * sx, my = env.mouse_y * sy, dy = rnd(sy * 16);
    float x0 = mx, y0 = my + dy;
    if (mx + bw >= env.vs.width) x0 = mx - bw - 1;
    if (my + dy + bh >= env.vs.height) y0 = my - bh - 1;
    ui::fill_rect(out, env.vs, x0 / sx, y0 / sy, (x0 + bw) / sx, (y0 + bh) / sy, 0xfff5f596u);
    const float t = std::max(1.f, env.vs.glyph_scale);
    ui::fill_rect(out, env.vs, x0 / sx, y0 / sy, (x0 + bw) / sx, (y0 + t) / sy, 0xff000000u);
    ui::fill_rect(out, env.vs, x0 / sx, (y0 + bh - t) / sy, (x0 + bw) / sx, (y0 + bh) / sy, 0xff000000u);
    ui::fill_rect(out, env.vs, x0 / sx, y0 / sy, (x0 + t) / sx, (y0 + bh) / sy, 0xff000000u);
    ui::fill_rect(out, env.vs, (x0 + bw - t) / sx, y0 / sy, (x0 + bw) / sx, (y0 + bh) / sy, 0xff000000u);
    ui::draw_text(out, env.vs, env.tool_font, (x0 + 5 * env.vs.glyph_scale) / sx, (y0 + 4 * env.vs.glyph_scale) / sy, tooltip, 0xff000000u);
}

void FlashWidget::set_focus(GuiEnv& env) {
    Widget::set_focus(env);
    env.play("HILIGHT");
}

std::uint32_t FlashWidget::text_colour(const GuiEnv& env) const {
    const TextFlash& f = env.ctrls ? env.ctrls->text_flash : TextFlash{};
    return state == 0 ? f.selected_bright : state == 1 ? f.on_bright : f.off_bright;
}

// ---- TextButton ----

void TextButton::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    int s = state;
    if (flash) {
        if (state == 0) {
            const double t = elapsed(env), I = interval(env);
            if (t > 2 * I) flash_ms = env.now_ms;
            else if (t > I) s = 1;
        } else {
            flash_ms = env.now_ms;
        }
    }
    const float lw = sprite_w(env, "lamp_highlight"), R = x + w, B = y + h;
    draw_named_sprite(out, env, kLamp[s], x, y, x + lw, B);
    draw_named_sprite(out, env, kLamp[s], R, y, R - lw - 1, B);
    draw_named_sprite(out, env, kTile[s], x + lw, y, R - lw, B);
    if (!text.empty()) {
        const std::string t = fit_text(env, env.menu_font, text, w - 2 * lw);
        draw_centred(out, env, env.menu_font, t, x + lw, R - lw, y, h, text_colour(env));
    }
    if (!tooltip.empty() && hit(env.mouse_x, env.mouse_y)) env.tooltip_owner = this;
}

bool TextButton::activate(GuiEnv& env) {
    if (!enabled) return false;
    env.play("BUTTON_CLICK");
    if (!container) notify(env, {id, 0, 0, 0});
    return true;
}

bool TextButton::lbutton_down(GuiEnv& env, float px, float py) {
    if (!enabled || !hit(px, py)) return false;
    return activate(env); // activation is on the press (100162a0)
}

bool TextButton::lbutton_up(GuiEnv&, float px, float py) { return hit(px, py); }

bool TextButton::key_down(GuiEnv& env, int vk) {
    if (enabled && (vk == kVkReturn || vk == kVkSpace)) return activate(env);
    return false;
}

void ImageButton::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    int idx = state;
    if (flash && state == 0) {
        const double t = elapsed(env), I = interval(env);
        if (t > 2 * I) flash_ms = env.now_ms;
        else if (t > I) idx = 1;
    } else if (state != 0) {
        flash_ms = env.now_ms;
    }
    static const char* kSuffix[3] = {"_on_selected", "_off_active", "_disable_active"};
    draw_named_sprite(out, env, prefix + kSuffix[std::clamp(idx, 0, 2)], x, y, x + w, y + h);
    if (!tooltip.empty() && hit(env.mouse_x, env.mouse_y)) env.tooltip_owner = this;
}

// ---- ScrollArrow ----

void ScrollArrow::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    int idx = state;
    if (state == 0 && !pressed) {
        const double t = elapsed(env), I = interval(env);
        if (t > 2 * I) flash_ms = env.now_ms;
        else if (t > I) idx = 1;
    } else if (state != 0) {
        flash_ms = env.now_ms;
    }
    static const char* kSuffix[3] = {"_on_selected", "_off_active", "_disable_active"};
    draw_named_sprite(out, env, prefix + kSuffix[std::clamp(idx, 0, 2)], x, y, x + w, y + h);
    if (!tooltip.empty() && hit(env.mouse_x, env.mouse_y)) env.tooltip_owner = this;
    // AutoRepeat 1001f1e0: while pressed, a bar command every scroll_interval.
    const float iv = env.ctrls ? env.ctrls->scroll_interval : 0.1f;
    if (enabled && pressed && (env.now_ms - repeat_ms) / 1000.0 > iv) {
        repeat_ms = env.now_ms;
        if (container && container->parent) container->parent->on_command(env, {container->id, 0, bar_index, 0});
        env.play("BUTTON_CLICK");
    }
}

bool ScrollArrow::lbutton_down(GuiEnv&, float px, float py) {
    if (enabled && hit(px, py)) pressed = true, repeat_ms = -1e9;
    return false; // the bar does not notify for the arrows
}
bool ScrollArrow::lbutton_up(GuiEnv&, float, float) {
    pressed = false;
    return false;
}
bool ScrollArrow::key_down(GuiEnv&, int vk) {
    if (enabled && (vk == kVkReturn || vk == kVkSpace)) pressed = true, repeat_ms = -1e9;
    return false;
}
bool ScrollArrow::key_up(GuiEnv&, int vk) {
    if (vk == kVkReturn || vk == kVkSpace) pressed = false;
    return false;
}
void ScrollArrow::kill_focus(GuiEnv& env) {
    pressed = false;
    ImageButton::kill_focus(env);
}

// ---- Title ----

void Title::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    // CGuiTitle_Draw 1001eb20: only the "_on" art.
    const float ew = sprite_w(env, "title_ending_on"), nw = sprite_w(env, "title_not_text_ending_on"), sw = sprite_w(env, "stub_on");
    const float R = x + w, B = y + h;
    draw_named_sprite(out, env, "title_ending_on", x, y, x + ew, B);
    const float L = x + ew;
    float T;
    if (buttons.empty()) {
        draw_named_sprite(out, env, "title_ending_on", R, y, R - ew - 1, B);
        T = R - ew;
    } else {
        draw_named_sprite(out, env, "title_not_text_ending_on", R, y, R - nw - 1, B);
        float c = R - nw - 34;
        for (auto& b : buttons) {
            b->x = c, b->y = y, b->w = 34, b->h = 25;
            b->draw(out, env);
            c -= 34;
        }
        c += 34;
        draw_named_sprite(out, env, "stub_on", c, y, c - sw - 1, B);
        T = c - sw;
    }
    draw_named_sprite(out, env, "tile_highlight", L, y, T, B);
    if (env.menu_font.font && !text.empty()) {
        const float xt = rnd(std::trunc((rnd(env.sx() * (T - L)) - env.text_w(env.menu_font, text)) / 2) / env.sx() + L);
        ui::draw_text(out, env.vs, env.menu_font, xt, text_y(env, env.menu_font, y, h), text, 0xffffffffu);
    }
}

// ---- CheckBox ----

void CheckBox::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    const int s = state;
    const float lw = sprite_w(env, "lamp1_highlight"), gw = sprite_w(env, "green_light_on"), sw = sprite_w(env, "stub_on");
    const float R = x + w, B = y + h;
    draw_named_sprite(out, env, kLamp1[s], x, y, x + lw, B);
    draw_named_sprite(out, env, kLamp1[s], R, y, R - lw - 1, B);
    const float L = x + lw, Rr = R - lw;
    int f = 0;
    if (s == 0) {
        const double t = elapsed(env), I = interval(env);
        if (t < I) f = 1;
        if (t > 2 * I) flash_ms = env.now_ms;
    }
    const int c = !enabled ? 3 : (checked ? f : 5 + f);
    draw_named_sprite(out, env, kLight[c], L, y, L + gw, B);
    draw_named_sprite(out, env, kTileNoText[s], Rr, y, Rr - gw - 1, B);
    const float L2 = L + gw - 1;
    draw_named_sprite(out, env, kStub[s], L2, y, L2 + sw, B);
    const float Rr2 = Rr - gw;
    draw_named_sprite(out, env, kStub[s], Rr2, y, Rr2 - sw - 1, B);
    const float L3 = L2 + sw - 1, Rr3 = Rr2 - sw;
    draw_named_sprite(out, env, kTile[s], L3, y, Rr3, B);
    const std::string t = fit_text(env, env.menu_font, text, Rr3 - L3);
    draw_centred(out, env, env.menu_font, t, L3, Rr3, y, h, text_colour(env));
}

void CheckBox::toggle(GuiEnv& env) {
    env.play("CHECKBOX");
    checked = !checked;
    notify(env, {id, 0, 0, 0});
}

bool CheckBox::lbutton_down(GuiEnv& env, float px, float py) {
    if (!enabled || !hit(px, py)) return false;
    toggle(env);
    return true;
}

bool CheckBox::key_down(GuiEnv& env, int vk) {
    if (!enabled || (vk != kVkReturn && vk != kVkSpace)) return false;
    toggle(env);
    return true;
}

// ---- Slider / ArrowScroller ----

float Slider::lw(const GuiEnv& env) const { return sprite_w(env, "lamp1_highlight"); }
float Slider::aw(const GuiEnv& env) const { return sprite_w(env, "arrow_left_on_selected"); }

std::string Slider::display_text() const {
    char buf[256];
    switch (kind) {
    case Kind::Percent: std::snprintf(buf, sizeof buf, "%s : %d%%", text.c_str(), int(std::nearbyint(value * 100))); return buf;
    case Kind::Float: std::snprintf(buf, sizeof buf, "%s : %3.1f", text.c_str(), double(value)); return buf;
    case Kind::Items: {
        const int i = index();
        return i >= 0 && std::size_t(i) < items.size() ? items[std::size_t(i)] : fallback;
    }
    }
    return text;
}

void Slider::step_by(GuiEnv& env, int dir) {
    const float old = value;
    value = std::clamp(value + float(dir) * step, min, max);
    if (kind == Kind::Items) value = std::round(value);
    if (on_change) on_change(env);
    if (value != old) {
        notify(env, {id, 0, 0, 0});
        env.play("BUTTON_CLICK");
    }
}

void Slider::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    const int s = state;
    int li = s, ri = s;
    if (s == 0) {
        const double t = elapsed(env), I = interval(env);
        bool A = t > I, Bf = (t > 0.5 * I && t <= I) || (t > 1.5 * I && t <= 2 * I);
        if (t > 2 * I) flash_ms = env.now_ms, A = Bf = false;
        switch (mode) {
        case 0: li = !A, ri = A; break;
        case 1: li = 0, ri = 1; break;
        case 2: li = 1, ri = 0; break;
        case 3: li = Bf, ri = 1; break;
        case 4: li = 1, ri = Bf; break;
        }
    } else {
        flash_ms = env.now_ms;
    }
    if (value <= min) li = 2;
    if (value >= max) ri = 2;
    const float LW = lw(env), AW = aw(env), sw = sprite_w(env, "stub_on"), R = x + w, B = y + h;
    draw_named_sprite(out, env, kLamp1[s], x, y, x + LW, B);
    draw_named_sprite(out, env, kLamp1[s], R, y, R - LW - 1, B);
    const float L = x + LW;
    draw_named_sprite(out, env, kArrowL[li], L, y, L + AW, B);
    const float Rr = R - LW;
    draw_named_sprite(out, env, kArrowR[ri], Rr - AW - 1, y, Rr, B);
    const float L2 = L + AW - 1;
    draw_named_sprite(out, env, kStub[s], L2, y, L2 + sw, B);
    const float Rr2 = Rr - AW;
    draw_named_sprite(out, env, kStub[s], Rr2, y, Rr2 - sw - 1, B);
    const float L3 = L2 + sw - 1, Rr3 = Rr2 - sw;
    draw_named_sprite(out, env, kTile[s], L3, y, Rr3, B);
    const float W = Rr3 - L3;
    if (kind != Kind::Items && max > min) {
        const float fill = rnd((value - min) * W / (max - min));
        ui::fill_rect(out, env.vs, L3, y + 4, L3 + fill, B - 4, 0x8032c832u);
    }
    const std::string t = display_text();
    const float xt = L3 + rnd((env.sx() * W - env.text_w(env.menu_font, t)) / (2 * env.sx()));
    ui::draw_text(out, env.vs, env.menu_font, xt, text_y(env, env.menu_font, y, h), t, text_colour(env));
    // AutoStep 10018f50 (sliders only: the arrow scroller steps once per key or click).
    if (kind != Kind::Items) {
        const float iv = env.ctrls ? env.ctrls->volume_scroll_interval : 0.1f;
        while ((mode == 1 || mode == 2) && (env.now_ms - repeat_ms) / 1000.0 > iv) {
            repeat_ms = env.now_ms;
            const int dir = mode == 1 ? -1 : 1;
            step_by(env, dir);
            if (value <= min || value >= max) mode = 0;
        }
    }
}

bool Slider::key_down(GuiEnv& env, int vk) {
    if (vk != kVkLeft && vk != kVkRight) return false;
    if (kind == Kind::Items) {
        step_by(env, vk == kVkLeft ? -1 : 1);
        return true;
    }
    mode = vk == kVkLeft ? 1 : 2;
    return true;
}

bool Slider::key_up(GuiEnv&, int vk) {
    if (vk != kVkLeft && vk != kVkRight) return false;
    mode = 0;
    return true;
}

bool Slider::lbutton_down(GuiEnv& env, float px, float py) {
    if (!enabled || !hit(px, py)) return false;
    const float LW = lw(env), AW = aw(env), R = x + w;
    if (px >= x + LW && px < x + LW + AW) {
        mode = value <= min ? 0 : 1;
        if (kind == Kind::Items) step_by(env, -1);
    } else if (px >= R - AW - LW && px < R - LW) {
        mode = value >= max ? 0 : 2;
        if (kind == Kind::Items) step_by(env, 1);
    } else {
        mode = 0;
    }
    repeat_ms = -1e9; // the first step comes with the next draw
    return true;
}

bool Slider::lbutton_up(GuiEnv& env, float px, float py) {
    const float LW = lw(env), AW = aw(env), R = x + w;
    const bool in_y = py >= y && py < y + h;
    if (in_y && px >= x + LW && px < x + LW + AW) mode = value <= min ? 0 : 3;
    else if (in_y && px >= R - AW - LW && px < R - LW) mode = value >= max ? 0 : 4;
    else mode = 0;
    return hit(px, py);
}

bool Slider::mouse_move(GuiEnv& env, float px, float py) {
    if (!hit(px, py)) {
        mode = 0;
        return false;
    }
    const float LW = lw(env), AW = aw(env), R = x + w;
    if (px >= x + LW && px < x + LW + AW) {
        if (mode != 1 && value > min) mode = 3;
    } else if (px >= R - AW - LW && px < R - LW) {
        if (mode != 2 && value < max) mode = 4;
    } else {
        mode = 0;
    }
    return true;
}

void Slider::kill_focus(GuiEnv& env) {
    mode = 0;
    FlashWidget::kill_focus(env);
}

// ---- ItemList ----

void ItemList::add(std::unique_ptr<TextButton> b) {
    b->container = this;
    b->parent = nullptr;
    items.push_back(std::move(b));
}

bool ItemList::hit(float px, float py) const {
    for (const auto& i : items)
        if (i->enabled && i->w > 0 && i->hit(px, py)) return true;
    return false;
}

void ItemList::select(GuiEnv& env, int i) {
    // CGuiItemList_SelectItem 100ca940.
    if (i < 0 || std::size_t(i) >= items.size() || !items[std::size_t(i)]->enabled) return;
    if (i != selected) {
        if (selected >= 0 && std::size_t(selected) < items.size()) items[std::size_t(selected)]->kill_focus(env);
        items[std::size_t(i)]->set_focus(env);
    }
    selected = i;
    if (selected < top) top = selected;
    else if (selected >= top + visible_rows) top = selected - visible_rows + 1;
}

void ItemList::clear_selection(GuiEnv& env) {
    if (selected >= 0 && std::size_t(selected) < items.size()) items[std::size_t(selected)]->kill_focus(env);
    selected = -1;
}

void ItemList::set_focus(GuiEnv&) { state = enabled ? 0 : 2; }
void ItemList::kill_focus(GuiEnv& env) {
    clear_selection(env);
    Widget::kill_focus(env);
}

bool ItemList::mouse_move(GuiEnv& env, float px, float py) {
    for (auto& i : items) i->mouse_move(env, px, py);
    for (std::size_t k = 0; k < items.size(); ++k)
        if (items[k]->enabled && items[k]->w > 0 && items[k]->hit(px, py)) {
            select(env, int(k));
            return true;
        }
    clear_selection(env);
    return false;
}

bool ItemList::lbutton_down(GuiEnv& env, float px, float py) {
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k]->w <= 0) continue;
        if (items[k]->lbutton_down(env, px, py)) {
            select(env, int(k));
            notify(env, {id, 0, int(k), 0});
            return true;
        }
    }
    return false;
}

bool ItemList::lbutton_up(GuiEnv& env, float px, float py) {
    bool any = false;
    for (auto& i : items) any |= i->lbutton_up(env, px, py);
    return any;
}

bool ItemList::key_down(GuiEnv& env, int vk) {
    if (vk == kVkReturn || vk == kVkSpace) {
        if (selected < 0) return false;
        items[std::size_t(selected)]->key_down(env, vk);
        notify(env, {id, 0, selected, 0});
        return true;
    }
    if (vertical && (vk == kVkUp || vk == kVkDown)) {
        const int d = vk == kVkUp ? -1 : 1;
        for (int i = selected + d; i >= 0 && std::size_t(i) < items.size(); i += d)
            if (items[std::size_t(i)]->enabled) {
                select(env, i);
                break;
            }
        return true; // Up/Down never leave the list
    }
    if (!vertical && (vk == kVkLeft || vk == kVkRight)) {
        const float cx = selected >= 0 ? items[std::size_t(selected)]->x : (vk == kVkLeft ? 1e9f : -1e9f);
        int best = -1;
        float bd = 1e9f;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i]->enabled || int(i) == selected) continue;
            const float d = vk == kVkLeft ? cx - items[i]->x : items[i]->x - cx;
            if (d > 0 && d < bd) bd = d, best = int(i);
        }
        if (best >= 0) select(env, best);
        return true;
    }
    return false;
}

bool ItemList::key_up(GuiEnv& env, int vk) {
    if ((vk == kVkReturn || vk == kVkSpace) && selected >= 0) return items[std::size_t(selected)]->key_up(env, vk);
    return false;
}

// ---- ButtonBar ----

void ButtonBar::add_button(std::unique_ptr<TextButton> b, int a) {
    if (auto* arrow = dynamic_cast<ScrollArrow*>(b.get())) arrow->bar_index = int(items.size());
    align.push_back(a);
    add(std::move(b));
}

void ButtonBar::draw(OverlayFrame& out, GuiEnv& env) {
    if (w <= 0 || h <= 0) return;
    // CGuiButtonBar_Draw 1001b4a0.
    const float ew = sprite_w(env, "title_not_text_ending_off"), R = x + w, B = y + h;
    draw_named_sprite(out, env, "title_not_text_ending_off", x, y, x + ew, B);
    draw_named_sprite(out, env, "title_not_text_ending_off", R, y, R - ew - 1, B);
    float L = x + ew, Rp = R - ew - 34;
    for (std::size_t i = 0; i < items.size(); ++i) {
        TextButton& b = *items[i];
        switch (align[i]) {
        case 0: b.x = L, b.y = y, L += 33, b.w = 34, b.h = 25, b.draw(out, env); break;
        case 1: b.x = Rp, b.y = y, Rp -= 34, b.w = 34, b.h = 25, b.draw(out, env); break;
        case 2: draw_named_sprite(out, env, "tile_not_text", L, y, L + 34, B), L += 34; break;
        case 3: draw_named_sprite(out, env, "tile_not_text", Rp, y, Rp + 34, B), Rp -= 34; break;
        default: b.w = 34, b.h = 25, b.draw(out, env); break;
        }
    }
    draw_named_sprite(out, env, "tile_not_text", L, y, Rp + 34, B);
}

void ButtonBar::set_focus(GuiEnv& env) {
    ItemList::set_focus(env);
    int best = -1;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i]->enabled && (best < 0 || items[i]->x < items[std::size_t(best)]->x)) best = int(i);
    if (best >= 0) select(env, best);
}

// ---- ScrollList ----

void ScrollList::draw(OverlayFrame& out, GuiEnv& env) {
    // 1001c8a0: title, the visible rows, the bar (the form draws the title and the bar a second time).
    if (title) title->x = x, title->y = y, title->w = w, title->h = 25, title->draw(out, env);
    const int t = std::max(top, 0), n = int(items.size()), end = std::min(n, t + visible_rows);
    for (int i = 0; i < n; ++i) {
        TextButton& r = *items[std::size_t(i)];
        if (i >= t && i < end) {
            r.x = x, r.y = y + 31 + float(i - t) * 28, r.w = w, r.h = 25;
            r.draw(out, env);
        } else {
            r.x = r.y = r.w = r.h = 0;
        }
    }
    const int rows = bar_below_all_rows ? visible_rows : std::min(n, visible_rows);
    if (bar) bar->x = x, bar->y = y + 34 + 28 * float(rows), bar->w = w, bar->h = 25, bar->draw(out, env);
}

void ScrollList::set_focus(GuiEnv& env) {
    select(env, top);
    state = 0;
}

void ScrollList::clear_rows() {
    items.clear();
    paths.clear();
    top = 0, selected = -1;
    update_arrows();
}

void ScrollList::add_row(GuiEnv*, std::string row_text, std::string path, bool row_enabled) {
    auto b = std::make_unique<TextButton>();
    b->text = std::move(row_text);
    b->flash = true;
    b->enable(row_enabled);
    add(std::move(b));
    paths.push_back(std::move(path));
    update_arrows();
}

void ScrollList::scroll(int dir) {
    if (dir < 0 && top > 0) --top;
    if (dir > 0 && top + visible_rows < int(items.size())) ++top;
    update_arrows();
}

void ScrollList::update_arrows() {
    if (up) up->enable(top > 0);
    if (down) down->enable(int(items.size()) > top + visible_rows);
}

} // namespace parkan::shell
