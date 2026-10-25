// Form 44 redefine_controls (ctor 10003dd0; listener 10004080; Load 10004870, Save 100042e0): the key
// binding tables ui_other / ui_hq / ui_bots / ui_hero.man (defaults: the *_d.man files) shown through
// command.dsc and scancode.dsc, with capture, conflicts and the per-tab defaults (CKeyBindList 1001fe10,
// CKeyBindRow 1001f3b0). Spec: notes/shell.md "Redefine controls".
//
// Port decisions:
// - Bindings are kept by name (CMD_*, SCAN_*), as the .man files spell them.
// - The edited ui_*.man files and the four merged in-game lists (addition.man, table_2.man, table_1.man,
//   hero.man; CFormRedefineControls_BuildGameTables 10004fb0, input/man_tables) are written to the user's
//   folder (Shell::Config::user_dir), never into the game folder; missions load them from there
//   (input::load_game_bindings).
#include "core/paths.hpp"
#include "input/man_tables.hpp"
#include "shell/shell.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

namespace parkan::shell {

namespace {

constexpr int kVkDelete = 0x2e, kVkPause = 0x13, kVkLWin = 0x5b, kVkRWin = 0x5c, kVkApps = 0x5d;

struct Row {
    std::string cmd, modifier = "SCAN_NULL", key = "SCAN_NULL";
};
using Table = std::vector<Row>;

const char* kFiles[8] = {"ui_other.man", "ui_hq.man", "ui_bots.man", "ui_hero.man",
                         "ui_other_d.man", "ui_hq_d.man", "ui_bots_d.man", "ui_hero_d.man"};

bool is_axis(const std::string& k) { return k == "SCAN_MOUSE_X" || k == "SCAN_MOUSE_Y" || k == "SCAN_JOY_X" || k == "SCAN_JOY_Y"; }

// `NAME <whitespace> text` lines (command.dsc / scancode.dsc).
std::map<std::string, std::string> parse_dsc(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::size_t b = line.find_first_of(" \t", a);
        const std::string name = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        std::string desc;
        if (b != std::string::npos) {
            const std::size_t c = line.find_first_not_of(" \t", b);
            if (c != std::string::npos) desc = line.substr(c);
            while (!desc.empty() && (desc.back() == ' ' || desc.back() == '\t')) desc.pop_back();
        }
        out[name] = desc;
    }
    return out;
}

Table parse_man(const std::string& text) {
    Table t;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        Row r;
        if (!(ls >> r.cmd >> r.modifier >> r.key)) continue;
        t.push_back(r);
    }
    return t;
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

class KeyBindRow : public TextButton {
public:
    std::string key_text;
    bool capturing = false, conflict = false, editable = true, unbound = false;
    void draw(OverlayFrame& out, GuiEnv& env) override {
        if (w <= 0 || h <= 0) return;
        int s = state;
        if (capturing) s = std::fmod((env.now_ms - flash_ms) / 1000.0, 2.0 * interval(env)) > interval(env) ? 1 : 0; // the row blinks
        static const char* kLamp[3] = {"lamp_highlight", "lamp_normal", "lamp_off"};
        static const char* kTile[3] = {"tile_highlight", "tile", "tile_disabled"};
        static const char* kStub[3] = {"stub_on", "stub_normal", "stub_off"};
        const float lw = sprite_w(env, "lamp_highlight"), sw = sprite_w(env, "stub_on"), R = x + w, B = y + h;
        const float split = x + std::floor(w * 65 / 100);
        draw_named_sprite(out, env, kLamp[s], x, y, x + lw, B);
        draw_named_sprite(out, env, kLamp[s], R, y, R - lw - 1, B);
        draw_named_sprite(out, env, kStub[s], split, y, split - sw - 1, B);
        draw_named_sprite(out, env, kStub[s], split, y, split + sw, B);
        draw_named_sprite(out, env, kTile[s], x + lw, y, split - sw, B);
        draw_named_sprite(out, env, kTile[s], split + sw, y, R - lw, B);
        const TextFlash& tf = env.ctrls->text_flash;
        const std::uint32_t state_colour = text_colour(env);
        // CKeyBindRow_SetKeyTextColour 1001fd10: {state colour, X, off_bright}[capturing ? 2 : state].
        std::uint32_t c[3] = {state_colour, state_colour, tf.off_bright};
        if (!editable) c[0] = c[1] = c[2] = tf.off_bright;
        else if (conflict) c[1] = 0xffeb3232u;
        else if (unbound) c[1] = 0xff32aa32u;
        const std::uint32_t key_colour = c[capturing ? 2 : std::clamp(state, 0, 2)];
        const float pad = std::nearbyint(2 * env.sx()) / env.sx();
        const float ty = y + std::floor((h * env.sy() - GuiEnv::font_h(env.menu_font, env.vs)) / 2) / env.sy();
        ui::draw_text(out, env.vs, env.menu_font, x + lw + pad, ty, text, state_colour);
        ui::draw_text(out, env.vs, env.menu_font, split + sw + pad, ty, key_text, key_colour);
    }
};

// The four tab buttons in the title: CGuiAlertButton 100198a0 (`_on_alert` / `_off_alert` while the tab has a
// conflict).
class AlertButton : public ImageButton {
public:
    bool alert = false;
    void draw(OverlayFrame& out, GuiEnv& env) override {
        if (!alert) return ImageButton::draw(out, env);
        draw_named_sprite(out, env, prefix + (state != 0 ? "_off_alert" : "_on_alert"), x, y, x + w, y + h);
        if (!tooltip.empty() && hit(env.mouse_x, env.mouse_y)) env.tooltip_owner = this;
    }
};

// The title as a choice group: its tab buttons can be clicked (CGuiChoiceGroup 100cb030).
class TabTitle : public Title {
public:
    bool hit(float px, float py) const override {
        for (const auto& b : buttons)
            if (b->enabled && b->hit(px, py)) return true;
        return false;
    }
    bool lbutton_down(GuiEnv& env, float px, float py) override {
        for (std::size_t i = 0; i < buttons.size(); ++i)
            if (buttons[i]->enabled && buttons[i]->hit(px, py)) {
                env.play("BUTTON_CLICK");
                notify(env, {id, 0, int(i), 0});
                return true;
            }
        return false;
    }
};

class RedefineControlsForm : public Form {
public:
    explicit RedefineControlsForm(Shell& s) : Form(s, kRedefineControls, "Opts") {
        auto list = std::make_unique<ScrollList>();
        list->set_rect(42, 42, 590, 275);
        list->visible_rows = 12;
        list->bar_below_all_rows = true;
        list_ = add(std::move(list));
        auto b = std::make_unique<ButtonBar>();
        b->set_rect(42, 42, 590, 275);
        auto up = std::make_unique<ScrollArrow>();
        up->prefix = "scroll_up_button";
        auto down = std::make_unique<ScrollArrow>();
        down->prefix = "scroll_down_button";
        list_->up = up.get(), list_->down = down.get();
        auto img = [&](const char* prefix, int str) {
            auto i = std::make_unique<ImageButton>();
            i->prefix = prefix, i->tooltip = s.str(str), i->set_rect(0, 0, 34, 25);
            return i;
        };
        b->add_button(std::move(up), 1);
        b->add_button(std::move(down), 1);
        auto ok = img("ok_button", 6226);
        ok_ = ok.get();
        b->add_button(std::move(ok), 0);
        b->add_button(img("cancel_button", 6227), 0);
        b->add_button(img("setdef_button", 6229), 0);
        list_->bar = add(std::move(b));
        auto t = std::make_unique<TabTitle>();
        t->set_rect(42, 42, 590, 67);
        t->text = s.str(6121);
        // Tabs in creation order othbot, flybot, grbot, hero = tables 0..3 (drawn right to left).
        const char* prefixes[4] = {"othbot_btn", "flybot_btn", "grbot_btn", "hero_btn"};
        const int tips[4] = {6233, 6232, 6231, 6230};
        for (int i = 0; i < 4; ++i) {
            auto a = std::make_unique<AlertButton>();
            a->prefix = prefixes[i], a->tooltip = s.str(tips[i]), a->flash = false;
            tabs_[i] = a.get();
            t->buttons.push_back(std::move(a));
        }
        list_->title = add(std::move(t));
    }

