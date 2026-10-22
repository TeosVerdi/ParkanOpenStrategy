#include "app/fx_viewer.hpp"

#include "fx/effect.hpp"
#include "res/library.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

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

struct Vertex {
    float pos[3];
    float uv[2];
    float color[4];
};

// One run of vertices drawn with one pipeline and texture.
struct Batch {
    int pipe = 0; // 0 opaque, 1 alpha, 2 additive, 3 multiply, 4 multiply 2x; +5: no depth test
    std::int32_t texture = -1;
    Uint32 first = 0, count = 0;
};

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

// The frame as vertices: opaque first (ground, debris), then transparent quads back to front.
void build_vertices(const FxFrame& f, Vec3 eye, std::vector<Vertex>& v, std::vector<Batch>& batches) {
    v.clear(), batches.clear();
    auto add = [&](int pipe, std::int32_t tex) {
        if (batches.empty() || batches.back().pipe != pipe || batches.back().texture != tex)
            batches.push_back({pipe, tex, Uint32(v.size()), 0});
    };
    auto vert = [&](Vec3 p, const float* uv, const float* c) {
        Vertex x{{p.x, p.y, p.z}, {uv[0], uv[1]}, {c[0], c[1], c[2], c[3]}};
        v.push_back(x);
        ++batches.back().count;
    };
    // Ground: a 60 m checkerboard in the XY plane (the world is Z-up).
    const float zero[2] = {0, 0};
    for (int i = -6; i < 6; ++i)
        for (int j = -6; j < 6; ++j) {
            const float g = ((i + j) & 1) ? 0.22f : 0.27f, c[4] = {g, g, g * 1.05f, 1};
            const float x0 = float(i) * 5, y0 = float(j) * 5, x1 = x0 + 5, y1 = y0 + 5;
            add(0, -1);
            vert({x0, y0, 0}, zero, c), vert({x1, y0, 0}, zero, c), vert({x1, y1, 0}, zero, c);
            vert({x0, y0, 0}, zero, c), vert({x1, y1, 0}, zero, c), vert({x0, y1, 0}, zero, c);
        }
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
}

} // namespace

