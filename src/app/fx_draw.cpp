#include "app/fx_draw.hpp"

#include "scene/night_vision.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace parkan {

namespace {

using namespace fx;

constexpr const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VIn {
    float3 pos   [[attribute(0)]];
    float2 uv    [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct VOut {
    float4 pos [[position]];
    float2 uv;
    float4 color;
};
struct VU { float4x4 mvp; };
struct FU { float4 params; }; // x: textured
vertex VOut vs_main(VIn in [[stage_in]], constant VU& u [[buffer(0)]]) {
    VOut o;
    o.pos = u.mvp * float4(in.pos, 1.0);
    o.uv = in.uv;
    o.color = in.color;
    return o;
}
fragment float4 fs_main(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],
                        constant FU& f [[buffer(0)]]) {
    float4 c = f.params.x > 0.5 ? tex.sample(smp, in.uv) : float4(1.0);
    return c * in.color;
}
)";

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

void upload(SDL_GPUDevice* dev, const void* data, Uint32 size, SDL_GPUBuffer* buf, SDL_GPUTexture* tex, Uint32 tw = 0,
            Uint32 th = 0) {
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) fail("SDL_CreateGPUTransferBuffer");
    std::memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), data, size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    if (buf) {
        SDL_GPUTransferBufferLocation src{tb, 0};
        SDL_GPUBufferRegion dst{buf, 0, size};
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    } else {
        SDL_GPUTextureTransferInfo src{tb, 0, tw, th};
        SDL_GPUTextureRegion dst{};
        dst.texture = tex;
        dst.w = tw, dst.h = th, dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

SDL_GPUTexture* make_texture(SDL_GPUDevice* dev, Uint32 w, Uint32 h, const void* rgba) {
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w, ci.height = h, ci.layer_count_or_depth = 1, ci.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &ci);
    if (!t) fail("SDL_CreateGPUTexture");
    upload(dev, rgba, w * h * 4, nullptr, t, w, h);
    return t;
}

} // namespace

FxRenderer::FxRenderer(SDL_GPUDevice* dev, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format) : dev_(dev) {
    auto shader = [&](const char* entry, SDL_GPUShaderStage stage, Uint32 samplers) {
        SDL_GPUShaderCreateInfo info{};
        info.code = reinterpret_cast<const Uint8*>(kShader);
        info.code_size = std::strlen(kShader);
        info.entrypoint = entry;
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = 1;
        SDL_GPUShader* s = SDL_CreateGPUShader(dev_, &info);
        if (!s) fail("SDL_CreateGPUShader");
        return s;
    };
    SDL_GPUShader* vs = shader("vs_main", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fs = shader("fs_main", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUVertexBufferDescription vbd{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attrs[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, pos)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, color)},
    };
    for (int i = 0; i < 10; ++i) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = color_format;
        auto& b = ct.blend_state;
        b.color_blend_op = b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO, b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        // Ngi32 blend presets (notes/ngi32-platform.md): 4 alpha, 2 additive, 3 multiply, 5 2× multiply.
        switch (i % 5) {
        case 1: b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA, b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; break;
        case 2: b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA, b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;
        case 3: b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO, b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR; break;
        case 4: b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR, b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR; break;
        default: break;
        }
        b.enable_blend = i % 5 != 0;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs, pi.fragment_shader = fs;
        pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 3;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        // Transparent effects test depth but do not write it; on-top glows skip the test.
        pi.depth_stencil_state.enable_depth_test = i < 5;
        pi.depth_stencil_state.enable_depth_write = i == 0;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = depth_format;
        pi.target_info.has_depth_stencil_target = true;
        pipes_[i] = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
        if (!pipes_[i]) fail("SDL_CreateGPUGraphicsPipeline (effects)");
    }
    SDL_ReleaseGPUShader(dev_, vs);
    SDL_ReleaseGPUShader(dev_, fs);
    // REPEAT: beam UVs run past the atlas frame (notes/effects.md).
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_ = SDL_CreateGPUSampler(dev_, &si);
    const Uint32 white_px = 0xFFFFFFFF;
    white_ = make_texture(dev_, 1, 1, &white_px);
}

