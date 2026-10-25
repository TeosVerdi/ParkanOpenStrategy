// The HUD's textures and named sprites: ui/game_resources.cfg `object textures` (names → ui\ui.lib
// entries), the sprite objects of ui/hq.cfg and ui/compaund.cfg, and the mission's satellite map
// (mission.cfg `object minimap` → ui\minimap.lib). Spec: notes/hud.md §1.
#pragma once

#include "res/library.hpp"
#include "res/texm.hpp"

#include <map>
#include <optional>

namespace parkan::ui {

// The texture pages the cockpit draws from (game_resources.cfg names).
inline constexpr const char* kHudPages[] = {"page6", "page7", "page9", "ui_menu", "ui_menu3",
                                            "new_ui1", "icons", "page1", "page3", "page4", "page5"};

// A named sprite (CHudSprite_LoadFromCfg 1008f6a0): `texture` is a page name; rot = rotate / 90.
struct HudSprite {
    std::string texture;
    std::int32_t x = 0, y = 0, w = 0, h = 0, rot = 0;
};
// The `object NAME … end` blocks of hq.cfg / compaund.cfg, by lower-case name.
std::map<std::string, HudSprite> parse_sprite_cfg(std::string_view text);

// game_resources.cfg `object textures`: page name (lower case) → index into its library.
struct TexturePageTable {
    std::string library; // e.g. "ui\\ui.lib"
    std::map<std::string, std::uint32_t> index;
};
TexturePageTable parse_texture_pages(std::string_view game_resources_cfg);

struct HudResources {
    std::map<std::string, Texm> pages;       // kHudPages that loaded, by name
    std::map<std::string, HudSprite> sprites; // hq.cfg then compaund.cfg objects
    std::optional<Texm> minimap;              // the mission's satellite map
};

// Loads the cockpit pages and sprite tables from the game folder; missing pieces are left out (logged
// by the caller). `mission_dir` (optional) adds the satellite map.
HudResources load_hud_resources(AssetLibrary& lib, const std::filesystem::path* mission_dir = nullptr);
// mission.cfg `object minimap` → its ui\minimap.lib texture.
std::optional<Texm> load_minimap(AssetLibrary& lib, const std::filesystem::path& mission_dir);

} // namespace parkan::ui
