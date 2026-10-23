// The shell's data files: ui/shell_ctrls.cfg (GUI timings, logo/splash parameters, faders, form
// backgrounds, text colours, the control sprites) and ui/menu_resources.cfg (texture pages, background
// pieces, fonts, sounds), read by CShell::Init 100078c0 through IResManager. Spec: notes/shell.md.
#pragma once

#include "ui/hud_resources.hpp"

#include <map>
#include <string>
#include <string_view>

namespace parkan::shell {

// object text_flash: the text colours of the controls, ARGB.
struct TextFlash {
    std::uint32_t selected_bright = 0xffffffff, selected_dark = 0xff96ffff; // focused (flashing)
    std::uint32_t on_bright = 0xff64ff64, on_dark = 0xff408040;             // enabled
    std::uint32_t off_bright = 0xff646464, off_dark = 0xff282828;           // disabled
    std::uint32_t pressed_bright = 0xffff0000, pressed_dark = 0xffff0000;
};

// object <name>_fader: a logo shown with a fade in, a hold and a fade out, and a sound.
struct Fader {
    float in = 0, out = 0, show = 0; // seconds
    std::string sound;               // menu_resources.cfg `sounds` key
};

struct ShellControls {
    // object common
    float caret_flash_interval = 0.5f, focus_flash_interval = 0.25f, scroll_interval = 0.1f;
    float volume_scroll_interval = 0.1f, credits_scroll_interval = 0.02f, demo_run_interval = 60.f;
    float refresh_session_interval = 10.f;
    int main_menu_track = -1;             // −1 = a random track
    std::optional<int> credits_track;     // documented, absent in the shipped files
    std::string logo1_param, logo2_param, splash_param; // "" skip; a movie file; or a fader name
    std::map<std::string, Fader> faders;  // by object name without "_fader" (e.g. "logo2")
    std::map<std::string, std::string> backgrounds; // form name → background prefix ("Main", "Opts", …)
    TextFlash text_flash;
    std::map<std::string, ui::HudSprite> sprites; // lower-case object name → sprite (TEXTURE …)
};
ShellControls parse_shell_ctrls(std::string_view text);

struct MenuResources {
    std::string textures_library = "ui\\ui.lib";
    std::map<std::string, std::uint32_t> textures; // lower-case page name → entry index
    std::string backgrounds_library = "ui\\ui_back.lib";
    std::map<std::string, std::uint32_t> backgrounds; // lower-case piece name ("main_lu") → entry index
    std::map<std::string, std::string> sounds;        // key (as written) → sounds.lib entry
    std::string sounds_library = "sounds.lib";
    std::map<std::string, std::uint32_t> fonts;       // lower-case font name ("menu_font_640x480") → font.lib index
};
MenuResources parse_menu_resources(std::string_view text);

} // namespace parkan::shell
