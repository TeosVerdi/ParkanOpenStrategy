// Model materials: WEAR (row → material name, optional lightmaps) and MAT0 (phases with texture
// names). Specs: FParkan reference/materials, notes/formats-wea-cpt-ndp.md.
// Native: World3D CMatManager (LoadMatManager 10002aa0, ParseWear 10003f80).
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

struct Wear {
    std::vector<std::string> materials; // by row (the number at the line start is legacy, ignored)
    std::vector<std::string> lightmaps;
};

Wear parse_wear(std::string_view text);

struct MaterialPhase {
    std::array<std::uint8_t, 18> params{}; // p0..2 additive RGB/255, p3 opacity %, ... p16 power, p17 page
    std::string texture;                   // empty = untextured
    float opacity() const { return params[3] * 0.01f; }
};

struct MaterialAnimKey {
    std::uint16_t phase, end_time_ms, raw;
};

struct MaterialAnimBlock {
    std::uint32_t header = 0; // & 7 = mode (0 loop, 1 ping-pong, 2 clamp, 3 random), >> 3 = blend mask
    std::vector<MaterialAnimKey> keys;
};

struct Material {
    std::uint32_t flags = 0;   // NRes attr1; (flags >> 2) & 0xF = blend category
    std::uint32_t version = 0; // NRes attr2
    std::array<std::uint8_t, 10> prefix{}; // version >= 6 header extension (meaning open)
    std::vector<MaterialPhase> phases;
    std::vector<MaterialAnimBlock> anims;
    unsigned category() const { return (flags >> 2) & 0xF; }
};

Material decode_mat0(std::span<const std::uint8_t> data, std::uint32_t attr1, std::uint32_t attr2);

} // namespace parkan
