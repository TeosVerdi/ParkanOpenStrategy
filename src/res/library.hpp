// Opens archives from the game directory on demand and keeps them for lookups, the way the
// original keeps INgiResFile handles (ngi32-platform.md).
#pragma once

#include "res/nres.hpp"

#include <map>
#include <memory>

namespace parkan {

class AssetLibrary {
public:
    explicit AssetLibrary(std::filesystem::path game_dir) : game_(std::move(game_dir)) {}

    const std::filesystem::path& game_dir() const { return game_; }
    // `name` is a game-relative path such as "Textures.lib" or "DATA\\MAPS\\Tut_2\\Land.msh".
    const NResArchive& archive(std::string_view name);
    const NResEntry* find(std::string_view archive_name, std::string_view entry_name);

private:
    std::filesystem::path game_;
    std::map<std::string, std::unique_ptr<NResArchive>> open_;
};

} // namespace parkan
