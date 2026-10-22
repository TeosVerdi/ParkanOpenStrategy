// Texm images (Textures.lib, ui/*.lib, lightmaps). Spec: FParkan reference/texm; native decoders in
// Ngi32 (VA 0x10005930, 0x1000F620 / 0x1000F70C).
#pragma once

#include "core/bytes.hpp"

namespace parkan {

struct TexmPageRect {
    std::int16_t x, width, y, height;
};

struct Texm {
    std::uint32_t width = 0, height = 0;
    std::uint32_t format = 0; // 0 indexed, 565, 556, 4444, 88, 888, 8888
    std::uint32_t flags4 = 0, flags5 = 0, unknown6 = 0;
    std::vector<Bytes> mips_rgba; // RGBA8, level 0 first
    std::vector<TexmPageRect> pages;
};

Texm decode_texm(std::span<const std::uint8_t> data);

} // namespace parkan
