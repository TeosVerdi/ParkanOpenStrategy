// Help screen tests (notes/help-pager.md): tab switching, scrolling limits, the message history, the mouse, the
// pager's F2 toggle — on synthetic data — and the controls page built from the shipped .man files. Linked
// into parkan-tests.
#include "core/bytes.hpp"
#include "core/paths.hpp"
#include "input/keymap.hpp"
#include "input/scancodes.hpp"
#include "res/textres.hpp"
#include "ui/help_screen.hpp"

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

HelpStrings fake_strings() {
    HelpStrings s;
    s.tab[0] = "Mission", s.tab[1] = "Controls", s.tab[2] = "Messages";
    s.no_messages = "NONE", s.message_n = "Msg %d", s.close = "Close";
    return s;
}

struct Fixture {
    TftFont font = fake_font();
    VirtualScreen vs{640, 480, 1};
    HelpMetrics m{&vs, &font};
    HelpScreen h;
    Fixture() { h.set_strings(fake_strings()); }
    HelpFrame frame(double ms = 1000) { return h.frame(m, ms); }
};

std::vector<HelpLine> plain_lines(int n) {
    std::vector<HelpLine> v;
    for (int i = 0; i < n; ++i) {
        HelpLine l;
        l.text = "line " + std::to_string(i);
        v.push_back(l);
    }
    return v;
}

void test_tabs() {
    Fixture f;
    CHECK(!f.h.visible() && !f.h.key_down(HelpScreen::Key::Tab) && f.h.tab() == 0);
    f.h.open();
    CHECK(f.h.visible() && f.h.tab() == 0);
    // Tab / Right step forward, Left back, both wrap over the three pages (10067d90).
    CHECK(f.h.key_down(HelpScreen::Key::Tab) && f.h.tab() == 1);
    CHECK(f.h.key_down(HelpScreen::Key::Right) && f.h.tab() == 2);
    CHECK(f.h.key_down(HelpScreen::Key::Right) && f.h.tab() == 0);
    CHECK(f.h.key_down(HelpScreen::Key::Left) && f.h.tab() == 2);
    // The last tab stays selected across closing and opening.
    f.h.close();
    CHECK(!f.h.visible());
    f.h.open();
    CHECK(f.h.tab() == 2);
    // The tab labels and the current tab's look: lamp 2 white; the others lamp 1 green.
    const HelpFrame fr = f.frame();
    CHECK(fr.tabs[0].label == "Mission" && fr.tabs[2].label == "Messages");
    CHECK(fr.tabs[2].lamp == 2 && fr.tabs[2].argb == 0xffffffffu);
    CHECK(fr.tabs[0].lamp == 1 && fr.tabs[0].argb == 0xff00ff00u);
    CHECK(fr.close_lamp == 1 && fr.close_argb == 0xff00ff00u);
}

void test_scroll_limits() {
    Fixture f;
    f.h.set_controls(plain_lines(100));
    f.h.set_tab(HelpScreen::Controls);
    f.h.open();
    // Before the first layout nothing is known: no scrolling.
    f.h.scroll_lines(1);
    CHECK(f.h.first_line() == 0);
    f.frame();
    HelpFrame fr = f.frame(); // the buttons read the rows drawn by the frame before
    CHECK(fr.scroll_up.state == 0 && fr.scroll_down.state == 1); // up disabled at the top
    // Lines are 7 px (spacing 100) from y 35 while y < 430: 57 drawn.
    CHECK(fr.texts.size() == 57 && near(fr.texts[0].y, 35) && near(fr.texts[1].y, 42) && near(fr.texts[0].x, 15));
    // ScrollLines only while first + n <= lines − drawn: 100 − 57 = 43.
    for (int i = 0; i < 200; ++i) f.h.key_down(HelpScreen::Key::Down), f.frame();
    CHECK(f.h.first_line() == 43 && !f.h.can_scroll_down() && f.h.can_scroll_up());
    fr = f.frame();
    CHECK(fr.texts.size() == 57 && fr.texts[0].text == "line 43" && fr.scroll_down.state == 0 && fr.scroll_up.state == 1);
    // The wheel moves two lines (down for a negative delta); the ends hold.
    CHECK(f.h.wheel(120) && f.h.first_line() == 41);
    CHECK(f.h.wheel(-120) && f.h.first_line() == 43);
    CHECK(f.h.wheel(-120) && f.h.first_line() == 43);
    for (int i = 0; i < 200; ++i) f.h.key_down(HelpScreen::Key::Up);
    CHECK(f.h.first_line() == 0 && !f.h.can_scroll_up());
    f.h.wheel(120);
    CHECK(f.h.first_line() == 0);
    // A page shorter than the area does not scroll.
    f.h.set_controls(plain_lines(10));
    f.frame();
    f.h.scroll_lines(1);
    CHECK(f.h.first_line() == 0 && !f.h.can_scroll_down());
    // Each page keeps its own position.
    f.h.set_controls(plain_lines(100));
    f.frame();
    f.h.scroll_lines(5);
    f.h.set_tab(HelpScreen::Messages);
    CHECK(f.h.first_line() == 0);
    f.h.set_tab(HelpScreen::Controls);
    CHECK(f.h.first_line() == 5);
    // Not visible: the wheel is not taken.
    f.h.close();
    CHECK(!f.h.wheel(-120));
}