    void on_show(GuiEnv& env) override {
        // CFormRedefineControls_Load 10004870: the dsc names and the eight tables (user copies first).
        const auto& game = shell_.config().game_dir;
        auto game_text = [&](const char* name) {
            auto p = resolve_path(game, name);
            return p ? read_text(*p) : std::string();
        };
        commands_ = parse_dsc(game_text("Command.dsc"));
        scans_ = parse_dsc(game_text("ScanCode.dsc"));
        for (int i = 0; i < 8; ++i) {
            const auto& dir = shell_.config().user_dir;
            std::error_code ec;
            const bool user = i < 4 && !dir.empty() && std::filesystem::exists(dir / kFiles[i], ec);
            tables_[i] = parse_man(user ? read_text(dir / kFiles[i]) : game_text(kFiles[i]));
        }
        capture_ = -1;
        show_table(3);
        set_focus_index(env, 0);
        list_->select(env, 0);
    }

    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender == list_->id) return begin_capture(env, c.value);
        if (c.sender == list_->bar->id) {
            switch (c.value) {
            case 0: list_->scroll(-1); break;
            case 1: list_->scroll(1); break;
            case 2:
                if (!validate()) save(), shell_.go_back(env);
                break;
            case 3: shell_.go_back(env); break;
            case 4: tables_[table_] = tables_[table_ + 4], show_table(table_); break; // CopyTable(t, t + 4)
            default: break;
            }
            return;
        }
        if (c.sender == list_->title->id && c.value >= 0 && c.value < 4) show_table(c.value);
    }

    bool key_down(GuiEnv& env, int vk) override {
        if (capture_ >= 0) {
            if (vk == kVkPause || vk == kVkLWin || vk == kVkRWin || vk == kVkApps) return true;
            if (!env.scan_name.empty()) apply_capture(env, env.scan_name); // any other key, Esc included
            return true;
        }
        if (vk == kVkDelete && focus() == list_->id && list_->selected >= 0) return clear_binding(env, list_->selected), true;
        return Form::key_down(env, vk);
    }
    bool lbutton_down(GuiEnv& env, float px, float py) override {
        if (capture_ >= 0) return apply_capture(env, "SCAN_LMOUSE"), true;
        return Form::lbutton_down(env, px, py);
    }
    bool rbutton_down(GuiEnv& env, float, float) override {
        if (capture_ >= 0) return apply_capture(env, "SCAN_RMOUSE"), true;
        if (list_->selected >= 0) clear_binding(env, list_->selected);
        return true;
    }
    bool mouse_move(GuiEnv& env, float px, float py) override { return capture_ >= 0 ? true : Form::mouse_move(env, px, py); }

