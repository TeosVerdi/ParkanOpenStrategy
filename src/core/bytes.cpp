#include "core/bytes.hpp"

#include <fstream>

namespace parkan {

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

std::string_view cstr_at(std::span<const std::uint8_t> pool, std::size_t off) {
    if (off >= pool.size()) throw FormatError("string offset out of range");
    auto begin = reinterpret_cast<const char*>(pool.data()) + off;
    auto len = ::strnlen(begin, pool.size() - off);
    if (off + len == pool.size()) throw FormatError("unterminated string");
    return {begin, len};
}

std::string cp1251_to_utf8(std::string_view s) {
    // 0x80..0xBF need a table; 0xC0..0xFF map to U+0410..U+044F.
    static constexpr char16_t hi[64] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409,
        0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0,
        0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB,
        0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6,
        0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        char32_t cp = c < 0x80 ? c : c < 0xC0 ? hi[c - 0x80] : 0x0410 + (c - 0xC0);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace parkan