void test_history() {
    Fixture f;
    f.h.set_tab(HelpScreen::Messages);
    f.h.open();
    // No message yet: the spacer and "no messages" (yellow, spacing 150: a 10-px pitch).
    HelpFrame fr = f.frame();
    CHECK(fr.texts.size() == 2 && fr.texts[1].text == "NONE" && fr.texts[1].argb == 0xffffff00u && near(fr.texts[1].y, 45));
    f.h.log_message("from A", "first body");
    f.h.log_message("from B", "second body");
    CHECK(f.h.message_count() == 2);
    // Oldest first, numbered from 1: a yellow header "Msg n source", the body rows (white), a spacer between.
    fr = f.frame();
    std::vector<std::string> want = {" ", "Msg 1 from A", "first body", " ", "Msg 2 from B", "second body", " "};
    std::vector<std::string> got;
    for (const auto& t : fr.texts) got.push_back(t.text);
    CHECK(got.size() == want.size());
    for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
        if (want[i] == " ") CHECK(got[i].find_first_not_of(' ') == std::string::npos);
        else CHECK(got[i] == want[i]);
    }
    CHECK(fr.texts.size() > 5 && fr.texts[1].argb == 0xffffff00u && fr.texts[2].argb == 0xffffffffu && fr.texts[4].argb == 0xffffff00u);
    // A long body wraps at 628 px into rows that are lines of their own.
    f.h.log_message("from C", std::string(40, 'Q') + ' ' + std::string(40, 'R') + ' ' + std::string(40, 'S'));
    fr = f.frame();
    int rows = 0;
    for (const auto& t : fr.texts)
        if (t.text.find_first_of("QRS") != std::string::npos) ++rows;
    CHECK(rows == 2); // about 100 characters fit per row: Q…R…, then S…
    CHECK(f.h.message_count() == 3);
}

void test_pages() {
    Fixture f;
    f.h.open();
    // No mistips.mis: the three yellow fallback lines.
    HelpFrame fr = f.frame();
    CHECK(fr.texts.size() == 3 && fr.texts[1].text == "No mission tips" && fr.texts[1].argb == 0xffffff00u);
    // Tips: rich text; a pop takes effect from the line after the one it starts, so "pad" is still centred.
    f.h.set_mission_tips(std::string("<<align=center>>ab<<br>><</align>>pad<<br>>left<<br>>"));
    fr = f.frame();
    CHECK(fr.texts.size() == 3 && near(fr.texts[0].x, 15 + std::round(622.f / 2 - 12.f / 2)) && near(fr.texts[1].x, 15 + std::round(622.f / 2 - 18.f / 2)) &&
          near(fr.texts[2].x, 15));
    // A key-value line: the text at x0, the key at x0 + 250, both a font line below the slot top (7 + 1).
    HelpLine kv;
    kv.text = "Fire", kv.value = "SPACE", kv.key_value = true, kv.spacing = 150;
    f.h.set_controls({kv, kv});
    f.h.set_tab(HelpScreen::Controls);
    fr = f.frame();
    CHECK(fr.texts.size() == 4 && near(fr.texts[0].x, 15) && near(fr.texts[1].x, 265) && near(fr.texts[0].y, 43) && near(fr.texts[2].y, 53));
}

