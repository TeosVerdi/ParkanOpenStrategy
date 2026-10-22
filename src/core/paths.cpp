#include "core/paths.hpp"

#include <algorithm>
#include <cctype>

namespace parkan {

namespace {
bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
} // namespace

std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& base, std::string_view rel) {
    namespace fs = std::filesystem;
    fs::path cur = base;
    std::size_t start = 0;
    while (start <= rel.size()) {
        std::size_t end = rel.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = rel.size();
        std::string_view part = rel.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        fs::path exact = cur / std::string(part);
        std::error_code ec;
        if (fs::exists(exact, ec)) {
            cur = exact;
            continue;
        }
        bool found = false;
        for (const auto& e : fs::directory_iterator(cur, ec)) {
            if (iequals(e.path().filename().string(), part)) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return cur;
}

} // namespace parkan
