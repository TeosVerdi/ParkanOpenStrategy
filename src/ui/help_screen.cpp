#include "ui/help_screen.hpp"

#include "core/bytes.hpp"
#include "input/scancodes.hpp"
#include "res/textres.hpp"
#include "shell/rich_text.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::ui {

namespace {

constexpr std::uint32_t kYellow = 0xffffff00u, kWhite = 0xffffffffu, kGreen = 0xff00ff00u, kLightGreen = 0xff64ff64u;

HelpLine line(std::string text, std::uint32_t argb, int spacing = 100) {
    HelpLine l;
    l.text = std::move(text), l.argb = argb, l.spacing = spacing;
    return l;
}

// WrapTextToLines(font, text, px): the width is in screen pixels, round(ScaleX · 0.98 · px) (hud_text.hpp).
std::vector<std::string> wrap(const HelpMetrics& m, const std::string& text, float px) {
    return wrap_text(*m.vs, *m.font, text, std::round(m.vs->sx() * 0.98f * px));
}

std::string replace_first(std::string text, std::string_view what, const std::string& with) {
    if (const auto p = text.find(what); p != std::string::npos) text.replace(p, what.size(), with);
    return text;
}

} // namespace

HelpStrings load_help_strings(const TextRes& t) {
    auto s = [&](std::uint32_t id) { return utf8_to_cp1251(t.get(id)); };
    HelpStrings h;
    h.tab[0] = s(5087), h.tab[1] = s(5088), h.tab[2] = s(5090);
    h.controls_title = s(6209), h.save_hint = s(6210);
    h.section[0] = s(6163), h.section[1] = s(6161), h.section[2] = s(6162), h.section[3] = s(6164);
    h.no_messages = s(6167), h.message_n = s(6168), h.close = s(6169);
    return h;
}

std::vector<HelpLine> build_controls_help(const ControlsSource& src, const HelpStrings& str) {
    auto command_text = [&](std::uint32_t cmd) {
        const std::string name(input::man_command_name(cmd));
        const auto it = src.commands.find(name);
        return it != src.commands.end() ? it->second : name;
    };
    // FUN_100617c0: the key's text from scancode.dsc.
    auto key_text = [&](std::uint16_t key) {
        const std::string name(input::scan_name(key));
        const auto it = src.scans.find(name);
        return it != src.scans.end() ? it->second : name;
    };
    auto key_row = [&](const input::ManRow& r) {
        HelpLine l = line(command_text(r.cmd), kWhite, 150);
        l.value = key_text(r.key), l.key_value = true;
        return l;
    };
    auto find_row = [&](std::string_view command) -> const input::ManRow* { // CKeyBindings_FindRow(table 3, id)
        const auto id = input::man_command(command);
        if (id)
            for (const auto& r : src.tables[3])
                if (r.cmd == *id) return &r;
        return nullptr;
    };
    const std::string blank = " ";
    std::vector<HelpLine> out;
    // Block 1: HELP, the game menu and the objectives, then the sentence naming the game menu's key.
    out.push_back(line(str.controls_title, kYellow));
    out.push_back(line(blank, kYellow));
    std::string menu_key;
    for (const char* c : {"CMD_HELP", "CMD_GAME_MENU", "CMD_JAMES_MISSION_OBJ"})
        if (const input::ManRow* r = find_row(c)) {
            out.push_back(key_row(*r));
            if (std::string_view(c) == "CMD_GAME_MENU") menu_key = key_text(r->key);
        }
    // [inferred] 6210 is added in 8 AddLine calls in the original (white ×3, 0xFFE6E6E6, white ×4; which piece
    // goes where is open): here a blank line before it (the key rows are drawn a font line low, 1006c670), one
    // white line, which the drawer wraps, and a blank spacer line after it.
    out.push_back(line(blank, kWhite));
    out.push_back(line(replace_first(str.save_hint, "%s", menu_key), kWhite));
    out.push_back(line(blank, kYellow));
    // Block 2: a section per list, headers 6163 / 6161 / 6162 / 6164 for ui_hero / ui_bots / ui_hq / ui_other.
    for (int t = 0; t < 4; ++t) {
        out.push_back(line(str.section[t], kYellow));
        out.push_back(line(blank, kYellow));
        for (const auto& r : src.tables[std::size_t(t)]) out.push_back(key_row(r));
        for (int k = 0; k < 4; ++k) out.push_back(line(blank, kYellow));
    }
    return out;
}

HelpScreen::HelpScreen() = default;

void HelpScreen::set_strings(HelpStrings s) { str_ = std::move(s); }

