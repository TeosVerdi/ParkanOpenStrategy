// TFT bitmap fonts (ui/font.lib GAME/TOOL/MENU_FONT sets, sys.lib ARIALTEX). Spec: notes/rsli-font.md §2;
// loader Ngi32 LoadTextureSurface 1000fb30, layout CNgiFont::DrawText 10010e40.
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

struct TftFont {
    struct Glyph {
        float u0 = 0, u1 = 0, v0 = 0; // atlas UVs; the quad is w+1 by height+1 pixels
        std::int32_t w = 0;           // advance before spacing
    };
    std::int32_t max_width = 0, height = 0, spacing = 0;
    float v_height = 0; // (height + 1) / atlas height
    std::array<Glyph, 256> glyphs{};
    std::uint32_t atlas_width = 0, atlas_height = 0;
    Bytes rgba; // palette colours; index 0 transparent, every other index opaque (SelectPixelConverter)

    // GetTextWidth 10010de0: Σ w + spacing per character, in the font's pixels. `text` is single-byte
    // (cp1251): the game fonts use the identity character map (SetCharMap(0)).
    std::int32_t text_width(std::string_view text) const;
};

TftFont decode_tft(std::span<const std::uint8_t> data);

} // namespace parkan