void test_mouse() {
    Fixture f;
    f.h.set_controls(plain_lines(100));
    f.h.set_tab(HelpScreen::Controls);
    CHECK(!f.h.lbutton_down(10, 10)); // not visible
    f.h.open();
    // A tab (rect x0..x0 + 194, y 2..23).
    CHECK(f.h.lbutton_down(300, 10) && f.h.tab() == 1);
    CHECK(f.h.lbutton_down(2, 2) && f.h.tab() == 0);
    CHECK(f.h.lbutton_down(396 + 193, 22) && f.h.tab() == 2);
    CHECK(!f.h.lbutton_down(196, 10)); // the 3-px gap between the tabs falls through
    f.h.set_tab(HelpScreen::Controls);
    // Hovering: lamp 2 / white while the blink is on, lamp 1 / light green while it is off.
    f.h.mouse_move(100, 10);
    HelpFrame a = f.frame(1000), b = f.frame(1300);
    CHECK((a.tabs[0].lamp == 2) != (b.tabs[0].lamp == 2));
    for (const HelpFrame* fr : {&a, &b}) CHECK(fr->tabs[0].lamp == 2 ? fr->tabs[0].argb == 0xffffffffu : fr->tabs[0].argb == 0xff64ff64u);
    CHECK(a.tabs[2].lamp == 1 && a.tabs[2].argb == 0xff00ff00u);
    // Hold-to-scroll: the scroll-down button (581..631 × 458..477) sets the hold flag; every 50 ms over it the page
    // moves a line; leaving the button ends it.
    CHECK(f.h.lbutton_down(600, 470));
    f.frame(2000); // the first repeat after > 50 ms since the last (0)
    CHECK(f.h.first_line() == 1);
    f.frame(2020);
    CHECK(f.h.first_line() == 1);
    HelpFrame held = f.frame(2060);
    CHECK(f.h.first_line() == 2 && held.scroll_down.state == 2 && held.scroll_up.state == 2);
    f.h.lbutton_up();
    f.frame(2200);
    CHECK(f.h.first_line() == 2);
    // The scroll-up button scrolls back.
    CHECK(f.h.lbutton_down(540, 470));
    f.frame(3000);
    CHECK(f.h.first_line() == 1);
    f.h.mouse_move(10, 300); // away from the buttons: the hold ends
    f.frame(3100);
    f.frame(3200);
    CHECK(f.h.first_line() == 1);
    // The close box (593..640 × 2..23): the tooltip while hovered, a click closes.
    f.h.mouse_move(600, 10);
    CHECK(f.frame(4000).close_hover);
    CHECK(f.h.lbutton_down(600, 10) && !f.h.visible());
    // Opening again clears the hold flag.
    f.h.open();
    CHECK(f.h.lbutton_down(600, 470));
    f.h.close();
    f.h.open();
    f.h.mouse_move(10, 300);
    f.frame(5000);
    CHECK(f.h.first_line() == 1);
    // Outside everything the click falls through.
    CHECK(!f.h.lbutton_down(100, 300));
}

void test_layout_keys() {
    // The vehicle lists show the keys the mission uses: under the Modern layout forward is W (the base key only, no
    // Shift twin), under Classic the shipped arrow.
    input::ManTable t = input::parse_man("CMD_OBJ_MOVE_FORWARD SCAN_NULL SCAN_G_UP\n", false);
    CHECK(t.size() == 1);
    ControlsSource src;
    src.commands["CMD_OBJ_MOVE_FORWARD"] = "Forward";
    src.scans["SCAN_W"] = "W", src.scans["SCAN_G_UP"] = "Up";
    src.tables[0] = t;
    input::apply_layout(src.tables[0], input::KeyLayout::Modern);
    CHECK(src.tables[0].size() == 1);
    auto count_rows = [&](const std::vector<HelpLine>& v, const std::string& key) {
        int n = 0;
        for (const auto& l : v) n += l.key_value && l.text == "Forward" && l.value == key;
        return n;
    };
    HelpStrings str;
    CHECK(count_rows(build_controls_help(src, str), "W") == 1);
    src.tables[0] = t;
    input::apply_layout(src.tables[0], input::KeyLayout::Classic);
    CHECK(count_rows(build_controls_help(src, str), "Up") == 1);
}

void test_pager() {
    PagerPanel p;
    CHECK(!p.visible() && !p.shown());
    p.toggle(); // nothing to show
    CHECK(!p.visible());
    PagerMessage m;
    m.header = "h", m.body = "b", m.shown_ms = 1000;
    p.post(m);
    CHECK(p.visible() && p.shown() && p.shown()->body == "b");
    p.toggle(); // F2 hides
    CHECK(!p.visible());
    p.toggle(); // and shows the latest again while it exists
    CHECK(p.visible());
    p.hide(); // Esc
    CHECK(!p.visible());
    p.update(1000 + kPagerLifetimeMs - 1);
    p.toggle();
    CHECK(p.visible());
    p.update(1000 + kPagerLifetimeMs); // the widget is deleted at 20 s: F2 does nothing
    CHECK(!p.visible());
    p.toggle();
    CHECK(!p.visible());
    // A new message replaces it and shows it, even after a hide.
    PagerMessage n = m;
    n.body = "c", n.shown_ms = 30000;
    p.post(n);
    p.hide();
    p.post(n);
    CHECK(p.visible() && p.shown()->body == "c");
}

