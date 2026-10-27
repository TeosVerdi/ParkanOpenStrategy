// Draws an fx::FxFrame (effect sprites, beams, trails, debris) inside a scene render pass: the five
// Ngi32 blend presets with and without depth test, transparent quads back to front, effect textures
// uploaded as the materials load them (notes/effects.md "Rendering"). Shared by the effect viewer and
// the scene viewer.
#pragma once

#include "core/math.hpp"
#include "fx/effect.hpp"

#include <SDL3/SDL.h>

#include <vector>

namespace parkan {

class FxRenderer {
public:
    // `color` and `depth` are the render pass's target formats.
    FxRenderer(SDL_GPUDevice* device, SDL_GPUTextureFormat color, SDL_GPUTextureFormat depth);
    ~FxRenderer();
    FxRenderer(const FxRenderer&) = delete;
    FxRenderer& operator=(const FxRenderer&) = delete;

    // Before the render pass: uploads textures added since the last call and this frame's vertices.
    // `night_vision`: sprite colours go through ConvertToNightVisionColor (1002f230, notes/infrared-camera.md §4.2).
    void prepare(const fx::FxFrame& frame, Vec3 eye, const std::vector<ModelTexture>& textures, bool night_vision = false);
    // Inside the render pass, after the opaque scene.
    void draw(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* rp, const Mat4& view_proj);

private:
    struct Vertex {
        float pos[3];
        float uv[2];
        float color[4];
    };
    struct Batch {
        int pipe = 0; // 0 opaque, 1 alpha, 2 additive, 3 multiply, 4 multiply 2x; +5: no depth test
        std::int32_t texture = -1;
        Uint32 first = 0, count = 0;
    };
    SDL_GPUDevice* dev_;
    SDL_GPUGraphicsPipeline* pipes_[10] = {};
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* white_ = nullptr;
    std::vector<SDL_GPUTexture*> textures_;
    SDL_GPUBuffer* vbuf_ = nullptr;
    Uint32 vcap_ = 0;
    std::vector<Vertex> verts_;
    std::vector<Batch> batches_;
};

} // namespace parkan
