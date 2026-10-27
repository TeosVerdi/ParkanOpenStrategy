// The in-mission help screen (CHelpScreen, ctor 10067700; F1 = CMD_HELP) with its three tabs "mission",
// "controls_help" and "message_history" (the pager's log), and the state the pager's F2 toggle needs.
// Spec: notes/help-pager.md; page scrolling and the rich-text markup: notes/shell.md §4.5–4.6.
//
// The class keeps the state (open tab, per-page scroll position, hold-to-scroll, blink) and the page
// sources; `frame()` lays one frame out in the 640×480 virtual space (what the tests check) and
// `draw_help_screen` draws it with the compaund.cfg skin.
//
// Port decisions:
// - The pages are laid out when drawn (CTextScroller wraps when a line is added, in the font of the
//   moment): the sources stay as text, so a font-set change re-wraps. The scroll position is a line
//   index, as the original's.
// - Time (blink 250 ms, hold-to-scroll 50 ms) is the real clock passed in, not the game clock: the game
//   time is frozen while the screen is open (the original uses timeGetTime).
#pragma once

#include "input/man_tables.hpp"
#include "ui/hq.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace parkan {
class TextRes;
}

namespace parkan::ui {

// The iron3d strings of the screen (cp1251), RT_STRING ids in comments.
struct HelpStrings {
    std::string tab[3];         // 5087 "Миссия и полезные советы", 5088 "Управление", 5090 "Сообщения"
    std::string controls_title; // 6209 "ЧАСТО ИСПОЛЬЗУЕМЫЕ КЛАВИШИ"
    std::string save_hint;      // 6210 "Для Записи игры, Восстановления или Выхода используйте клавишу %s."
    std::string section[4];     // ui_hero 6163, ui_bots 6161, ui_hq 6162, ui_other 6164
    std::string no_messages;    // 6167 "Нет сообщений"
    std::string message_n;      // 6168 "Сообщение %d"
    std::string close;          // 6169 "Закрыть" (the close box's tooltip)
    std::string no_tips = "No mission tips"; // the original's English literal (10103d0c), not localised
};
HelpStrings load_help_strings(const TextRes& iron3d);

// A line of a page (CTextLine 0xbc bytes: type 0 text, type 2 text + value).
struct HelpLine {
    std::string text, value; // `value` (key_value lines): the key text at x0 + 250
    std::uint32_t argb = 0xffffffffu;
    int align = 1; // 1 left, 2 centre, 3 right (honoured by the mission-tips page only)
    int spacing = 100;
    bool key_value = false;
};

// The sources of the controls page (FUN_10068da0): the four lists as the options menu keeps them and the
// display texts of command.dsc / scancode.dsc (cp1251), keyed by CMD_ / SCAN_ name.
struct ControlsSource {
    std::array<input::ManTable, 4> tables; // ui_hero.man, ui_bots.man, ui_hq.man, ui_other.man (table 3 is the interface list)
    std::map<std::string, std::string> commands, scans;
};
std::vector<HelpLine> build_controls_help(const ControlsSource& src, const HelpStrings& str);

// What a layout needs of the display: the game font on the virtual screen.
struct HelpMetrics {
    const VirtualScreen* vs = nullptr;
    const TftFont* font = nullptr;
};

// Layout constants (10068000), virtual px.
inline constexpr float kHelpTabX[3] = {2, 199, 396};
inline constexpr float kHelpTabY0 = 2, kHelpTabY1 = 23, kHelpTabW = 194; // [inferred] kHelpTabW (from the cell widths)
inline constexpr float kHelpCloseX0 = 593, kHelpCloseX1 = 640;
inline constexpr float kHelpPageX0 = 15, kHelpPageY0 = 35, kHelpPageX1 = 637, kHelpPageY1 = 430;
inline constexpr float kHelpBtnY0 = 458, kHelpBtnY1 = 477, kHelpBtnAX0 = 531, kHelpBtnBX0 = 581, kHelpBtnW = 50; // [inferred] kHelpBtnBX0 and kHelpBtnW (B's right edge)
inline constexpr float kHelpValueX = 250; // the key column of a key-value line, from x0
inline constexpr double kHelpBlinkMs = 250, kHelpRepeatMs = 50;

// One frame of the screen.
struct HelpFrame {
    struct Tab {
        std::string label;
        int lamp = 1;
        std::uint32_t argb = 0;
    } tabs[3];
    int close_lamp = 1;
    std::uint32_t close_argb = 0;
    bool close_hover = false; // the tooltip (strings.close) shows
    struct Button {
        int state = 0; // 0 disabled, 1 normal, 2 pressed / blink
    } scroll_up, scroll_down;
    struct Text {
        std::string text;
        float x = 0, y = 0;
        std::uint32_t argb = 0;
    };
    std::vector<Text> texts; // the page's rows
};

class HelpScreen {
public:
    enum Tab { Mission = 0, Controls = 1, Messages = 2, kTabs = 3 };
    enum class Key { Tab, Left, Right, Up, Down }; // VK_TAB / VK_LEFT / VK_RIGHT / VK_UP / VK_DOWN

