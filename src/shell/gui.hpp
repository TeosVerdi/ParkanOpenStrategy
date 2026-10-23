// iron3d's shell GUI (the framework compiled into iron3d around 100c9000 and the shell widget classes
// 10013720–1001f850): widgets on the 640×480 virtual screen, drawn from the ui/shell_ctrls.cfg sprites in
// the TFT fonts, with the original focus, hover, keyboard and command rules. SDL-free: the app feeds it
// input (virtual mouse coordinates, Windows virtual-key codes, cp1251 characters) and a clock, and draws
// the OverlayFrame it fills. Spec: notes/shell.md "GUI framework" and "Widgets".
//
// Port decisions:
// - Text is centred vertically by its real glyph height ((h·ScaleY − fontH) / 2 screen px), which is the
//   original's formula at 640×480; the original scales that offset twice at other resolutions.
// - Sprite UVs follow CHudSprite_SetSource: (u + 0.5) / 256 … (u + w) / 256 (half-texel inset top-left).
#pragma once

#include "shell/shell_data.hpp"
#include "ui/hud_text.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace parkan::shell {

// Windows virtual-key codes the shell reacts to.
enum Vk : int {
    kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0d, kVkEscape = 0x1b, kVkSpace = 0x20,
    kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28,
};

// What the widgets need from the app: resources, the clock, the mouse and a sound queue.
struct GuiEnv {
    ui::VirtualScreen vs;
    ui::HudFont menu_font, game_font, tool_font;
    const ShellControls* ctrls = nullptr;
    std::function<std::int32_t(std::string_view)> texture; // lower-case page/background name → overlay slot (−1 none)
    double now_ms = 0;                                      // ITimer::GetTimeMs
    float mouse_x = -1000, mouse_y = -1000;                 // virtual
    std::vector<std::string> sounds;                        // menu_resources.cfg `sounds` keys to play (in order)
    struct Widget* tooltip_owner = nullptr;                 // set while drawing (Tooltip_GetHolder 10023b40)
    bool software_cursor = true;                            // false: the app shows the system cursor instead
    std::string scan_name;                                  // the pressed key's scancode.dsc name (SCAN_*), for key capture

    void play(std::string_view key) { sounds.emplace_back(key); }
    float sx() const { return vs.sx(); }
    float sy() const { return vs.sy(); }
    // A font's height and text width in screen pixels (GetHeight / GetTextWidth).
    static float font_h(const ui::HudFont& f, const ui::VirtualScreen& v) { return f.font ? float(f.font->height) * v.glyph_scale : 0; }
    float text_w(const ui::HudFont& f, std::string_view t) const { return f.font ? ui::text_width_px(vs, *f.font, t) : 0; }
};

// CHudSprite_Draw 1008fbc0 with a shell_ctrls.cfg sprite: stretched over (x0,y0)-(x1,y1); x1 < x0 mirrors.
void draw_named_sprite(OverlayFrame& out, const GuiEnv& env, std::string_view name, float x0, float y0, float x1, float y1);
// A texture region (u, v, w, h on a 256² page) over a rect.
void draw_texture(OverlayFrame& out, const GuiEnv& env, std::int32_t slot, float u, float v, float w, float h, float x0, float y0,
                  float x1, float y1, std::uint32_t argb = 0xffffffffu);
// The width of a sprite (its WIDTH key), 0 when missing.
float sprite_w(const GuiEnv& env, std::string_view name);

class Form;

// A command to a form's listener (the 0x30-byte message, vtbl 100e46c0).
struct Command {
    int sender = -1; // child index of the sender in the form (WidgetId.b); −1 = the form itself
    int code = 0;    // 0 activate / changed, 1 cancel (Esc)
    int value = 0;   // CGuiVariant int: row / bar button index, 0 for buttons
    int extra = 0;   // +0x2c: the preselected row (scripted messages only)
};