void HelpScreen::log_message(std::string source, std::string body) { messages_.push_back({std::move(source), std::move(body)}); }

void HelpScreen::open() {
    if (visible_) return;
    visible_ = true, hold_ = false; // Show(current): the last used tab stays
}

bool HelpScreen::key_down(Key k) {
    if (!visible_) return false;
    switch (k) {
    case Key::Tab: case Key::Right: tab_ = (tab_ + 1) % kTabs; break;
    case Key::Left: tab_ = (tab_ + kTabs - 1) % kTabs; break;
    case Key::Up: scroll_back(1); break;
    case Key::Down: scroll_lines(1); break;
    }
    return true;
}

bool HelpScreen::lbutton_down(float vx, float vy) {
    if (!visible_) return false;
    mx_ = vx, my_ = vy;
    for (int i = 0; i < kTabs; ++i)
        if (over(kHelpTabX[i], kHelpTabY0, kHelpTabX[i] + kHelpTabW, kHelpTabY1)) return tab_ = i, true;
    // OnUICommandDown(0x2ED); its stdClearKeyboard is covered by the app forwarding key releases while the screen is open.
    if (over(kHelpCloseX0, kHelpTabY0, kHelpCloseX1, kHelpTabY1)) return close(), true;
    if (over(kHelpBtnAX0, kHelpBtnY0, kHelpBtnBX0, kHelpBtnY1) || over(kHelpBtnBX0, kHelpBtnY0, kHelpBtnBX0 + kHelpBtnW, kHelpBtnY1))
        return hold_ = true, true; // the scroll itself happens in the next draw
    return false;
}

bool HelpScreen::wheel(int delta) {
    if (!visible_) return false;
    for (int i = 0; i < 2; ++i) delta < 0 ? scroll_lines(1) : scroll_back(1);
    return true;
}

void HelpScreen::scroll_lines(int n) {
    // nLines counts lines, rowsDrawn counts wrapped rows: the spec states the limit so (1006c050), and the credits'
    // StepPixel / IsActive (shell.md §4.6) mix the units the same way, so it is kept as compiled.
    if (first_[tab_] + n <= lines_[tab_] - drawn_[tab_]) first_[tab_] += n; // 1006c050
}

void HelpScreen::scroll_back(int n) {
    if (first_[tab_] - n >= 0) first_[tab_] -= n; // 1006c080
}

std::vector<HelpLine> HelpScreen::page(int t, const HelpMetrics& m) const {
    std::vector<HelpLine> out;
    if (t == Mission) {
        if (tips_) {
            // LayoutRichTextLines(font, text, 628).
            for (auto& r : shell::layout_rich_text(*tips_, *m.font, 628)) {
                HelpLine l = line(std::move(r.text), r.argb, r.spacing);
                l.align = r.align;
                out.push_back(std::move(l));
            }
        } else {
            out.push_back(line(std::string(15, ' '), kYellow));
            out.push_back(line(str_.no_tips, kYellow));
            out.push_back(line(std::string(15, ' '), kYellow));
        }
    } else if (t == Controls) {
        out = controls_;
    } else if (messages_.empty()) { // FUN_100687e0 + "Нет сообщений"
        out.push_back(line(std::string(13, ' '), kWhite, 150));
        out.push_back(line(str_.no_messages, kYellow, 150));
    } else {
        out.push_back(line(std::string(13, ' '), kWhite, 150));
        for (std::size_t i = 0; i < messages_.size(); ++i) {
            // "Сообщение %d" (the counter + 1 [inferred]), a space and the source; the body wrapped at 628, a row a line.
            out.push_back(line(replace_first(str_.message_n, "%d", std::to_string(i + 1)) + ' ' + messages_[i].source, kYellow, 150));
            for (auto& row : wrap(m, messages_[i].body, 628)) out.push_back(line(std::move(row), kWhite, 150));
            out.push_back(line(std::string(13, ' '), kWhite, 150));
        }
    }
    return out;
}

