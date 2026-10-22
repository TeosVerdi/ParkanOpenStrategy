#include "ui/hud_resources.hpp"

#include "core/paths.hpp"
#include "game/mission_runtime.hpp"

#include <cctype>
#include <cstdlib>

namespace parkan::ui {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

std::string text_of(const Bytes& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

std::optional<std::string> read_text(const std::filesystem::path& base, std::string_view name) {
    auto p = resolve_path(base, name);
    if (!p) return std::nullopt;
    return text_of(read_file(*p));
}

} // namespace

std::map<std::string, HudSprite> parse_sprite_cfg(std::string_view text) {
    std::map<std::string, HudSprite> out;
    for (const auto& o : game::parse_cfg(text)) {
        const std::string *tex = o.get("texture"), *x = o.get("offset_x"), *y = o.get("offset_y"), *w = o.get("width"),
                          *h = o.get("height"), *rot = o.get("rotate");
        if (!tex || !x || !y || !w || !h) continue;
        HudSprite s{lower(*tex), std::atoi(x->c_str()), std::atoi(y->c_str()), std::atoi(w->c_str()), std::atoi(h->c_str()), 0};
        // CHudSprite_LoadFromCfg: rotate 90 / 180 / 270 → 1 / 2 / 3, anything else 0.
        if (rot) s.rot = *rot == "90" ? 1 : *rot == "180" ? 2 : *rot == "270" ? 3 : 0;
        out[lower(o.name)] = s;
    }
    return out;
}

TexturePageTable parse_texture_pages(std::string_view game_resources_cfg) {
    TexturePageTable t;
    for (const auto& o : game::parse_cfg(game_resources_cfg)) {
        if (lower(o.name) != "textures") continue;
        for (const auto& [k, v] : o.entries) {
            const std::string key = lower(k);
            if (key == "library") t.library = v;
            else if (key != "desc" && key != "libtype" && key != "type") t.index[key] = std::uint32_t(std::atoi(v.c_str()));
        }
    }
    return t;
}

std::optional<Texm> load_minimap(AssetLibrary& lib, const std::filesystem::path& mission_dir) {
    const auto cfg = read_text(mission_dir, "mission.cfg");
    if (!cfg) return std::nullopt;
    for (const auto& o : game::parse_cfg(*cfg)) {
        if (lower(o.name) != "minimap") continue;
        const std::string *library = o.get("library"), *entry = o.get("minimap");
        if (!library || !entry) return std::nullopt;
        if (const NResEntry* e = lib.find(*library, *entry)) return decode_texm(e->data);
        return std::nullopt;
    }
    return std::nullopt;
}

HudResources load_hud_resources(AssetLibrary& lib, const std::filesystem::path* mission_dir) {
    HudResources r;
    if (const auto cfg = read_text(lib.game_dir(), "UI/game_resources.cfg")) {
        const TexturePageTable t = parse_texture_pages(*cfg);
        if (!t.library.empty()) {
            const NResArchive& a = lib.archive(t.library);
            for (const char* name : kHudPages)
                if (auto it = t.index.find(name); it != t.index.end() && it->second < a.entries().size())
                    r.pages[name] = decode_texm(a.entries()[it->second].data);
        }
    }
    for (const char* file : {"UI/hq.cfg", "UI/compaund.cfg"})
        if (const auto cfg = read_text(lib.game_dir(), file))
            for (auto& [k, v] : parse_sprite_cfg(*cfg)) r.sprites[k] = v;
    if (mission_dir) r.minimap = load_minimap(lib, *mission_dir);
    return r;
}

} // namespace parkan::ui
