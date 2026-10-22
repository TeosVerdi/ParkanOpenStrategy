#include "res/library.hpp"

#include "core/paths.hpp"

#include <cctype>

namespace parkan {

const NResArchive& AssetLibrary::archive(std::string_view name) {
    std::string key(name);
    for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c == '\\' ? '/' : c)));
    if (auto it = open_.find(key); it != open_.end()) return *it->second;
    auto path = resolve_path(game_, name);
    if (!path) throw std::runtime_error("archive not found: " + std::string(name));
    auto arc = std::make_unique<NResArchive>(NResArchive::open(*path));
    return *open_.emplace(key, std::move(arc)).first->second;
}

const NResEntry* AssetLibrary::find(std::string_view archive_name, std::string_view entry_name) {
    return archive(archive_name).find_name(entry_name);
}

} // namespace parkan
