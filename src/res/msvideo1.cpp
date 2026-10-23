#include "res/msvideo1.hpp"

namespace parkan {

MsVideo1Decoder::MsVideo1Decoder(int width, int height, int bit_count) : width_(width), height_(height), bits_(bit_count) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) throw FormatError("msvideo1: bad frame size");
    if (bit_count != 8 && bit_count != 16) throw FormatError("msvideo1: unsupported bit depth " + std::to_string(bit_count));
    pixels_.assign(std::size_t(width) * std::size_t(height), 0);
}

void MsVideo1Decoder::decode(std::span<const std::uint8_t> frame) {
    const int blocks_wide = width_ / 4, blocks_high = height_ / 4;
    std::size_t total = std::size_t(blocks_wide) * std::size_t(blocks_high);
    std::size_t p = 0;
    auto need = [&](std::size_t n) {
        if (frame.size() - p < n) throw FormatError("msvideo1: frame data ends early");
    };
    auto rd16 = [&] {
        const std::uint16_t v = std::uint16_t(frame[p] | (frame[p + 1] << 8));
        p += 2;
        return v;
    };
    std::size_t skip = 0;
    const std::size_t stride = std::size_t(width_);
    // Block rows bottom-up in the image = top-down in the DIB's memory order (row 0 is the bottom).
    for (int by = 0; by < blocks_high; ++by) {
        for (int bx = 0; bx < blocks_wide; ++bx) {
            if (skip) {
                --skip;
                --total;
                continue;
            }
            need(2);
            const std::uint8_t a = frame[p], b = frame[p + 1];
            p += 2;
            // Pixel (x, y) of the block, y = 0 its bottom row (lowest DIB row).
            std::uint16_t* base = pixels_.data() + std::size_t(by) * 4 * stride + std::size_t(bx) * 4;
            auto put = [&](int x, int y, std::uint16_t c) { base[std::size_t(y) * stride + std::size_t(x)] = c; };
            if (a == 0 && b == 0 && total == 0) return;
            if ((b & 0xFC) == 0x84) {
                skip = (std::size_t(b - 0x84) << 8) + a; // this block included
                --skip;
                --total;
                continue;
            }
            std::uint16_t flags = std::uint16_t((b << 8) | a);
            if (bits_ == 16) {
                if (b < 0x80) {
                    need(4);
                    std::uint16_t c[8];
                    c[0] = rd16(), c[1] = rd16();
                    if (c[0] & 0x8000) {
                        need(12);
                        for (int i = 2; i < 8; ++i) c[i] = rd16();
                        for (int y = 0; y < 4; ++y)
                            for (int x = 0; x < 4; ++x, flags >>= 1)
                                put(x, y, c[((y & 2) << 1) + (x & 2) + ((flags & 1) ^ 1)]);
                    } else {
                        for (int y = 0; y < 4; ++y)
                            for (int x = 0; x < 4; ++x, flags >>= 1) put(x, y, c[(flags & 1) ^ 1]);
                    }
                } else {
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) put(x, y, flags);
                }
            } else {
                if (b < 0x80) {
                    need(2);
                    const std::uint16_t c[2] = {frame[p], frame[p + 1]};
                    p += 2;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x, flags >>= 1) put(x, y, c[(flags & 1) ^ 1]);
                } else if (b >= 0x90) {
                    need(8);
                    std::uint16_t c[8];
                    for (int i = 0; i < 8; ++i) c[i] = frame[p + std::size_t(i)];
                    p += 8;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x, flags >>= 1) put(x, y, c[((y & 2) << 1) + (x & 2) + ((flags & 1) ^ 1)]);
                } else {
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) put(x, y, a);
                }
            }
            --total;
        }
    }
}

void MsVideo1Decoder::to_rgba(Bytes& out) const {
    out.resize(pixels_.size() * 4);
    std::uint8_t* d = out.data();
    for (int y = 0; y < height_; ++y) {
        const std::uint16_t* row = pixels_.data() + std::size_t(height_ - 1 - y) * std::size_t(width_);
        for (int x = 0; x < width_; ++x, d += 4) {
            const std::uint16_t v = row[x];
            if (bits_ == 16) {
                const unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                d[0] = std::uint8_t((r << 3) | (r >> 2));
                d[1] = std::uint8_t((g << 3) | (g >> 2));
                d[2] = std::uint8_t((b << 3) | (b >> 2));
            } else if (v < palette.size()) {
                d[0] = palette[v][0], d[1] = palette[v][1], d[2] = palette[v][2];
            } else {
                d[0] = d[1] = d[2] = 0;
            }
            d[3] = 255;
        }
    }
}

} // namespace parkan