private:
    std::string key_text(const std::string& scan) const {
        if (scan.empty() || scan == "SCAN_NULL") return shell_.str(6101); // "< пусто >"
        auto it = scans_.find(scan);
        return it != scans_.end() ? it->second : scan;
    }
    // CKeyBindList_ShowTable 10020920.
    void show_table(int t) {
        table_ = t;
        list_->clear_rows();
        for (const Row& r : tables_[t]) {
            auto row = std::make_unique<KeyBindRow>();
            auto it = commands_.find(r.cmd);
            row->text = it != commands_.end() ? it->second : r.cmd;
            row->editable = !is_axis(r.key);
            list_->add(std::move(row));
            list_->paths.push_back(r.cmd);
        }
        list_->update_arrows();
        refresh();
        for (int i = 0; i < 4; ++i) tabs_[i]->state = i == t ? 0 : 1;
    }
    // UpdateRowConflict 100210e0: rows with the same (modifier, key) in the searched tables.
    bool conflicted(int t, const Row& r) const {
        if (is_axis(r.key) || r.key == "SCAN_NULL") return false;
        int n = 0;
        auto count = [&](int k) {
            for (const Row& o : tables_[k])
                if (o.key == r.key && o.modifier == r.modifier) ++n;
        };
        if (t == 0) {
            for (int k = 0; k < 4; ++k) count(k);
        } else {
            count(t), count(0);
        }
        return n > 1;
    }
    void refresh() {
        for (std::size_t i = 0; i < list_->items.size() && i < tables_[table_].size(); ++i) {
            auto* row = static_cast<KeyBindRow*>(list_->items[i].get());
            const Row& r = tables_[table_][i];
            row->key_text = key_text(r.key);
            row->unbound = r.key == "SCAN_NULL";
            row->conflict = conflicted(table_, r);
        }
        validate();
    }
    // ValidateAll 10021350: a tab with a conflict gets its alert; any conflict disables OK.
    bool validate() {
        bool any = false;
        for (int t = 0; t < 4; ++t) {
            bool c = false;
            for (const Row& r : tables_[t]) c = c || conflicted(t, r);
            tabs_[t]->alert = c;
            any = any || c;
        }
        ok_->enable(!any);
        return any;
    }
    void begin_capture(GuiEnv& env, int row) {
        if (row < 0 || std::size_t(row) >= list_->items.size()) return;
        auto* r = static_cast<KeyBindRow*>(list_->items[std::size_t(row)].get());
        if (!r->editable) return;
        capture_ = row;
        r->capturing = true;
        r->flash_ms = env.now_ms;
        env.play("BUTTON_CLICK");
    }
    // ApplyCapturedKey 10020be0 + EndCapture 10020ff0: SetBinding(t, modifier 0, scan, cmd).
    void apply_capture(GuiEnv& env, const std::string& scan) {
        Row& r = tables_[table_][std::size_t(capture_)];
        r.modifier = "SCAN_NULL", r.key = scan;
        static_cast<KeyBindRow*>(list_->items[std::size_t(capture_)].get())->capturing = false;
        capture_ = -1;
        refresh();
        env.play("BUTTON_CLICK");
    }
    void clear_binding(GuiEnv& env, int row) {
        auto* r = static_cast<KeyBindRow*>(list_->items[std::size_t(row)].get());
        if (!r->editable) return;
        tables_[table_][std::size_t(row)].modifier = "SCAN_NULL";
        tables_[table_][std::size_t(row)].key = "SCAN_NULL";
        refresh();
        env.play("BUTTON_CLICK");
    }
    // CFormRedefineControls_Save 100042e0: tables 0–3 back to their .man files ("%s %s %s\n"), then the four
    // in-game lists merged from them (BuildGameTables).
    void save() {
        const auto& dir = shell_.config().user_dir;
        if (dir.empty()) return;
        std::array<input::ManTable, 4> tabs;
        for (int i = 0; i < 4; ++i) {
            std::string text;
            for (const Row& r : tables_[i]) text += r.cmd + ' ' + r.modifier + ' ' + r.key + '\n';
            std::ofstream(dir / kFiles[i], std::ios::binary) << text;
            tabs[std::size_t(i)] = input::parse_man(text, false); // no joystick
        }
        const auto game = input::build_game_tables(tabs);
        for (std::size_t i = 0; i < 4; ++i)
            std::ofstream(dir / input::kGameTableFiles[i], std::ios::binary) << input::write_man(game[i]);
    }

    ScrollList* list_ = nullptr;
    ImageButton* ok_ = nullptr;
    AlertButton* tabs_[4] = {};
    std::map<std::string, std::string> commands_, scans_;
    Table tables_[8];
    int table_ = 3, capture_ = -1;
};

} // namespace

std::unique_ptr<Form> make_redefine_controls_form(Shell& shell) { return std::make_unique<RedefineControlsForm>(shell); }

} // namespace parkan::shell
