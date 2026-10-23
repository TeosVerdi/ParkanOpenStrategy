#include "shell/shell_data.hpp"

#include "game/mission_runtime.hpp"

#include <cctype>
#include <cstdlib>

namespace parkan::shell {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

bool is_resource_header(const std::string& key) {
    return key == "desc" || key == "library" || key == "libtype" || key == "type";
}

} // namespace

ShellControls parse_shell_ctrls(std::string_view text) {
    ShellControls c;
    const auto objects = game::parse_cfg(text);
    for (const auto& o : objects) {
        const std::string n = lower(o.name);
        auto num = [&](const char* k, float& out) {
            if (const std::string* v = o.get(k)) out = float(std::atof(v->c_str()));
        };
        if (n == "common") {
            num("caret_flash_interval", c.caret_flash_interval);
            num("focus_flash_interval", c.focus_flash_interval);
            num("scroll_interval", c.scroll_interval);
            num("volume_scroll_interval", c.volume_scroll_interval);
            num("credits_scroll_interval", c.credits_scroll_interval);
            num("demo_run_interval", c.demo_run_interval);
            num("refresh_session_interval", c.refresh_session_interval);
            if (const std::string* v = o.get("main_menu_track")) c.main_menu_track = std::atoi(v->c_str());
            if (const std::string* v = o.get("credits_track")) c.credits_track = std::atoi(v->c_str());
            if (const std::string* v = o.get("logo1_param")) c.logo1_param = *v;
            if (const std::string* v = o.get("logo2_param")) c.logo2_param = *v;
            if (const std::string* v = o.get("splash_param")) c.splash_param = *v;
        } else if (n.size() > 6 && n.ends_with("_fader")) {
            Fader f;
            num("in_interval", f.in);
            num("out_interval", f.out);
            num("show_interval", f.show);
            if (const std::string* v = o.get("sound_resource")) f.sound = *v;
            c.faders[n.substr(0, n.size() - 6)] = f;
        } else if (n == "backgrounds") {
            for (const auto& [k, v] : o.entries) c.backgrounds[lower(k)] = v;
        } else if (n == "text_flash") {
            auto colour = [&](const char* base, std::uint32_t& out) {
                const std::string b(base);
                std::uint32_t argb = out;
                const char* ch[4] = {"_a", "_r", "_g", "_b"};
                for (int i = 0; i < 4; ++i)
                    if (const std::string* v = o.get(b + ch[i])) {
                        const int shift = 24 - 8 * i;
                        argb = (argb & ~(0xffu << shift)) | (std::uint32_t(std::atoi(v->c_str()) & 0xff) << shift);
                    }
                out = argb;
            };
            colour("rgb_selected_bright", c.text_flash.selected_bright);
            colour("rgb_selected_dark", c.text_flash.selected_dark);
            colour("rgb_on_bright", c.text_flash.on_bright);
            colour("rgb_on_dark", c.text_flash.on_dark);
            colour("rgb_off_bright", c.text_flash.off_bright);
            colour("rgb_off_dark", c.text_flash.off_dark);
            colour("rgb_pressed_bright", c.text_flash.pressed_bright);
            colour("rgb_pressed_dark", c.text_flash.pressed_dark);
        }
    }
    c.sprites = ui::parse_sprite_cfg(text);
    return c;
}

MenuResources parse_menu_resources(std::string_view text) {
    MenuResources r;
    for (const auto& o : game::parse_cfg(text)) {
        const std::string n = lower(o.name);
        const std::string* lib = o.get("library");
        if (n == "textures" || n == "backgrounds" || n == "fonts") {
            auto& map = n == "textures" ? r.textures : n == "backgrounds" ? r.backgrounds : r.fonts;
            if (lib && n == "textures") r.textures_library = *lib;
            if (lib && n == "backgrounds") r.backgrounds_library = *lib;
            for (const auto& [k, v] : o.entries)
                if (!is_resource_header(lower(k))) map[lower(k)] = std::uint32_t(std::atoi(v.c_str()));
        } else if (n == "sounds") {
            if (lib) r.sounds_library = *lib;
            for (const auto& [k, v] : o.entries)
                if (!is_resource_header(lower(k))) r.sounds[k] = v;
        }
    }
    return r;
}

} // namespace parkan::shell
