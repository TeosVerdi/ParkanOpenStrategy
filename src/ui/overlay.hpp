// Screen-space 2D drawing (HUD, text), in pixels from the top-left corner of the drawable. The
// viewer draws the quads after the scene (app/scene_viewer); building them needs no SDL.
#pragma once

#include <cstdint>
#include <vector>

namespace parkan {

struct OverlayQuad {
    float x0, y0, x1, y1;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    std::uint8_t rgba[4] = {255, 255, 255, 255};
    std::int32_t texture = -1; // into ViewerOptions::overlay_textures; -1 = solid colour
};
struct OverlayFrame {
    std::uint32_t width = 0, height = 0; // drawable size in pixels
    float scale = 1;                     // pixels per point (Retina: 2)
    std::vector<OverlayQuad> quads;      // drawn in order, alpha-blended
};

} // namespace parkan
