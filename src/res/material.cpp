#include "res/material.hpp"

#include <cctype>
#include <cstdlib>

namespace parkan {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        auto nl = text.find('\n');
        out.push_back(trim(text.substr(0, nl)));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

// "<n>" then n rows of "<legacy id> <name>"; returns the names.
std::vector<std::string> table(const std::vector<std::string_view>& ls, std::size_t& i) {
    while (i < ls.size() && ls[i].empty()) ++i;
    if (i >= ls.size()) throw FormatError("WEAR: missing row count");
    const long n = std::strtol(std::string(ls[i++]).c_str(), nullptr, 10);
    if (n < 0 || n > 4096) throw FormatError("WEAR: bad row count");
    std::vector<std::string> out;
    for (long k = 0; k < n; ++k, ++i) {
        if (i >= ls.size()) throw FormatError("WEAR: truncated table");
        auto sp = ls[i].find_first_of(" \t");
        if (sp == std::string_view::npos) throw FormatError("WEAR: row without a name");
        out.emplace_back(trim(ls[i].substr(sp)));
    }
    return out;
}

} // namespace

Wear parse_wear(std::string_view text) {
    auto ls = lines(text);
    std::size_t i = 0;
    Wear w;
    w.materials = table(ls, i);
    while (i < ls.size() && ls[i].empty()) ++i;
    if (i < ls.size()) {
        if (ls[i] != "LIGHTMAPS") throw FormatError("WEAR: unexpected text after the material table");
        ++i;
        w.lightmaps = table(ls, i);
    }
    return w;
}

Material decode_mat0(std::span<const std::uint8_t> data, std::uint32_t attr1, std::uint32_t attr2) {
    Reader r(data);
    Material m;
    m.flags = attr1;
    m.version = attr2;
    const std::uint16_t phases = r.read<std::uint16_t>();
    const std::uint16_t blocks = r.read<std::uint16_t>();
    // Version 6 (every shipped material) carries 10 more header bytes, e.g. 05 ff | 1.0f | 0.
    if (attr2 >= 6) {
        auto pre = r.bytes(10);
        std::copy(pre.begin(), pre.end(), m.prefix.begin());
    }
    m.phases.resize(phases);
    for (auto& p : m.phases) {
        auto b = r.bytes(18);
        std::copy(b.begin(), b.end(), p.params.begin());
        auto name = r.bytes(16);
        p.texture.assign(reinterpret_cast<const char*>(name.data()),
                         ::strnlen(reinterpret_cast<const char*>(name.data()), 16));
    }
    m.anims.resize(blocks);
    for (auto& a : m.anims) {
        a.header = r.u32();
        a.keys.resize(r.read<std::uint16_t>());
        for (auto& k : a.keys) k = {r.read<std::uint16_t>(), r.read<std::uint16_t>(), r.read<std::uint16_t>()};
    }
    if (!r.at_end()) throw FormatError("trailing bytes in MAT0 (version " + std::to_string(attr2) + ")");
    return m;
}

} // namespace parkan
