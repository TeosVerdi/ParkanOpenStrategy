// Microsoft Video 1 (fourcc CRAM / MSVC, Video for Windows msvidc32): the codec of the intro videos.
// Spec: notes/shell.md "Intro videos".
//
// The image is cut into 4×4 blocks, coded in raster order from the bottom block row up (DIB order).
// Each block starts with a 16-bit little-endian code word (byte A, byte B):
// - A = B = 0 once every block is coded: end of frame;
// - (B & 0xFC) == 0x84: skip ((B − 0x84) << 8) + A blocks (kept from the previous frame; the count runs on
//   across block rows);
// - B < 0x80: the word is a 16-bit mask, bit i for pixel i (row by row from the block's bottom row, left to
//   right), set = colour 0, clear = colour 1. 16-bit streams: two RGB555 colours follow; when bit 15 of the
//   first one is set, eight colours follow instead and each 2×2 quadrant (bottom-left, bottom-right,
//   top-left, top-right) has its own pair. 8-bit streams: two palette indices follow (2-colour);
// - 8-bit, B ≥ 0x90: eight indices follow, one pair per quadrant (as above);
// - otherwise one colour fills the block: the word itself (16 bit, RGB555) or byte A (8 bit).
// Port: the 8-bit path is implemented from the format description; no shipped file uses it.
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

class MsVideo1Decoder {
public:
    // `bit_count` 8 or 16; `width`, `height` multiples of 4 (other sizes: the partial blocks are skipped).
    MsVideo1Decoder(int width, int height, int bit_count);

    int width() const { return width_; }
    int height() const { return height_; }

    // Decodes one frame onto the previous one (skipped blocks keep their pixels). Throws FormatError when
    // the data ends before every block is coded.
    void decode(std::span<const std::uint8_t> frame);
    // The 8-bit stream's palette (R, G, B, A); 16-bit streams ignore it.
    std::vector<std::array<std::uint8_t, 4>> palette;

    // The current image as top-down RGBA8 (width × height × 4). RGB555 channels expand as (v << 3) | (v >> 2).
    void to_rgba(Bytes& out) const;
    // Raw pixels, bottom-up: RGB555 words (16 bit) or palette indices (8 bit).
    const std::vector<std::uint16_t>& pixels() const { return pixels_; }

private:
    int width_, height_, bits_;
    std::vector<std::uint16_t> pixels_;
};

} // namespace parkan
