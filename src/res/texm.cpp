#include "res/texm.hpp"

#include "res/nres.hpp"

namespace parkan {

namespace {

std::size_t bytes_per_pixel(std::uint32_t format) {
    switch (format) {
    case 0: return 1;
    case 565: case 556: case 4444: case 88: return 2;
    case 888: case 8888: return 4;
    }
    throw FormatError("unknown Texm pixel format " + std::to_string(format));
}

// Widen an n-bit channel to 8 bits by repeating its significant bits.
std::uint8_t expand(std::uint32_t v, int bits) {
    v &= (1u << bits) - 1;
    std::uint32_t out = 0;
    int shift = 8 - bits;
    while (shift > -bits) {
        out |= shift >= 0 ? v << shift : v >> -shift;
        shift -= bits;
    }
    return std::uint8_t(out & 0xFF);
}

} // namespace

Texm decode_texm(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.u32() != fourcc("Texm")) throw FormatError("not a Texm image");
    Texm t;
    t.width = r.u32();
    t.height = r.u32();
    const std::uint32_t mips = r.u32();
    t.flags4 = r.u32();
    t.flags5 = r.u32();
    t.unknown6 = r.u32();
    t.format = r.u32();
    if (t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192 || mips == 0 || mips > 16)
        throw FormatError("implausible Texm header");

    const std::size_t bpp = bytes_per_pixel(t.format);
    std::span<const std::uint8_t> palette;
    if (t.format == 0) palette = r.bytes(1024); // 256 x [B,G,R,A]

    for (std::uint32_t level = 0; level < mips; ++level) {
        const std::size_t w = std::max<std::uint32_t>(1, t.width >> level);
        const std::size_t h = std::max<std::uint32_t>(1, t.height >> level);
        auto px = r.bytes(w * h * bpp);
        Bytes out(w * h * 4);
        for (std::size_t i = 0; i < w * h; ++i) {
            std::uint8_t* o = &out[i * 4];
            switch (t.format) {
            case 0: {
                const std::uint8_t* p = &palette[px[i] * 4];
                o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255; // palette alpha unused (assumed)
                break;
            }
            case 565: case 556: case 4444: case 88: {
                const std::uint32_t v = px[i * 2] | px[i * 2 + 1] << 8;
                if (t.format == 565) o[0] = expand(v >> 11, 5), o[1] = expand(v >> 5, 6), o[2] = expand(v, 5), o[3] = 255;
                else if (t.format == 556) o[0] = expand(v >> 11, 5), o[1] = expand(v >> 6, 5), o[2] = expand(v, 6), o[3] = 255;
                else if (t.format == 4444) o[0] = expand(v >> 8, 4), o[1] = expand(v >> 4, 4), o[2] = expand(v, 4), o[3] = expand(v >> 12, 4);
                else o[0] = o[1] = o[2] = std::uint8_t(v & 0xFF), o[3] = std::uint8_t(v >> 8);
                break;
            }
            case 888: case 8888: {
                const std::uint8_t* p = &px[i * 4];
                o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = t.format == 8888 ? p[3] : 255;
                break;
            }
            }
        }
        t.mips_rgba.push_back(std::move(out));
    }

    if (r.remaining() >= 8) {
        Reader page(data.subspan(r.pos()));
        if (page.u32() == fourcc("Page")) {
            const std::uint32_t n = page.u32();
            if (page.remaining() < std::size_t(n) * 8) throw FormatError("truncated Texm page chunk");
            for (std::uint32_t i = 0; i < n; ++i)
                t.pages.push_back({page.read<std::int16_t>(), page.read<std::int16_t>(), page.read<std::int16_t>(),
                                   page.read<std::int16_t>()});
            r.seek(r.pos() + 8 + std::size_t(n) * 8);
        }
    }
    if (!r.at_end()) throw FormatError("trailing bytes after Texm data");
    return t;
}

} // namespace parkan
