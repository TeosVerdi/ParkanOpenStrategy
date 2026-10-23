// The shell's 2D presenter: draws OverlayFrame quads (menus, text, intro video frames) with SDL's 2D
// renderer, outside a mission. The mission renderer (app/scene_viewer, SDL_GPU) claims the window for
// itself, so the shell drops its Screen2D before a mission starts and makes a new one afterwards.
//
// Port decisions:
// - The 640×480 virtual screen is drawn in the largest centred 4:3 rectangle of the window (black bars
//   on other aspect ratios); the original ran full screen at a 4:3 display mode.
// - Textures sample nearest by default (bitmap fonts and sprites stay sharp when scaled, as in the HUD).
#pragma once

#include "ui/overlay.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace parkan {

class Screen2D {
public:
    explicit Screen2D(SDL_Window* window); // throws std::runtime_error when no renderer can be made
    ~Screen2D();
    Screen2D(const Screen2D&) = delete;
    Screen2D& operator=(const Screen2D&) = delete;

    // RGBA8 texture; returns its slot (OverlayQuad::texture). `streaming` textures can be updated each frame.
    std::int32_t add_texture(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba, bool streaming = false,
                             bool linear = false);
    void update_texture(std::int32_t slot, const std::uint8_t* rgba);

    // The 4:3 area in drawable pixels (the frame's width/height) and its offset in the window.
    struct Area {
        float x = 0, y = 0, width = 640, height = 480;
        float scale = 1; // drawable pixels per window point
    };
    Area area() const;
    // Window point → virtual 640×480 coordinates.
    void to_virtual(float wx, float wy, float& vx, float& vy) const;

    // Clears to black, draws the frame's quads in the 4:3 area and presents.
    void present(const OverlayFrame& frame);
    // Draws the frame offscreen and writes a BMP of the whole drawable; false on failure.
    bool save_bmp(const OverlayFrame& frame, const std::string& path);
    SDL_Renderer* renderer() const { return renderer_; }

private:
    void draw(const OverlayFrame& frame);

    SDL_Window* window_;
    SDL_Renderer* renderer_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    std::vector<std::uint32_t> widths_;
};

} // namespace parkan