    HelpScreen();
    void set_strings(HelpStrings s);
    // LoadMissionTips 10069750: the mission's mistips.mis (cp1251) when it exists.
    void set_mission_tips(std::optional<std::string> text) { tips_ = std::move(text); }
    void set_controls(std::vector<HelpLine> lines) { controls_ = std::move(lines); }
    // FUN_100688c0: one logged pager message, `source` = the header text ("от: инструктора").
    void log_message(std::string source, std::string body);
    std::size_t message_count() const { return messages_.size(); }

    bool visible() const { return visible_; }
    // FUN_10067c60 / FUN_10067d00. The tab stays across opens; the hold flag is cleared on open.
    void open();
    void close() { visible_ = false; }
    int tab() const { return tab_; }
    // Debug aid (--help-tab): select a tab.
    void set_tab(int t) { tab_ = ((t % kTabs) + kTabs) % kTabs; }

    // The key listener (10067d90): true when the key is taken; false when not visible or another key.
    bool key_down(Key k);
    // WM_MOUSEMOVE's position (the original reads the cursor in the draw function).
    void mouse_move(float vx, float vy) { mx_ = vx, my_ = vy; }
    float mouse_x() const { return mx_; }
    float mouse_y() const { return my_; }
    // LButtonDown 10067e60: true when taken. The close box closes the screen.
    bool lbutton_down(float vx, float vy);
    void lbutton_up() { hold_ = false; }
    // WM_MOUSEWHEEL 10067fb0: delta < 0 scrolls down two lines, else up two; true while visible.
    bool wheel(int delta);

    // The container's scroll functions on the current page (vtbl +0x1c / +0x20 / +0x24 / +0x28).
    void scroll_lines(int n);
    void scroll_back(int n);
    bool can_scroll_down() const { return first_[tab_] < lines_[tab_] - drawn_[tab_]; }
    bool can_scroll_up() const { return first_[tab_] > 0; }
    int first_line() const { return first_[tab_]; }

    // The page `t` as lines (what AddLine / AddKeyValueLine were given).
    std::vector<HelpLine> page(int t, const HelpMetrics& m) const;
    // 10068000: blink, the layout, then the hold-to-scroll step (which shows in the next frame).
    HelpFrame frame(const HelpMetrics& m, double now_ms);

private:
    bool over(float x0, float y0, float x1, float y1) const { return mx_ >= x0 && mx_ < x1 && my_ >= y0 && my_ < y1; }

    HelpStrings str_;
    std::optional<std::string> tips_;
    std::vector<HelpLine> controls_;
    struct Message {
        std::string source, body;
    };
    std::vector<Message> messages_;
    bool visible_ = false, hold_ = false, blink_ = false;
    int tab_ = 0;
    int first_[kTabs] = {}, lines_[kTabs] = {}, drawn_[kTabs] = {};
    double blink_ms_ = 0, hold_ms_ = 0;
    float mx_ = 320, my_ = 240; // until the first pointer event (the screen's centre)
};

// The transient pager panel and its F2 toggle (CMD_PAGER 0x2D9; g_PagerVisible 1010b080, widget DAT_1010b07c,
// spec §3): a message creates the widget and shows it; the widget is deleted 20 s after it was made
// (UpdateDrawPagerTimeout 1007f910) and then F2 does nothing; F2 / Esc hide it, F2 shows it again while it exists.
class PagerPanel {
public:
    // PushPagerHistory's end / CreatePagerFromHistory: the new message replaces the widget and shows it.
    void post(PagerMessage m) { msg_ = std::move(m), visible_ = true; }
    // FUN_1007fb50 / FUN_1007fb40.
    void toggle() { visible_ = visible_ ? false : msg_.has_value(); }
    void hide() { visible_ = false; }
    // UpdateDrawPagerTimeout: the widget goes at 20 s.
    void update(double now_ms) {
        if (msg_ && now_ms - msg_->shown_ms >= kPagerLifetimeMs) msg_.reset(), visible_ = false;
    }
    bool visible() const { return visible_ && msg_; }
    // The message to draw, when shown.
    const PagerMessage* shown() const { return visible() ? &*msg_ : nullptr; }

private:
    std::optional<PagerMessage> msg_;
    bool visible_ = false;
};

// Draws the frame (frame, tabs, close box, page, buttons) over the world.
void draw_help_screen(OverlayFrame& out, const HudContext& c, const HqSprites& sp, const HelpFrame& f);

} // namespace parkan::ui
