#include "res/tft.hpp"

namespace parkan {

std::int32_t TftFont::text_width(std::string_view text) const {
    std::int32_t w = 0;
    for (unsigned char c : text) w += glyphs[c].w + spacing;
    return w;
}

TftFont decode_tft(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.str(4) != "Tfnt") throw FormatError("not a Tfnt font");
    TftFont f;
    f.max_width = r.i32();
    f.height = r.i32();
    f.v_height = r.f32();
    f.spacing = r.i32();
    for (auto& g : f.glyphs) g.u0 = r.f32(), g.u1 = r.f32(), g.v0 = r.f32(), g.w = r.i32();
    // The embedded Texm (0x1014): 'Texm', W, H, mips, flags4, flags5, 0, format; format 0 carries a
    // BGRX palette. Format 2 occurs only in the unused gamefont.rlb copy of ARIALTEX.
    if (r.str(4) != "Texm") throw FormatError("Tfnt: no embedded Texm");
    f.atlas_width = r.u32(), f.atlas_height = r.u32();
    r.u32(), r.u32(), r.u32(), r.u32();
    if (const std::uint32_t format = r.u32(); format != 0) throw FormatError("Tfnt: atlas format " + std::to_string(format));
    if (!f.atlas_width || !f.atlas_height || f.atlas_width > 4096 || f.atlas_height > 4096) throw FormatError("Tfnt: atlas size");
    const auto pal = r.bytes(1024);
    const auto px = r.bytes(std::size_t(f.atlas_width) * f.atlas_height);
    if (!r.at_end()) throw FormatError("Tfnt: trailing bytes");
    f.rgba.resize(px.size() * 4);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const std::uint8_t k = px[i];
        f.rgba[i * 4 + 0] = pal[k * 4 + 2], f.rgba[i * 4 + 1] = pal[k * 4 + 1], f.rgba[i * 4 + 2] = pal[k * 4 + 0];
        f.rgba[i * 4 + 3] = k ? 255 : 0;
    }
    return f;
}

} // namespace parkan
