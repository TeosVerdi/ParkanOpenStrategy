#include "app/screen2d.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace parkan {

Screen2D::Screen2D(SDL_Window* window) : window_(window) {
    renderer_ = SDL_CreateRenderer(window, nullptr);
    if (!renderer_) throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    SDL_SetRenderVSync(renderer_, 1);
}

Screen2D::~Screen2D() {
    for (SDL_Texture* t : textures_) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(renderer_);
}

std::int32_t Screen2D::add_texture(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba, bool streaming, bool linear) {
    SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888,
                                       streaming ? SDL_TEXTUREACCESS_STREAMING : SDL_TEXTUREACCESS_STATIC, int(width), int(height));
    if (!t) {
        SDL_Log("screen2d: texture %ux%u: %s", width, height, SDL_GetError());
        return -1;
    }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    if (rgba) SDL_UpdateTexture(t, nullptr, rgba, int(width * 4));
    textures_.push_back(t);
    widths_.push_back(width);
    return std::int32_t(textures_.size() - 1);
}

void Screen2D::update_texture(std::int32_t slot, const std::uint8_t* rgba) {
    if (slot < 0 || std::size_t(slot) >= textures_.size()) return;
    SDL_UpdateTexture(textures_[std::size_t(slot)], nullptr, rgba, int(widths_[std::size_t(slot)] * 4));
}

Screen2D::Area Screen2D::area() const {
    int w = 0, h = 0, ww = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    SDL_GetWindowSize(window_, &ww, nullptr);
    Area a;
    a.scale = ww > 0 ? float(w) / float(ww) : 1.f;
    // The largest 4:3 rectangle, centred, whole pixels.
    a.width = std::min(float(w), std::floor(float(h) * 4.f / 3.f));
    a.height = std::floor(a.width * 3.f / 4.f);
    a.x = std::floor((float(w) - a.width) * 0.5f);
    a.y = std::floor((float(h) - a.height) * 0.5f);
    return a;
}

void Screen2D::to_virtual(float wx, float wy, float& vx, float& vy) const {
    const Area a = area();
    vx = (wx * a.scale - a.x) * 640.f / std::max(a.width, 1.f);
    vy = (wy * a.scale - a.y) * 480.f / std::max(a.height, 1.f);
}

void Screen2D::draw(const OverlayFrame& frame) {
    const Area a = area();
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_RenderClear(renderer_);
    const SDL_Rect vp{int(a.x), int(a.y), int(a.width), int(a.height)};
    SDL_SetRenderViewport(renderer_, &vp);
    std::vector<SDL_Vertex> verts;
    std::vector<int> idx;
    std::int32_t run_tex = -2;
    auto flush = [&] {
        if (verts.empty()) return;
        SDL_Texture* t = run_tex >= 0 && std::size_t(run_tex) < textures_.size() ? textures_[std::size_t(run_tex)] : nullptr;
        SDL_RenderGeometry(renderer_, t, verts.data(), int(verts.size()), idx.data(), int(idx.size()));
        verts.clear();
        idx.clear();
    };
    for (const OverlayQuad& q : frame.quads) {
        if (q.texture != run_tex) flush(), run_tex = q.texture;
        const SDL_FColor c{q.rgba[0] / 255.f, q.rgba[1] / 255.f, q.rgba[2] / 255.f, q.rgba[3] / 255.f};
        const int base = int(verts.size());
        if (q.free) {
            for (int k = 0; k < 4; ++k) verts.push_back({{q.x[k], q.y[k]}, c, {q.u[k], q.v[k]}});
        } else {
            verts.push_back({{q.x0, q.y0}, c, {q.u0, q.v0}});
            verts.push_back({{q.x1, q.y0}, c, {q.u1, q.v0}});
            verts.push_back({{q.x1, q.y1}, c, {q.u1, q.v1}});
            verts.push_back({{q.x0, q.y1}, c, {q.u0, q.v1}});
        }
        for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(base + k);
    }
    flush();
    SDL_SetRenderViewport(renderer_, nullptr);
}

void Screen2D::present(const OverlayFrame& frame) {
    draw(frame);
    SDL_RenderPresent(renderer_);
}

bool Screen2D::save_bmp(const OverlayFrame& frame, const std::string& path) {
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    SDL_Texture* target = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!target) return false;
    SDL_SetRenderTarget(renderer_, target);
    draw(frame);
    SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr);
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_DestroyTexture(target);
    const bool ok = s && SDL_SaveBMP(s, path.c_str());
    if (s) SDL_DestroySurface(s);
    return ok;
}

} // namespace parkan
