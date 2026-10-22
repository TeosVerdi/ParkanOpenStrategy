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

} // namespace

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
