// Game paths are written with Windows separators and arbitrary case ("MISSIONS\SCRIPTS\tut2_pl2");
// resolve them against the real tree case-insensitively.
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace parkan {

// Joins `rel` (either separator, any case) onto `base` and finds the existing file or directory.
std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& base, std::string_view rel);

} // namespace parkan