FxRenderer::~FxRenderer() {
    if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
    for (auto* t : textures_)
        if (t != white_) SDL_ReleaseGPUTexture(dev_, t);
    SDL_ReleaseGPUTexture(dev_, white_);
    SDL_ReleaseGPUSampler(dev_, sampler_);
    for (auto* p : pipes_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
}

void FxRenderer::prepare(const FxFrame& f, Vec3 eye, const std::vector<ModelTexture>& textures, bool night_vision) {
    while (textures_.size() < textures.size()) { // materials load lazily as instances are created
        const auto& t = textures[textures_.size()];
        textures_.push_back(t.rgba.empty() ? white_ : make_texture(dev_, t.width, t.height, t.rgba.data()));
    }
    // Opaque debris first, then transparent quads back to front.
    verts_.clear(), batches_.clear();
    auto add = [&](int pipe, std::int32_t tex) {
        if (batches_.empty() || batches_.back().pipe != pipe || batches_.back().texture != tex)
            batches_.push_back({pipe, tex, Uint32(verts_.size()), 0});
    };
    auto vert = [&](Vec3 p, const float* uv, const float* c) {
        float rgb[3] = {c[0], c[1], c[2]};
        // Packed (clamped) conversion of the vertex colour, alpha kept. [inferred] the colour is the quad's own
        // (the original builds it from the material's +0x14 quad); debris triangles take the same path.
        if (night_vision) {
            const Vec3 n = night_vision_rgb({c[0], c[1], c[2]});
            rgb[0] = n.x, rgb[1] = n.y, rgb[2] = n.z;
        }
        verts_.push_back({{p.x, p.y, p.z}, {uv[0], uv[1]}, {rgb[0], rgb[1], rgb[2], c[3]}});
        ++batches_.back().count;
    };
    // Debris is flat-shaded against a fixed light (inferred; the original lights it with the scene).
    const Vec3 sun = normalize(Vec3{0.4f, -0.5f, 0.75f});
    for (const auto& t : f.triangles) {
        const float shade = 0.45f + 0.55f * std::max(0.f, std::abs(dot(t.normal, sun)));
        const float c[4] = {t.rgba[0] * shade, t.rgba[1] * shade, t.rgba[2] * shade, 1};
        add(t.blend == FxBlend::Opaque ? 0 : 1, t.texture);
        for (int k = 0; k < 3; ++k) vert(t.p[k], t.uv[k], c);
    }
    std::vector<const FxQuad*> order;
    for (const auto& q : f.quads) order.push_back(&q);
    auto dist = [&](const FxQuad* q) {
        const Vec3 c = (q->p[0] + q->p[2]) * 0.5f - eye;
        return dot(c, c);
    };
    std::stable_sort(order.begin(), order.end(), [&](const FxQuad* a, const FxQuad* b) { return dist(a) > dist(b); });
    for (const FxQuad* q : order) {
        int pipe = 1;
        switch (q->blend) {
        case FxBlend::Opaque: pipe = 0; break;
        case FxBlend::Alpha: pipe = 1; break;
        case FxBlend::Additive: pipe = 2; break;
        case FxBlend::Multiply: pipe = 3; break;
        case FxBlend::Multiply2x: pipe = 4; break;
        }
        if (q->on_top) pipe += 5;
        add(pipe, q->texture);
        static constexpr int tri[6] = {0, 1, 2, 0, 2, 3};
        for (int k : tri) vert(q->p[k], q->uv[k], q->rgba);
    }
    const Uint32 bytes = Uint32(verts_.size() * sizeof(Vertex));
    if (bytes > vcap_) {
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        vcap_ = bytes * 2;
        SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, vcap_, 0};
        vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
        if (!vbuf_) fail("SDL_CreateGPUBuffer (effects)");
    }
    if (bytes) upload(dev_, verts_.data(), bytes, vbuf_, nullptr);
}

void FxRenderer::draw(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* rp, const Mat4& vp) {
    if (verts_.empty()) return;
    SDL_GPUBufferBinding vb{vbuf_, 0};
    for (const Batch& b : batches_) {
        if (!b.count) continue;
        SDL_BindGPUGraphicsPipeline(rp, pipes_[b.pipe]);
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_PushGPUVertexUniformData(cb, 0, &vp, sizeof vp);
        const float params[4] = {b.texture >= 0 ? 1.f : 0.f, 0, 0, 0};
        SDL_PushGPUFragmentUniformData(cb, 0, params, sizeof params);
        SDL_GPUTextureSamplerBinding tb{b.texture >= 0 && std::size_t(b.texture) < textures_.size() ? textures_[std::size_t(b.texture)] : white_,
                                        sampler_};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        SDL_DrawGPUPrimitives(rp, b.count, 1, b.first, 0);
    }
}

} // namespace parkan