// CGuiWidget (base ctor 100c9a40).
struct Widget {
    virtual ~Widget() = default;
    float x = 0, y = 0, w = 0, h = 0;
    bool visible = true, enabled = true;
    int state = 1;        // +0x58: 0 selected (focused or hovered), 1 normal, 2 disabled
    bool tab_stop = true; // +0x5c
    std::string text;     // +0x20, cp1251
    std::string tooltip;  // +0x2c, cp1251
    int id = -1;          // child index in the parent form
    Form* parent = nullptr;
    Widget* container = nullptr; // list or bar holding it

    void set_rect(float x0, float y0, float x1, float y1) { x = x0, y = y0, w = x1 - x0, h = y1 - y0; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    void enable(bool on);
    // Sends `c` to the parent form's listener (when there is one).
    void notify(GuiEnv& env, Command c);

    virtual void draw(OverlayFrame&, GuiEnv&) {}
    virtual bool hit(float px, float py) const { return contains(px, py); }
    // Input: true = handled.
    virtual bool key_down(GuiEnv&, int) { return false; }
    virtual bool key_up(GuiEnv&, int) { return false; }
    virtual bool char_in(GuiEnv&, unsigned char) { return false; }
    virtual bool lbutton_down(GuiEnv&, float, float) { return false; }
    virtual bool lbutton_up(GuiEnv&, float, float) { return false; }
    virtual bool rbutton_down(GuiEnv&, float, float) { return false; }
    virtual bool mouse_move(GuiEnv&, float, float) { return false; }
    virtual void set_focus(GuiEnv&);  // slot 19: state 0 (or 2 when disabled)
    virtual void kill_focus(GuiEnv&); // slot 20: state 1 (or 2)
    virtual void draw_tooltip(OverlayFrame&, GuiEnv&) const;
};

// CGuiFlashWidget (10013720): the text_flash colours and the focus-flash timer.
struct FlashWidget : Widget {
    double flash_ms = 0; // +0x710
    void set_focus(GuiEnv& env) override; // CGuiFlashWidget_SetFocus 100146a0: also plays HILIGHT
    std::uint32_t text_colour(const GuiEnv& env) const; // ApplyTextColour 100149b0: the *bright* colour of the state
    double elapsed(const GuiEnv& env) const { return (env.now_ms - flash_ms) / 1000.0; }
    float interval(const GuiEnv& env) const { return env.ctrls ? env.ctrls->focus_flash_interval : 0.25f; }
};

// CGuiTextButton (10015b90): lamps at both ends, a stretched tile, centred MENU_FONT text.
struct TextButton : FlashWidget {
    bool flash = true; // +0x718: the selected art blinks
    void draw(OverlayFrame&, GuiEnv&) override;
    bool lbutton_down(GuiEnv&, float, float) override;
    bool lbutton_up(GuiEnv&, float, float) override;
    bool key_down(GuiEnv&, int) override;
    bool mouse_move(GuiEnv&, float px, float py) override { return hit(px, py); }
    // The activation (click / Enter / Space): BUTTON_CLICK, then a command to the form (or, for rows and bar
    // buttons, true for the container to notify).
    virtual bool activate(GuiEnv& env);
};

// CGuiImageButton (100194d0): `<prefix>_on_selected / _off_active / _disable_active` stretched over the rect.
struct ImageButton : TextButton {
    std::string prefix;
    void draw(OverlayFrame&, GuiEnv&) override;
};

// CGuiScrollArrow (1001ee20): an image button that repeats while held (scroll_interval).
struct ScrollArrow : ImageButton {
    bool pressed = false;
    double repeat_ms = 0;
    int bar_index = 0; // its value in the bar
    void draw(OverlayFrame&, GuiEnv&) override;
    bool lbutton_down(GuiEnv&, float, float) override;
    bool lbutton_up(GuiEnv&, float, float) override;
    bool key_down(GuiEnv&, int) override;
    bool key_up(GuiEnv&, int) override;
    void kill_focus(GuiEnv&) override;
};

// CGuiTitle (1001e560): the header strip with the text centred in white; optional header buttons.
struct Title : Widget {
    std::vector<std::unique_ptr<Widget>> buttons; // right to left (the redefine-controls tabs)
    Title() { tab_stop = false; }
    void draw(OverlayFrame&, GuiEnv&) override;
    bool hit(float, float) const override { return false; }
};

// CGuiCheckBox (100168e0): green / red light, CHECKBOX on toggle.
struct CheckBox : FlashWidget {
    bool checked = false;
    void draw(OverlayFrame&, GuiEnv&) override;
    bool lbutton_down(GuiEnv&, float, float) override;
    bool key_down(GuiEnv&, int) override;
    bool mouse_move(GuiEnv&, float px, float py) override { return hit(px, py); }
    void toggle(GuiEnv& env);
};

// CGuiSlider (10017b50) and its percent (100191c0) / float (10019320) subclasses; also CArrowScroller
// (10019ac0) when `items` is set (no bar; the value is the item index).
struct Slider : FlashWidget {
    float min = 0, max = 1, value = 0, step = 0.01f;
    int mode = 0; // +0xfa0: 0 idle, 1 left held, 2 right held, 3 left hovered, 4 right hovered
    double repeat_ms = 0;
    enum class Kind { Percent, Float, Items } kind = Kind::Percent;
    std::vector<std::string> items; // ArrowScroller entries (cp1251)
    std::string fallback;           // ArrowScroller text with no items (string 6100 "ПУСТО")
    std::function<void(GuiEnv&)> on_change; // the +0x718 hook (live volume)

