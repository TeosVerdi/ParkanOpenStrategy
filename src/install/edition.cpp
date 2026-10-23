#include "install/edition.hpp"

#include "install/disc.hpp"

namespace parkan::install {

namespace {

constexpr EditionInfo kEditions[] = {
    {Edition::Unknown, "unknown", "unknown edition", ""},
    {Edition::GogRu, "gog-ru", "GOG 2.0.0.4 (Russian)", "ru"},
    {Edition::GogEng, "gog-en", "GOG 2.0.0.3 (English)", "en"},
    {Edition::Disc2002, "disc-2002", "CD «Часть 1» (2002 re-edition)", "ru"},
    {Edition::Disc2001, "disc-2001", "CD, the 2001 original", "ru"},
};

constexpr ManifestEntry kManifest[] = {
#include "install/manifests/manifest.inc"
    {nullptr, 0, 0, 0}, // keeps the array non-empty
};

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

} // namespace

const EditionInfo& edition_info(Edition e) {
    const auto i = std::size_t(e);
    return i < std::size(kEditions) ? kEditions[i] : kEditions[0];
}

Edition edition_from_key(std::string_view key) {
    for (const auto& e : kEditions)
        if (key == e.key) return e.id;
    return Edition::Unknown;
}

std::span<const ManifestEntry> manifest() { return {kManifest, std::size(kManifest) - 1}; }

bool is_user_file(std::string_view path) {
    const std::string p = ascii_lower(path);
    return ends_with(p, ".ini") || ends_with(p, ".man") || ends_with(p, ".tbl") || p.starts_with("save/");
}

bool is_ignored_file(std::string_view path) {
    const std::string p = ascii_lower(path);
    const std::string_view name = std::string_view(p).substr(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1);
    return name.starts_with("unins") || name == "goglog.ini" || ends_with(name, ".lnk") || name == ".ds_store" ||
           name == "thumbs.db" || p == "import.json" || ends_with(p, ".part") ||
           (p.starts_with("music/") && ends_with(p, ".wav"));
}

} // namespace parkan::install