HelpFrame HelpScreen::frame(const HelpMetrics& m, double now_ms) {
    HelpFrame f;
    if (now_ms - blink_ms_ > kHelpBlinkMs) blink_ms_ = now_ms, blink_ = !blink_;
    // A tab or the close box: lamp 2 / white when current or hovered while the blink is on, lamp 1 / light green
    // hovered while it is off, else lamp 1 / green.
    auto look = [&](bool current, bool hover, int& lamp, std::uint32_t& argb) {
        if (current || (hover && blink_)) lamp = 2, argb = kWhite;
        else if (hover) lamp = 1, argb = kLightGreen;
        else lamp = 1, argb = kGreen;
    };
    for (int i = 0; i < kTabs; ++i) {
        f.tabs[i].label = str_.tab[i];
        look(i == tab_, over(kHelpTabX[i], kHelpTabY0, kHelpTabX[i] + kHelpTabW, kHelpTabY1), f.tabs[i].lamp, f.tabs[i].argb);
    }
    f.close_hover = over(kHelpCloseX0, kHelpTabY0, kHelpCloseX1, kHelpTabY1);
    look(false, f.close_hover, f.close_lamp, f.close_argb); // the current-index test uses 3: never selected
    // The scroll buttons: disabled, pressed while the hold flag is set, else normal (blink + 1 hovered).
    const bool over_a = over(kHelpBtnAX0, kHelpBtnY0, kHelpBtnBX0, kHelpBtnY1),
               over_b = over(kHelpBtnBX0, kHelpBtnY0, kHelpBtnBX0 + kHelpBtnW, kHelpBtnY1);
    auto state = [&](bool enabled, bool hover) { return !enabled ? 0 : hold_ ? 2 : hover ? int(blink_) + 1 : 1; };
    f.scroll_up.state = state(can_scroll_up(), over_a);
    f.scroll_down.state = state(can_scroll_down(), over_b);

    // CTextScroller_Draw on the page rect: from the first line while y < the bottom; a line is `rows` rows of
    // H = font height × spacing / 100 each. [inferred] The heights are scaled by 1/ScaleY (the flag at +0x28).
    const std::vector<HelpLine> lines = page(tab_, m);
    const float unit = m.vs->glyph_scale / m.vs->sy(), step = std::round(float(m.font->height + 1) * unit);
    const float width = kHelpPageX1 - kHelpPageX0;
    float y = kHelpPageY0;
    int drawn = 0;
    for (int i = first_[tab_]; i < int(lines.size()) && y < kHelpPageY1; ++i) {
        const HelpLine& l = lines[std::size_t(i)];
        const float h = float(m.font->height * l.spacing / 100) * unit;
        if (l.key_value) {
            // 1006c670: the text at (x0, y'), the key at (x0 + 250, y'); y' is a font line below the slot top [inferred].
            f.texts.push_back({l.text, kHelpPageX0, y + step, l.argb});
            f.texts.push_back({l.value, kHelpPageX0 + kHelpValueX, y + step, l.argb});
            y += h, ++drawn;
            continue;
        }
        const std::vector<std::string> rows = wrap(m, l.text, 640); // 1006c710 wraps again at 640
        float ry = y;
        for (const std::string& row : rows) {
            float x = kHelpPageX0;
            if (tab_ == Mission) { // 10069a40: the tips page honours the line's alignment
                const float tw = text_width_px(*m.vs, *m.font, row) / m.vs->sx();
                if (l.align == 2) x += std::round(width / 2 - tw / 2);
                else if (l.align == 3) x += std::round(width - tw);
            }
            f.texts.push_back({row, x, ry, l.argb});
            ry += step;
        }
        y += float(rows.size()) * h, drawn += int(rows.size());
    }
    lines_[tab_] = int(lines.size()), drawn_[tab_] = drawn;
    // Hold-to-scroll: every 50 ms over a button, else the hold ends.
    if (hold_ && now_ms - hold_ms_ > kHelpRepeatMs) {
        hold_ms_ = now_ms;
        if (over_a) scroll_back(1);
        else if (over_b) scroll_lines(1);
        else hold_ = false;
    }
    return f;
}

namespace {

// The compaund.cfg skin cells of the row cursor (RowCell_*), as hq_draw.cpp's Row: from (x, y) to the right, or
// to the left with each cell mirrored over (x, y)-(x − w − 1, y + 19) when `left`.
struct Cells {
    OverlayFrame& out;
    const HudContext& c;
    const HqSprites& sp;
    float x, y;
    bool left = false;

