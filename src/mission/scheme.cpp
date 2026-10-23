#include "mission/scheme.hpp"

namespace parkan::mission {

namespace {

std::string fixed(Reader& r, std::size_t n) {
    std::string s = r.str(n);
    if (auto z = s.find('\0'); z != std::string::npos) s.resize(z);
    return s;
}

void read_node(Reader& r, SchemeNode& n, int depth) {
    if (depth > 16) throw FormatError("scheme nesting too deep");
    n.lib = fixed(r, 32);
    n.entry = fixed(r, 32);
    n.one = r.u32();
    n.mount = r.i32();
    n.name = cp1251_to_utf8(fixed(r, 32));
    n.category = r.u32();
    const std::uint32_t count = r.u32();
    if (std::size_t(count) * 0x70 > r.remaining()) throw FormatError("scheme child count exceeds file size");
    n.children.resize(count);
    for (auto& c : n.children) read_node(r, c, depth + 1);
}

void put_u32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

void put_fixed(Bytes& out, std::string_view v) {
    if (v.size() > 31) throw FormatError("scheme string longer than 31 bytes");
    out.insert(out.end(), v.begin(), v.end());
    out.insert(out.end(), 32 - v.size(), 0);
}

void write_node(Bytes& out, const SchemeNode& n) {
    put_fixed(out, n.lib);
    put_fixed(out, n.entry);
    put_u32(out, n.one);
    put_u32(out, std::uint32_t(n.mount));
    put_fixed(out, utf8_to_cp1251(n.name));
    put_u32(out, n.category);
    put_u32(out, std::uint32_t(n.children.size()));
    for (const auto& c : n.children) write_node(out, c);
}

} // namespace

Bytes encode_scheme(const Scheme& s) {
    Bytes out;
    put_u32(out, 0xF0F1);
    put_u32(out, s.flags);
    write_node(out, s.root);
    return out;
}

Scheme decode_scheme(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.u32() != 0xF0F1) throw FormatError("not a scheme (.dat) file");
    Scheme s;
    s.flags = r.u32();
    read_node(r, s.root, 0);
    if (!r.at_end()) throw FormatError("trailing bytes after the scheme");
    return s;
}

} // namespace parkan::mission
