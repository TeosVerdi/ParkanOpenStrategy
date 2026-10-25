// Screen-space 2D drawing (HUD, text), in pixels from the top-left corner of the drawable. The
// viewer draws the quads after the scene (app/scene_viewer); building them needs no SDL.
#pragma once

#include "scene/render_mesh.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace parkan {

struct OverlayQuad {
    float x0, y0, x1, y1;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    std::uint8_t rgba[4] = {255, 255, 255, 255};
    std::int32_t texture = -1; // into ViewerOptions::overlay_textures; -1 = solid colour
    // A free-form quad (mirrored or rotated sprites, fans, lines): when set, the corners are
    // (x[k], y[k]) with texture coordinates (u[k], v[k]) in the order TL, TR, BR, BL (triangles 0-1-2,
    // 0-2-3; a triangle repeats a corner) and x0..v1 are ignored.
    bool free = false;
    float x[4] = {}, y[4] = {}, u[4] = {}, v[4] = {};
};
// A small 3D view inside the 2D overlay (the HQ's model previews, CModelView_Draw 1009edd0): `mesh` posed by
// `transforms`, seen from its own camera, drawn into the drawable-pixel rect after the first `after_quads` quads
// (so 2D drawn later lies on top). Always in front of the world (its own depth range).
struct OverlayInset {
    std::shared_ptr<const RenderMesh> mesh; // GPU buffers are cached by this pointer
    std::vector<Mat4> transforms;           // empty: mesh->transforms
    Vec3 eye, forward, up{0, 0, 1};
    float fov_x = 1.f, znear = 0.1f, zfar = 1000.f;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::uint8_t tint[4] = {255, 255, 255, 255}; // multiplies the texels (e.g. the placement ghost's colour)
    bool world = false; // drawn into the world (full viewport, the scene's depth): the placement ghost
    std::size_t after_quads = 0;
};
struct OverlayFrame {
    std::uint32_t width = 0, height = 0; // drawable size in pixels
    float scale = 1;                     // pixels per point (Retina: 2)
    std::vector<OverlayQuad> quads;      // drawn in order, alpha-blended
    std::vector<OverlayInset> insets;    // in after_quads order
};

} // namespace parkan