int run_effect_viewer(SDL_Window* window, AssetLibrary& lib, const EffectViewerOptions& opt) {
    auto fx = load_effect(lib, opt.effect);
    if (!fx) throw std::runtime_error("effect not found in effects.rlb: " + opt.effect);
    FxContext ctx(lib);
    const Mat4 placement = Mat4::identity();
    std::optional<EffectInstance> inst(std::in_place, fx, ctx, placement, 0.0);
    std::vector<FxSoundEvent> sounds;
    // Count what the template holds, for the title.
    int counts[11] = {};
    for (const auto& c : fx->list) ++counts[c.opcode];
    std::string summary;
    for (int op = 1; op <= 10; ++op)
        if (counts[op]) summary += " op" + std::to_string(op) + "×" + std::to_string(counts[op]);
    SDL_Log("effect %s: %zu commands (%s), mode %u, %.2f s, flags 0x%x", opt.effect.c_str(), fx->list.size(),
            summary.c_str() + (summary.empty() ? 0 : 1), fx->header.time_mode, fx->header.duration, fx->header.flags);

    // Simulate to the requested time at 30 Hz from 0 (trails and falling particles integrate).
    double sim = 0;
    // Time modes other than 1..3 read the owner (gun state, speed…): the viewer sweeps that quantity
    // 0 → 1 every second instead.
    const bool owner_driven = fx->header.time_mode == 0 || fx->header.time_mode > 3;
    // Beams (op 5) and trails (op 8) only show on a moving effect: fly it along +X at 20 m/s, 3 m up.
    const bool moving = counts[5] || counts[8];
    auto place = [&](EffectInstance& e, double t) {
        if (!e.enabled()) e.set_enabled(true, t); // header flag 0x40: the owner enables it later
        if (owner_driven) e.set_param(float(t - std::floor(t)));
        if (moving) e.set_transform(Mat4::translation({float(-10 + 20 * t), 0, 3}));
    };
    auto step_to = [&](double target) {
        while (sim < target) {
            sim = std::min(target, sim + 1.0 / 30);
            sounds.clear();
            if (inst) place(*inst, sim);
            if (inst && !inst->update(sim, &sounds)) inst.reset();
        }
    };
    // Fit: most of the sprite corners over the first seconds (debris flies far, so it is ignored).
    Vec3 centre{0, 0, 0};
    float radius = 1.5f;
    if (opt.distance <= 0) {
        FxContext probe_ctx(lib);
        EffectInstance probe(fx, probe_ctx, placement, 0.0);
        FxFrame f;
        std::vector<Vec3> pts;
        const double until = std::clamp(std::max(opt.time, double(fx->header.duration)), 1.0, 4.0);
        for (double t = 0; t <= until; t += 1.0 / 30) {
            place(probe, t);
            if (!probe.update(t)) break;
            f.clear();
            probe.render(f, {0, -30, 10});
            for (const auto& q : f.quads) pts.push_back((q.p[0] + q.p[2]) * 0.5f);
        }
        if (!pts.empty()) {
            Vec3 sum{};
            for (const auto& p : pts) sum = sum + p;
            centre = sum * (1.f / float(pts.size()));
            std::vector<float> d;
            for (const auto& p : pts) d.push_back(std::sqrt(dot(p - centre, p - centre)));
            std::sort(d.begin(), d.end());
            radius = std::clamp(d[d.size() * 9 / 10] + 1.f, 1.5f, 60.f);
        }
    }
    float yaw = -1.5707964f, pitch = opt.pitch, dist = opt.distance > 0 ? opt.distance : radius * 2.5f;
    Vec3 look_at = centre;

    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!dev) fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(dev, window)) fail("SDL_ClaimWindowForGPUDevice");
    const SDL_GPUTextureFormat color_format = SDL_GetGPUSwapchainTextureFormat(dev, window);
    SDL_GPUGraphicsPipeline* pipes[10] = {};
    {
        auto shader = [&](const char* entry, SDL_GPUShaderStage stage, Uint32 samplers) {
            SDL_GPUShaderCreateInfo info{};
            info.code = reinterpret_cast<const Uint8*>(kShader);
            info.code_size = std::strlen(kShader);
            info.entrypoint = entry;
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.stage = stage;
            info.num_samplers = samplers;
            info.num_uniform_buffers = 1;
            SDL_GPUShader* s = SDL_CreateGPUShader(dev, &info);
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
            pi.depth_stencil_state.enable_depth_test = i < 5;
            pi.depth_stencil_state.enable_depth_write = i == 0;
            pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            pi.target_info.has_depth_stencil_target = true;
            pipes[i] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (!pipes[i]) fail("SDL_CreateGPUGraphicsPipeline");
        }
        SDL_ReleaseGPUShader(dev, vs);
        SDL_ReleaseGPUShader(dev, fs);
    }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(dev, &si);
    const Uint32 white_px = 0xFFFFFFFF;
    SDL_GPUTexture* white = make_texture(dev, 1, 1, &white_px);
    std::vector<SDL_GPUTexture*> textures;
    auto sync_textures = [&] { // materials load lazily as instances are created
        while (textures.size() < ctx.materials.textures.size()) {
            const auto& t = ctx.materials.textures[textures.size()];
            textures.push_back(t.rgba.empty() ? white : make_texture(dev, t.width, t.height, t.rgba.data()));
        }
    };
    sync_textures();
    SDL_GPUTexture* depth = nullptr;
    Uint32 depth_w = 0, depth_h = 0;
    SDL_GPUBuffer* vbuf = nullptr;
    Uint32 vcap = 0;
    std::vector<Vertex> verts;
    std::vector<Batch> batches;
    FxFrame frame;

    auto render = [&](SDL_GPUCommandBuffer* cb, SDL_GPUTexture* target, Uint32 w, Uint32 h) {
        if (w != depth_w || h != depth_h) {
            if (depth) SDL_ReleaseGPUTexture(dev, depth);
            SDL_GPUTextureCreateInfo di{};
            di.type = SDL_GPU_TEXTURETYPE_2D;
            di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            di.width = w, di.height = h, di.layer_count_or_depth = 1, di.num_levels = 1;
            depth = SDL_CreateGPUTexture(dev, &di);
            depth_w = w, depth_h = h;
        }
        const Vec3 eye = look_at + Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)} * dist;
        frame.clear();
        if (inst) inst->render(frame, eye);
        sync_textures();
        build_vertices(frame, eye, verts, batches);
        const Uint32 bytes = Uint32(verts.size() * sizeof(Vertex));
        if (bytes > vcap) {
            if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
            vcap = bytes * 2;
            SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, vcap, 0};
            vbuf = SDL_CreateGPUBuffer(dev, &bi);
            if (!vbuf) fail("SDL_CreateGPUBuffer");
        }
        if (bytes) upload(dev, verts.data(), bytes, vbuf, nullptr);
        const float aspect = float(w) / float(std::max<Uint32>(h, 1));
        const Mat4 vp = Mat4::perspective(0.8f, aspect, 0.1f, 2000.f) * Mat4::look_at(eye, look_at, {0, 0, 1});
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.clear_color = {0.30f, 0.36f, 0.45f, 1};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = depth;
        dt.clear_depth = 1;
        dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &color, 1, &dt);
        SDL_GPUBufferBinding vb{vbuf, 0};
        for (const Batch& b : batches) {
            if (!b.count) continue;
            SDL_BindGPUGraphicsPipeline(rp, pipes[b.pipe]);
            SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
            SDL_PushGPUVertexUniformData(cb, 0, &vp, sizeof vp);
            const float params[4] = {b.texture >= 0 ? 1.f : 0.f, 0, 0, 0};
            SDL_PushGPUFragmentUniformData(cb, 0, params, sizeof params);
            SDL_GPUTextureSamplerBinding tb{b.texture >= 0 ? textures[std::size_t(b.texture)] : white, sampler};
            SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
            SDL_DrawGPUPrimitives(rp, b.count, 1, b.first, 0);
        }
        SDL_EndGPURenderPass(rp);
    };
    auto title = [&] {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s  t=%.2f s  %zu quads %zu tris %zu lights%s", opt.effect.c_str(), sim,
                      frame.quads.size(), frame.triangles.size(), frame.lights.size(), inst ? "" : "  (finished)");
        SDL_SetWindowTitle(window, buf);
    };

    int rc = 0;
    if (!opt.shot_path.empty()) {
        step_to(opt.time);
        const Uint32 w = 1024, h = 768;
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = color_format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ci.width = w, ci.height = h, ci.layer_count_or_depth = 1, ci.num_levels = 1;
        SDL_GPUTexture* off = SDL_CreateGPUTexture(dev, &ci);
        SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        render(cb, off, w, h);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureRegion src{};
        src.texture = off;
        src.w = w, src.h = h, src.d = 1;
        SDL_GPUTextureTransferInfo dst{tb, 0, w, h};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void* px = SDL_MapGPUTransferBuffer(dev, tb, false);
        const bool bgra = color_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        SDL_Surface* surf = SDL_CreateSurfaceFrom(int(w), int(h), bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888, px, int(w * 4));
        const bool ok = surf && SDL_SaveBMP(surf, opt.shot_path.c_str());
        SDL_DestroySurface(surf);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTexture(dev, off);
        SDL_Log("screenshot %s (t = %.2f s, %zu quads, %zu triangles, %zu lights): %s", opt.shot_path.c_str(), sim,
                frame.quads.size(), frame.triangles.size(), frame.lights.size(), ok ? "saved" : SDL_GetError());
        rc = ok ? 0 : 1;
    } else {
        bool running = true, paused = false, dragging = false;
        double clock = 0;
        Uint64 last = SDL_GetTicksNS();
        const double loop_after = std::max(1.0, double(fx->header.duration)) + 1.0;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: dragging = true; break;
                case SDL_EVENT_MOUSE_BUTTON_UP: dragging = false; break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (dragging) {
                        yaw -= ev.motion.xrel * 0.01f;
                        pitch = std::clamp(pitch + ev.motion.yrel * 0.01f, -1.4f, 1.5f);
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL: dist = std::max(1.f, dist * (ev.wheel.y > 0 ? 0.9f : 1.1f)); break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) running = false;
                    if (ev.key.key == SDLK_SPACE) paused = !paused;
                    if (ev.key.key == SDLK_R) clock = loop_after;
                    break;
                }
            }
            const Uint64 now = SDL_GetTicksNS();
            if (!paused) clock += double(now - last) * 1e-9;
            last = now;
            if (clock >= loop_after) { // restart the effect
                clock = 0, sim = 0;
                inst.emplace(fx, ctx, placement, 0.0);
            }
            step_to(clock);
            SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUTexture* swap = nullptr;
            Uint32 w = 0, h = 0;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, window, &swap, &w, &h) || !swap) {
                SDL_SubmitGPUCommandBuffer(cb);
                continue;
            }
            render(cb, swap, w, h);
            SDL_SubmitGPUCommandBuffer(cb);
            title();
        }
    }

    if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
    if (depth) SDL_ReleaseGPUTexture(dev, depth);
    for (auto* t : textures)
        if (t != white) SDL_ReleaseGPUTexture(dev, t);
    SDL_ReleaseGPUTexture(dev, white);
    SDL_ReleaseGPUSampler(dev, sampler);
    for (auto* p : pipes) SDL_ReleaseGPUGraphicsPipeline(dev, p);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    return rc;
}

} // namespace parkan