std::string text_of(const std::filesystem::path& p) {
    const Bytes b = read_file(p);
    return std::string(b.begin(), b.end());
}

void test_corpus_controls(const std::filesystem::path& game) {
    ControlsSource src;
    const char* files[4] = {"ui_hero.man", "ui_bots.man", "ui_hq.man", "ui_other.man"};
    std::size_t rows[4] = {};
    for (int i = 0; i < 4; ++i) {
        const auto p = resolve_path(game, files[i]);
        CHECK(p.has_value());
        if (!p) return;
        src.tables[std::size_t(i)] = input::parse_man(text_of(*p), false);
        rows[i] = src.tables[std::size_t(i)].size();
    }
    // notes/shell.md "Rows per tab": 8, 13, 8 and 44 rows.
    CHECK(rows[0] == 8 && rows[1] == 13 && rows[2] == 8 && rows[3] == 44);
    const auto cmd = resolve_path(game, "Command.dsc"), scan = resolve_path(game, "ScanCode.dsc");
    CHECK(cmd && scan);
    if (!cmd || !scan) return;
    src.commands = input::parse_dsc(text_of(*cmd)), src.scans = input::parse_dsc(text_of(*scan));
    HelpStrings str;
    str.controls_title = "TITLE", str.save_hint = "Save with %s.";
    for (int i = 0; i < 4; ++i) str.section[i] = "SECTION" + std::to_string(i);
    const auto lines = build_controls_help(src, str);
    // The title, a blank, the three key rows, the sentence; then each section: header, blank, its rows, four blanks.
    CHECK(lines.size() > 5 && lines[0].text == "TITLE" && lines[0].argb == 0xffffff00u && !lines[1].key_value);
    CHECK(lines[2].key_value && lines[3].key_value && lines[4].key_value && lines[2].argb == 0xffffffffu && lines[2].spacing == 150);
    CHECK(lines[2].text == src.commands["CMD_HELP"] && lines[3].text == src.commands["CMD_GAME_MENU"] &&
          lines[4].text == src.commands["CMD_JAMES_MISSION_OBJ"]);
    CHECK(lines[2].value == src.scans["SCAN_F1"] && lines[3].value == src.scans["SCAN_F3"] && lines[4].value == src.scans["SCAN_F12"]);
    CHECK(!lines[2].value.empty() && lines[6].text == "Save with " + lines[3].value + ".");
    std::vector<std::size_t> header_at;
    std::size_t key_rows = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].text.rfind("SECTION", 0) == 0) header_at.push_back(i);
        key_rows += lines[i].key_value;
    }
    CHECK(header_at.size() == 4 && key_rows == 3 + 8 + 13 + 8 + 44);
    for (std::size_t s = 0; s < header_at.size(); ++s) {
        CHECK(lines[header_at[s]].text == "SECTION" + std::to_string(s));
        std::size_t n = 0;
        for (std::size_t i = header_at[s] + 2; i < lines.size() && lines[i].key_value; ++i) ++n;
        CHECK(n == rows[s]);
    }
    // Every row has a command text and a key text from the .dsc files.
    for (const auto& l : lines)
        if (l.key_value) CHECK(!l.text.empty() && !l.value.empty());
}

void test_corpus_strings(const std::filesystem::path& game) {
    const auto p = resolve_path(game, "iron3d.dll");
    CHECK(p.has_value());
    if (!p) return;
    const HelpStrings s = load_help_strings(TextRes::from_pe(read_file(*p)));
    for (const auto& t : s.tab) CHECK(!t.empty());
    for (const auto& t : s.section) CHECK(!t.empty());
    CHECK(!s.controls_title.empty() && s.save_hint.find("%s") != std::string::npos && s.message_n.find("%d") != std::string::npos);
    CHECK(!s.no_messages.empty() && !s.close.empty());
    // The mission tips exist in the first tutorial.
    CHECK(resolve_path(game, "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.01/mistips.mis").has_value());
}

} // namespace

int help_unit_tests() {
    g_fail = 0;
    std::printf("help screen\n");
    test_tabs();
    test_scroll_limits();
    test_history();
    test_pages();
    test_mouse();
    test_layout_keys();
    test_pager();
    return g_fail;
}

int help_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("help screen corpus\n");
    test_corpus_controls(game);
    test_corpus_strings(game);
    return g_fail;
}