    void draw(OverlayFrame&, GuiEnv&) override;
    bool key_down(GuiEnv&, int) override;
    bool key_up(GuiEnv&, int) override;
    bool lbutton_down(GuiEnv&, float, float) override;
    bool lbutton_up(GuiEnv&, float, float) override;
    bool mouse_move(GuiEnv&, float, float) override;
    void kill_focus(GuiEnv&) override;
    std::string display_text() const;
    int index() const { return int(value + 0.5f); }

private:
    void step_by(GuiEnv& env, int dir); // AutoStep / StepIndex
    float lw(const GuiEnv&) const;
    float aw(const GuiEnv&) const;
};

// CGuiItemList (100cace0) / CGuiButtonBar (1001b100): a container of buttons; hover selects; activation notifies
// the form with the selected index.
struct ItemList : Widget {
    std::vector<std::unique_ptr<TextButton>> items;
    int selected = -1, top = 0, visible_rows = 7;
    bool vertical = true;
    void add(std::unique_ptr<TextButton> b);
    void select(GuiEnv& env, int i);
    void clear_selection(GuiEnv& env);
    bool hit(float, float) const override;
    bool key_down(GuiEnv&, int) override;
    bool key_up(GuiEnv&, int) override;
    bool lbutton_down(GuiEnv&, float, float) override;
    bool lbutton_up(GuiEnv&, float, float) override;
    bool mouse_move(GuiEnv&, float, float) override;
    void kill_focus(GuiEnv&) override;
    void set_focus(GuiEnv&) override;
};

struct ButtonBar : ItemList {
    std::vector<int> align; // 0 left-packed, 1 right-packed, 2/3 spacers
    ButtonBar() { vertical = false; visible_rows = 1000; }
    void add_button(std::unique_ptr<TextButton> b, int a);
    void draw(OverlayFrame&, GuiEnv&) override;
    void set_focus(GuiEnv&) override; // 1001b640: the enabled button with the smallest x
};

// CGuiScrollList (1001c150) with its title and bar: 7 rows of text buttons, scroll arrows and a back button.
struct ScrollList : ItemList {
    Title* title = nullptr;  // form child (id + 2)
    ButtonBar* bar = nullptr; // form child (id + 1)
    ScrollArrow *up = nullptr, *down = nullptr;
    std::vector<std::string> paths; // the row's data path (GetItemPath 1001cb30)
    bool bar_below_all_rows = false; // CKeyBindList_LayoutBar 100208e0: the bar always below `visible_rows` rows
    void draw(OverlayFrame&, GuiEnv&) override;
    void set_focus(GuiEnv&) override; // 1001cb10: Select(top)
    void clear_rows();
    void add_row(GuiEnv* env, std::string text, std::string path, bool enabled);
    void scroll(int dir);
    void update_arrows();
};

} // namespace parkan::shell
