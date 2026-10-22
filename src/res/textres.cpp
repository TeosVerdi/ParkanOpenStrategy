#include "res/textres.hpp"

#include <cstdlib>
#include <cstring>

namespace parkan {

namespace {

template <class T>
T rd(std::span<const std::uint8_t> d, std::size_t off) {
    if (off + sizeof(T) > d.size()) throw FormatError("PE image truncated");
    T v;
    std::memcpy(&v, d.data() + off, sizeof v);
    return v;
}

struct Section {
    std::uint32_t va, vsize, raw, rawsize;
};

} // namespace

std::string utf16le_to_utf8(std::span<const std::uint8_t> b) {
    std::string out;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        std::uint32_t c = std::uint32_t(b[i]) | std::uint32_t(b[i + 1]) << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) { // surrogate pair
            const std::uint32_t lo = std::uint32_t(b[i + 2]) | std::uint32_t(b[i + 3]) << 8;
            if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 2;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
                out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

TextRes TextRes::from_pe(std::span<const std::uint8_t> d, std::uint16_t language) {
    if (rd<std::uint16_t>(d, 0) != 0x5A4D) throw FormatError("not an MZ image");
    const std::uint32_t pe = rd<std::uint32_t>(d, 0x3c);
    if (rd<std::uint32_t>(d, pe) != 0x4550) throw FormatError("not a PE image");
    const std::uint16_t nsec = rd<std::uint16_t>(d, pe + 6), opt_size = rd<std::uint16_t>(d, pe + 20);
    const std::size_t opt = pe + 24;
    if (rd<std::uint16_t>(d, opt) != 0x10b) throw FormatError("not a PE32 image");
    const std::uint32_t res_rva = rd<std::uint32_t>(d, opt + 96 + 2 * 8); // data directory 2: resources
    std::vector<Section> secs;
    for (std::size_t i = 0, s = opt + opt_size; i < nsec; ++i, s += 40)
        secs.push_back({rd<std::uint32_t>(d, s + 12), rd<std::uint32_t>(d, s + 8), rd<std::uint32_t>(d, s + 20),
                        rd<std::uint32_t>(d, s + 16)});
    auto file_off = [&](std::uint32_t rva) -> std::size_t {
        for (const auto& s : secs)
            if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawsize)) return s.raw + (rva - s.va);
        throw FormatError("RVA outside the sections");
    };
    const std::size_t root = file_off(res_rva);
    // A resource directory: 16-byte header, then named and id entries of 8 bytes each.
    auto entries = [&](std::size_t dir) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out; // (name/id, offset with the dir bit)
        const std::size_t n = std::size_t(rd<std::uint16_t>(d, dir + 12)) + rd<std::uint16_t>(d, dir + 14);
        for (std::size_t i = 0; i < n; ++i) out.push_back({rd<std::uint32_t>(d, dir + 16 + i * 8), rd<std::uint32_t>(d, dir + 20 + i * 8)});
        return out;
    };
    TextRes t;
    std::map<std::uint32_t, std::uint16_t> lang_of;
    for (auto [type, type_off] : entries(root)) {
        if (type != 6 || !(type_off & 0x80000000u)) continue; // RT_STRING
        for (auto [block, block_off] : entries(root + (type_off & 0x7fffffffu))) {
            if (!(block_off & 0x80000000u) || (block & 0x80000000u)) continue;
            for (auto [lang, data_off] : entries(root + (block_off & 0x7fffffffu))) {
                if (data_off & 0x80000000u) continue;
                const std::size_t de = root + data_off;
                const std::size_t at = file_off(rd<std::uint32_t>(d, de)), size = rd<std::uint32_t>(d, de + 4);
                if (at + size > d.size()) throw FormatError("string block outside the image");
                // Block n holds strings (n − 1)·16 … (n − 1)·16 + 15: u16 length + UTF-16 text each.
                std::size_t p = at;
                for (std::uint32_t k = 0; k < 16 && p + 2 <= at + size; ++k) {
                    const std::size_t len = rd<std::uint16_t>(d, p);
                    p += 2;
                    if (p + len * 2 > at + size) throw FormatError("string runs past its block");
                    const std::uint32_t id = (block - 1) * 16 + k;
                    if (len) {
                        auto it = lang_of.find(id);
                        if (it == lang_of.end() || (it->second != language && lang == language)) {
                            t.strings_[id] = utf16le_to_utf8(d.subspan(p, len * 2));
                            lang_of[id] = std::uint16_t(lang);
                        }
                    }
                    p += len * 2;
                }
            }
        }
    }
    return t;
}

void TextRes::load_names(std::string_view cfg) {
    std::size_t pos = 0;
    while (pos < cfg.size()) {
        std::size_t end = cfg.find('\n', pos);
        if (end == std::string_view::npos) end = cfg.size();
        std::string_view line = cfg.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        auto trim = [](std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
            return v;
        };
        const std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key.empty() || value.empty() || value.front() < '0' || value.front() > '9') continue;
        names_[std::string(key)] = std::uint32_t(std::strtoul(std::string(value).c_str(), nullptr, 10));
    }
}

const std::string& TextRes::get(std::string_view name) const {
    static const std::string empty;
    auto it = names_.find(name);
    return it == names_.end() ? empty : get(it->second);
}

const std::string& TextRes::get(std::uint32_t id) const {
    static const std::string empty;
    auto it = strings_.find(id);
    return it == strings_.end() ? empty : it->second;
}

} // namespace parkan
