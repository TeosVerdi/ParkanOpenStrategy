#include "shell/ini.hpp"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace parkan::shell {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

struct Line {
    std::size_t begin, end; // [begin, end) without the line break
};

std::vector<Line> split(std::string_view text) {
    std::vector<Line> out;
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t nl = text.find('\n', p);
        if (nl == std::string_view::npos) nl = text.size();
        std::size_t e = nl;
        if (e > p && text[e - 1] == '\r') --e;
        out.push_back({p, e});
        p = nl + 1;
    }
    return out;
}

// The section name of a "[name]" line, or nullopt.
std::optional<std::string_view> section_of(std::string_view line) {
    line = trim(line);
    if (line.size() < 2 || line.front() != '[') return std::nullopt;
    const auto close = line.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    return trim(line.substr(1, close - 1));
}

} // namespace

std::optional<std::string> ini_get(std::string_view text, std::string_view section, std::string_view key) {
    bool in = false;
    for (const Line& l : split(text)) {
        const std::string_view line = text.substr(l.begin, l.end - l.begin);
        if (auto s = section_of(line)) {
            in = iequals(*s, section);
            continue;
        }
        if (!in) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        if (iequals(trim(line.substr(0, eq)), key)) return std::string(trim(line.substr(eq + 1)));
    }
    return std::nullopt;
}

int ini_get_int(std::string_view text, std::string_view section, std::string_view key, int fallback) {
    const auto v = ini_get(text, section, key);
    return v && !v->empty() ? std::atoi(v->c_str()) : fallback;
}

std::string ini_set(std::string text, std::string_view section, std::string_view key, std::string_view value) {
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string entry = std::string(key) + "=" + std::string(value);
    bool in = false, found_section = false;
    std::size_t insert_at = std::string::npos; // after the section's last non-blank line
    for (const Line& l : split(text)) {
        const std::string_view line = std::string_view(text).substr(l.begin, l.end - l.begin);
        if (auto s = section_of(line)) {
            if (in) break;
            in = iequals(*s, section);
            if (in) found_section = true, insert_at = l.end;
            continue;
        }
        if (!in) continue;
        const auto eq = line.find('=');
        if (eq != std::string_view::npos && iequals(trim(line.substr(0, eq)), key)) {
            text.replace(l.begin, l.end - l.begin, entry);
            return text;
        }
        if (!trim(line).empty()) insert_at = l.end;
    }
    if (found_section) {
        text.insert(insert_at, eol + entry);
        return text;
    }
    if (!text.empty() && text.back() != '\n') text += eol;
    text += "[" + std::string(section) + "]" + eol + entry + eol;
    return text;
}

} // namespace parkan::shell