    void sprite(const char* name, float x0, float y0, float x1, float y1) const {
        if (const Sprite* s = sp.get(name)) draw_sprite(out, c.vs, c.tex, *s, x0, y0, x1, y1);
    }
    float sprite_w(const std::string& name) const {
        const Sprite* s = sp.get(name);
        return s ? s->w : 0.f;
    }
    void cell(const std::string& name, float w) {
        if (left) sprite(name.c_str(), x, y, x - w - 1, y + 19), x -= w;
        else sprite(name.c_str(), x, y, x + w, y + 19), x += w;
    }
    void ending(int n) { cell(n ? "ccres_ending_text" : "ccres_ending_stub", 5); } // RowCell_Ending
    void lamp_ending(int n) { // FUN_10099b00: off / normal / pressed
        static const char* k[3] = {"ccres_lamp_text_ending_off", "ccres_lamp_text_ending_normal", "ccres_lamp_text_ending_pressed"};
        cell(k[std::clamp(n, 0, 2)], 10);
    }
    void text_cell(float w, std::string_view t, std::uint32_t argb) { // RowCell_Text
        const float cx = left ? x - w : x;
        cell("ccres_body_text", w);
        const float tw = text_width_px(c.vs, *c.font.font, t) / c.vs.sx(), th = float(c.font.font->height + 1) * c.vs.glyph_scale / c.vs.sy();
        draw_text(out, c.vs, c.font, std::round((w - tw) * 0.5f + cx), std::round(y + (19 - th) / 2), t, argb);
    }
    // FUN_1009a980 (long_button, icon at +10, +2) and FUN_1009a200 (exit_button, icon at +15, +3).
    void button(const char* kind, int n, const char* icon) {
        static const char* suffix[3] = {"_off", "_normal", "_pressed"};
        const std::string frame = std::string("ccres_") + kind + suffix[std::clamp(n, 0, 2)];
        const float w = sprite_w(frame), cx = left ? x - w : x;
        cell(frame, w);
        const Sprite* s = sp.get(icon);
        if (!s) return;
        const bool exit = std::string_view(kind) == "exit_button";
        const float ix = exit ? 15.f : 10.f, iy = exit ? 3.f : 2.f;
        draw_sprite(out, c.vs, c.tex, *s, cx + ix, y + iy, cx + ix + s->w, y + iy + s->h);
    }
};

// FUN_1009ab90: corners, top / bottom edges, left / right edges.
void framed_panel(const Cells& k, float x0, float y0, float x1, float y1) {
    static const char* corner[4] = {"ccres_frame_corner_1", "ccres_frame_corner_2", "ccres_frame_corner_3", "ccres_frame_corner_4"};
    for (int r = 0; r < 4; ++r) {
        const float cx = r == 0 || r == 3 ? x0 : x1 - 8, cy = r < 2 ? y0 : y1 - 8;
        k.sprite(corner[r], cx, cy, cx + 8, cy + 8);
    }
    k.sprite("ccres_frame_edge_h", x0 + 8, y0, x1 - 7, y0 + 5);
    k.sprite("ccres_frame_edge_h", x0 + 8, y1, x1 - 7, y1 - 5);
    k.sprite("ccres_frame_edge_v", x0 + 5, y0 + 8, x0, y1 - 7);
    k.sprite("ccres_frame_edge_v", x1 - 5, y0 + 8, x1, y1 - 7);
}

} // namespace

void draw_help_screen(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HelpFrame& f) {
    if (!c.font.font) return;
    // Tabs: lamp ending, the 179-px text cell, then the mirrored lamp ending 5 px on (the hit rect is 194 wide).
    for (int i = 0; i < HelpScreen::kTabs; ++i) {
        Cells r{out, c, sp, kHelpTabX[i], kHelpTabY0};
        r.lamp_ending(f.tabs[i].lamp);
        r.text_cell(179, f.tabs[i].label, f.tabs[i].argb);
        r.left = true, r.x += 5;
        r.lamp_ending(f.tabs[i].lamp);
    }
    // The close box at (593, 2): lamp ending, the exit button (RowCell_ExitButton 1009a200) with exit_icon.
    {
        Cells r{out, c, sp, kHelpCloseX0, kHelpTabY0};
        r.lamp_ending(f.close_lamp);
        r.button("exit_button", f.close_lamp, "exit_icon");
    }
    // The content frame (DrawFramedPanel, fill 0xFF004000) and the page's rows.
    Cells k{out, c, sp, 0, 0};
    fill_rect(out, c.vs, 7, 29, 632, 449, 0xff004000u);
    framed_panel(k, 2, 24, 637, 454);
    for (const auto& t : f.texts) draw_text(out, c.vs, c.font, t.x, t.y, t.text, t.argb);
    // The bottom row at (0, 458): ending, a 526-px body stub, the scroll-up and scroll-down buttons, the ending mirrored.
    Cells r{out, c, sp, 0, kHelpBtnY0};
    r.ending(0);
    r.cell("ccres_body_stub", 526);
    r.button("long_button", f.scroll_up.state, "scroll_up_icon");
    r.button("long_button", f.scroll_down.state, "scroll_down_icon");
    r.left = true, r.x += 5; // [inferred] the right edge is about 636
    r.ending(0);
}

} // namespace parkan::ui
