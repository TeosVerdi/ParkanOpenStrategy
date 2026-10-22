#include "app/scene_viewer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace parkan {

namespace {

constexpr const char* kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VIn {
    float3 pos   [[attribute(0)]];
    float4 nrm   [[attribute(1)]];
    float2 uv0   [[attribute(2)]];
    float2 uv1   [[attribute(3)]];
    float  blend [[attribute(4)]];
    float4 color [[attribute(5)]];
};
struct VOut {
    float4 pos [[position]];
    float3 light;
    float2 uv0;
    float2 uv1;
    float  blend;
    float  fog;
    float4 color;
};
struct VUniforms {
    float4x4 mvp;
    float4x4 model;
    float4 sun_dir;   // direction the light travels (world)
    float4 sun_col;
    float4 fill_dir;
    float4 fill_col;
    float4 ambient;
    float4 eye;       // xyz: eye position
    float4 fog;       // x: start, y: end (0 = no fog)
    float4 uv0x;      // uv0 * x + (z, w)
    float4 uv1x;
};
struct FUniforms {
    float4 params;    // x: alpha test, y: tex1 mode (1 detail ×2, 2 add), z: textured, w: vertex alpha
    float4 params2;   // x: multiply by the vertex colour
    float4 tint;
    float4 fog_col;
};

// CShade CPU lighting (ShadeIndexedStrided, notes/terrain-render.md §4): lights summed, then
// max(sum + emissive, ambient) per channel, then overbright compression into [0, 1].
float compress(float c) {
    if (c > 1.0) c = c <= 7.0 ? c / 6.0 + 5.0 / 6.0 : 2.0;
    return min(c, 1.0);
}

vertex VOut vs_main(VIn in [[stage_in]], constant VUniforms& u [[buffer(0)]]) {
    VOut o;
    float4 wp = u.model * float4(in.pos, 1.0);
    o.pos = u.mvp * float4(in.pos, 1.0);
    float3 n = normalize((u.model * float4(in.nrm.xyz, 0.0)).xyz);
    float3 acc = max(0.0, -dot(n, u.sun_dir.xyz)) * u.sun_col.rgb + max(0.0, -dot(n, u.fill_dir.xyz)) * u.fill_col.rgb;
    acc = max(acc, u.ambient.rgb);
    o.light = float3(compress(acc.r), compress(acc.g), compress(acc.b));
    o.uv0 = in.uv0 * u.uv0x.x + u.uv0x.zw;
    o.uv1 = in.uv1 * u.uv1x.x + u.uv1x.zw;
    o.blend = in.blend;
    o.color = in.color;
    // Software fog, quadratic in distance (ComputeVertexColorAndFog 1002f230).
    float3 d = wp.xyz - u.eye.xyz;
    float d2 = dot(d, d), s2 = u.fog.x * u.fog.x, e2 = u.fog.y * u.fog.y;
    o.fog = u.fog.y > 0.0 ? (d2 <= s2 ? 1.0 : saturate(1.0 - (d2 - s2) / max(e2 - s2, 1e-6))) : 1.0;
    return o;
}

fragment float4 fs_main(VOut in [[stage_in]],
                        texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]],
                        sampler smp0 [[sampler(0)]], sampler smp1 [[sampler(1)]],
                        constant FUniforms& u [[buffer(0)]]) {
    float4 c = u.params.z > 0.5 ? tex0.sample(smp0, in.uv0) : float4(0.7, 0.7, 0.7, 1.0);
    if (u.params.x > 0.5 && c.a < 0.5) discard_fragment();
    if (u.params.y > 1.5) c.rgb += tex1.sample(smp1, in.uv1).rgb;
    else if (u.params.y > 0.5) c.rgb *= tex1.sample(smp1, in.uv1).rgb * 2.0;
    float a = u.params.w > 0.5 ? saturate(in.blend) : 1.0;
    if (u.params2.x > 0.5) c *= in.color;
    c *= u.tint;
    float3 rgb = mix(u.fog_col.rgb, c.rgb * in.light, in.fog);
    return float4(rgb, a * c.a);
}
)";

struct Gpu {
    SDL_GPUDevice* device = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;       // opaque
    SDL_GPUGraphicsPipeline* pipeline_blend = nullptr; // SRCALPHA/INVSRCALPHA, depth LESS_OR_EQUAL
    SDL_GPUGraphicsPipeline* pipeline_sky = nullptr, *pipeline_sky_blend = nullptr; // no depth
    SDL_GPUBuffer* sky_vbuf = nullptr;
    SDL_GPUBuffer* sky_ibuf = nullptr;
    SDL_GPUTexture* sky_tex[3] = {};
    SDL_GPUBuffer* vbuf = nullptr;
    SDL_GPUBuffer* ibuf = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUTexture* white = nullptr;
    SDL_GPUTexture* depth = nullptr;
    Uint32 depth_w = 0, depth_h = 0;
    std::vector<SDL_GPUTexture*> textures;
};

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

SDL_GPUShader* make_shader(SDL_GPUDevice* dev, const char* entry, SDL_GPUShaderStage stage, Uint32 samplers) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(kShaderSource);
    info.code_size = std::strlen(kShaderSource);
    info.entrypoint = entry;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = 1;
    SDL_GPUShader* s = SDL_CreateGPUShader(dev, &info);
    if (!s) fail("SDL_CreateGPUShader");
    return s;
}

// Uploads `size` bytes into a buffer or a texture through a transfer buffer.
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
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.width = w, ci.height = h, ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    for (Uint32 s = std::max(w, h); s > 1; s >>= 1) ++ci.num_levels;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &ci);
    if (!t) fail("SDL_CreateGPUTexture");
    upload(dev, rgba, w * h * 4, nullptr, t, w, h);
    if (ci.num_levels > 1) {
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GenerateMipmapsForGPUTexture(cb, t);
        SDL_SubmitGPUCommandBuffer(cb);
    }
    return t;
}

void upload_mesh(Gpu& g, const RenderMesh& m) {
    const Uint32 vsize = Uint32(m.vertices.size() * sizeof(RenderVertex));
    const Uint32 isize = Uint32(m.indices.size() * 4);
    SDL_GPUBufferCreateInfo vb{SDL_GPU_BUFFERUSAGE_VERTEX, vsize, 0};
    SDL_GPUBufferCreateInfo ib{SDL_GPU_BUFFERUSAGE_INDEX, isize, 0};
    g.vbuf = SDL_CreateGPUBuffer(g.device, &vb);
    g.ibuf = SDL_CreateGPUBuffer(g.device, &ib);
    if (!g.vbuf || !g.ibuf) fail("SDL_CreateGPUBuffer");
    upload(g.device, m.vertices.data(), vsize, g.vbuf, nullptr);
    upload(g.device, m.indices.data(), isize, g.ibuf, nullptr);
    for (const auto& t : m.textures) g.textures.push_back(make_texture(g.device, t.width, t.height, t.rgba.data()));
}

void release_mesh(Gpu& g) {
    if (g.vbuf) SDL_ReleaseGPUBuffer(g.device, g.vbuf);
    if (g.ibuf) SDL_ReleaseGPUBuffer(g.device, g.ibuf);
    for (auto* t : g.textures) SDL_ReleaseGPUTexture(g.device, t);
    g.vbuf = g.ibuf = nullptr;
    g.textures.clear();
}

} // namespace

int run_scene_viewer(SDL_Window* window, RenderMesh mesh, const ViewerOptions& opt) {
    Gpu g;
    g.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!g.device) fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(g.device, window)) fail("SDL_ClaimWindowForGPUDevice");

    const SDL_GPUTextureFormat color_format = SDL_GetGPUSwapchainTextureFormat(g.device, window);
    {
        SDL_GPUShader* vs = make_shader(g.device, "vs_main", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* fs = make_shader(g.device, "fs_main", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
        SDL_GPUVertexBufferDescription vbd{0, sizeof(RenderVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        SDL_GPUVertexAttribute attrs[6] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(RenderVertex, pos)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM, offsetof(RenderVertex, nrm)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(RenderVertex, uv0)},
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(RenderVertex, uv1)},
            {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(RenderVertex, blend)},
            {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(RenderVertex, color)},
        };
        SDL_GPUColorTargetDescription ct{};
        ct.format = color_format;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs;
        pi.fragment_shader = fs;
        pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 6;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; // the original culls in software per batch flags
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.enable_depth_write = true;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        pi.target_info.has_depth_stencil_target = true;
        g.pipeline = SDL_CreateGPUGraphicsPipeline(g.device, &pi);
        if (!g.pipeline) fail("SDL_CreateGPUGraphicsPipeline");
        // Blend preset 4 (terrain overlay); it reuses the base layer's triangles, hence LESS_OR_EQUAL.
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        g.pipeline_blend = SDL_CreateGPUGraphicsPipeline(g.device, &pi);
        if (!g.pipeline_blend) fail("SDL_CreateGPUGraphicsPipeline (blend)");
        // Sky passes: no depth test or write; drawn first, behind everything.
        pi.depth_stencil_state.enable_depth_test = false;
        pi.depth_stencil_state.enable_depth_write = false;
        g.pipeline_sky_blend = SDL_CreateGPUGraphicsPipeline(g.device, &pi);
        ct.blend_state = {};
        g.pipeline_sky = SDL_CreateGPUGraphicsPipeline(g.device, &pi);
        if (!g.pipeline_sky || !g.pipeline_sky_blend) fail("SDL_CreateGPUGraphicsPipeline (sky)");
        SDL_ReleaseGPUShader(g.device, vs);
        SDL_ReleaseGPUShader(g.device, fs);
    }
    {
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.max_lod = 16;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        g.sampler = SDL_CreateGPUSampler(g.device, &si);
        const Uint32 white = 0xFFFFFFFF;
        g.white = make_texture(g.device, 1, 1, &white);
    }

    upload_mesh(g, mesh);
    if (opt.relative_mouse && opt.shot_path.empty()) SDL_SetWindowRelativeMouseMode(window, true);
    SkyDome dome;
    if (opt.sky) {
        dome = build_sky_dome(16);
        const Uint32 vsize = Uint32(dome.vertices.size() * sizeof(RenderVertex)), isize = Uint32(dome.indices.size() * 4);
        SDL_GPUBufferCreateInfo vb{SDL_GPU_BUFFERUSAGE_VERTEX, vsize, 0};
        SDL_GPUBufferCreateInfo ib{SDL_GPU_BUFFERUSAGE_INDEX, isize, 0};
        g.sky_vbuf = SDL_CreateGPUBuffer(g.device, &vb);
        g.sky_ibuf = SDL_CreateGPUBuffer(g.device, &ib);
        if (!g.sky_vbuf || !g.sky_ibuf) fail("SDL_CreateGPUBuffer (sky)");
        upload(g.device, dome.indices.data(), isize, g.sky_ibuf, nullptr);
        for (std::size_t i = 0; i < 3; ++i) {
            const ModelTexture* t = i < opt.sky_textures.textures.size() ? &opt.sky_textures.textures[i] : nullptr;
            g.sky_tex[i] = t && !t->rgba.empty() ? make_texture(g.device, t->width, t->height, t->rgba.data()) : g.white;
        }
    }
    SceneLighting lighting = opt.lighting;
    SkyFrame sky_frame;
    int state = 0, lod = 0;
    auto set_title = [&] {
        std::string t = opt.title + "  (" + std::to_string(mesh.draws.size()) + " draws)";
        if (opt.reload) t += "  state " + std::to_string(state) + " lod " + std::to_string(lod);
        if (opt.cycle) t += "  " + opt.cycle(0);
        SDL_SetWindowTitle(window, t.c_str());
    };
    set_title();

    Vec3 centre = (mesh.bounds_min + mesh.bounds_max) * 0.5f;
    const Vec3 ext = mesh.bounds_max - mesh.bounds_min;
    float radius = std::max(1.f, std::sqrt(dot(ext, ext)) * 0.5f);
    float yaw = 0.8f, pitch = opt.pitch, dist = opt.orbit_distance > 0 ? opt.orbit_distance : radius * opt.distance;
    bool dragging = false, running = true, paused = false;
    double anim_time = opt.start_time;
    Uint64 last_tick = SDL_GetTicksNS();
    if (opt.animate) opt.animate(anim_time, mesh.transforms);
    if (opt.follow) opt.follow(centre);

    auto ensure_depth = [&](Uint32 w, Uint32 h) {
        if (w == g.depth_w && h == g.depth_h) return;
        if (g.depth) SDL_ReleaseGPUTexture(g.device, g.depth);
        SDL_GPUTextureCreateInfo di{};
        di.type = SDL_GPU_TEXTURETYPE_2D;
        di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        di.width = w, di.height = h, di.layer_count_or_depth = 1, di.num_levels = 1;
        g.depth = SDL_CreateGPUTexture(g.device, &di);
        g.depth_w = w, g.depth_h = h;
    };
    struct DrawUniforms {
        Mat4 mvp, model;
        float sun_dir[4], sun_col[4], fill_dir[4], fill_col[4], ambient[4], eye[4], fog[4], uv0x[4], uv1x[4];
    };
    struct FragUniforms {
        float params[4], params2[4], tint[4], fog_col[4];
    };
    auto vertex_uniforms = [](const Mat4& vp, const Mat4& model, const SceneLighting& L, Vec3 eye) {
        return DrawUniforms{vp * model,
                            model,
                            {L.sun_dir.x, L.sun_dir.y, L.sun_dir.z, 0},
                            {L.sun_color.x, L.sun_color.y, L.sun_color.z, 0},
                            {L.fill_dir.x, L.fill_dir.y, L.fill_dir.z, 0},
                            {L.fill_color.x, L.fill_color.y, L.fill_color.z, 0},
                            {L.ambient.x, L.ambient.y, L.ambient.z, 0},
                            {eye.x, eye.y, eye.z, 0},
                            {L.fog_start, L.fog_end, 0, 0},
                            {1, 1, 0, 0},
                            {1, 1, 0, 0}};
    };

    auto render = [&](SDL_GPUCommandBuffer* cb, SDL_GPUTexture* target, Uint32 w, Uint32 h) {
        ensure_depth(w, h);
        Vec3 eye = centre + Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)} * dist;
        Vec3 look = centre;
        const float aspect = float(w) / float(std::max<Uint32>(h, 1));
        float fovy = 0.8f, znear = std::max(0.05f, dist * 0.005f), zfar = radius * 20.f;
        Vec3 up{0, 0, 1};
        ViewerOptions::Camera cam;
        if (opt.camera && opt.camera(cam)) {
            eye = cam.eye, look = cam.eye + cam.forward, up = cam.up;
            fovy = 2 * std::atan(std::tan(cam.fov_x / 2) / aspect);
            znear = cam.znear, zfar = cam.zfar;
        }
        const Mat4 view = Mat4::look_at(eye, look, up);
        const Mat4 vp = Mat4::perspective(fovy, aspect, znear, zfar) * view;
        if (opt.sky) { // colours and lighting for this frame, then the dome's vertices to the GPU
            opt.sky(anim_time, normalize(look - eye), dome, sky_frame);
            lighting = sky_frame.lighting;
            upload(g.device, dome.vertices.data(), Uint32(dome.vertices.size() * sizeof(RenderVertex)), g.sky_vbuf, nullptr);
        }
        const SceneLighting& L = lighting;
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.clear_color = {L.clear_color.x, L.clear_color.y, L.clear_color.z, 1};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = g.depth;
        depth.clear_depth = 1;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &color, 1, &depth);

        if (opt.sky) {
            // RenderSkyDome (10048110): the clear colour is the fog colour (background quad), then the
            // nebula with stars, the alpha-blended colour gradient and the scrolling clouds 5000 lower.
            // The sky passes are unfogged and unlit; the clouds are lit and fogged from 5000 to R/3.
            const Mat4 sky_vp = Mat4::perspective(fovy, aspect, 10.f, 100000.f) * view;
            SceneLighting unlit;
            unlit.sun_color = unlit.fill_color = {};
            unlit.ambient = {1, 1, 1};
            unlit.fog_end = 0;
            const double ms = sky_frame.ms;
            const float nebula = float(std::fmod(ms, 300000.0) * 3.333e-6), stars = float(std::fmod(ms, 100000.0) * 1e-5),
                        clouds = float(std::fmod(ms, 50000.0) * 2e-5);
            SDL_GPUBufferBinding svb{g.sky_vbuf, 0}, sib{g.sky_ibuf, 0};
            auto sky_pass = [&](SDL_GPUGraphicsPipeline* pipe, const DrawUniforms& vu, const FragUniforms& fu,
                                SDL_GPUTexture* t0, SDL_GPUTexture* t1) {
                SDL_BindGPUGraphicsPipeline(rp, pipe);
                SDL_BindGPUVertexBuffers(rp, 0, &svb, 1);
                SDL_BindGPUIndexBuffer(rp, &sib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_PushGPUVertexUniformData(cb, 0, &vu, sizeof vu);
                SDL_PushGPUFragmentUniformData(cb, 0, &fu, sizeof fu);
                SDL_GPUTextureSamplerBinding tb[2] = {{t0, g.sampler}, {t1, g.sampler}};
                SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
                SDL_DrawGPUIndexedPrimitives(rp, Uint32(dome.indices.size()), 1, 0, 0, 0);
            };
            DrawUniforms vu = vertex_uniforms(sky_vp, Mat4::translation(eye), unlit, eye);
            vu.uv0x[2] = vu.uv0x[3] = nebula;
            vu.uv1x[2] = vu.uv1x[3] = stars;
            sky_pass(g.pipeline_sky, vu, FragUniforms{{0, 2, 1, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 1}},
                     g.sky_tex[0], g.sky_tex[1]);
            vu.uv0x[2] = vu.uv0x[3] = vu.uv1x[2] = vu.uv1x[3] = 0;
            sky_pass(g.pipeline_sky_blend, vu, FragUniforms{{0, 0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 1}},
                     g.white, g.white);
            SceneLighting cloud_light = L;
            cloud_light.fog_start = SkyDome::kCloudDrop;
            cloud_light.fog_end = dome.radius / 3;
            DrawUniforms cu = vertex_uniforms(sky_vp, Mat4::translation(eye - Vec3{0, 0, SkyDome::kCloudDrop}), cloud_light, eye);
            cu.uv0x[0] = 3;
            cu.uv0x[2] = clouds;
            const Vec3 cc = sky_frame.cloud_color;
            sky_pass(g.pipeline_sky_blend, cu,
                     FragUniforms{{0, 0, 1, 0}, {0, 0, 0, 0}, {cc.x, cc.y, cc.z, 1}, {L.fog_color.x, L.fog_color.y, L.fog_color.z, 1}},
                     g.sky_tex[2], g.white);
        }

        SDL_GPUBufferBinding vbind{g.vbuf, 0};
        SDL_GPUBufferBinding ibind{g.ibuf, 0};
        for (int pass = 0; pass < 2; ++pass) { // opaque, then vertex-alpha draws
            SDL_BindGPUGraphicsPipeline(rp, pass == 0 ? g.pipeline : g.pipeline_blend);
            SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
            SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            for (const auto& d : mesh.draws) {
                if (((d.flags & kDrawVertexAlpha) != 0) != (pass == 1)) continue;
                const DrawUniforms vu = vertex_uniforms(vp, mesh.transforms[d.transform], L, eye);
                SDL_PushGPUVertexUniformData(cb, 0, &vu, sizeof vu);
                const FragUniforms fu{{d.flags & kDrawAlphaTest ? 1.f : 0.f, d.flags & kDrawDetail ? 1.f : 0.f,
                                       d.tex0 >= 0 ? 1.f : 0.f, d.flags & kDrawVertexAlpha ? 1.f : 0.f},
                                      {0, 0, 0, 0},
                                      {1, 1, 1, 1},
                                      {L.fog_color.x, L.fog_color.y, L.fog_color.z, 1}};
                SDL_PushGPUFragmentUniformData(cb, 0, &fu, sizeof fu);
                auto tex = [&](std::int32_t i) { return i >= 0 ? g.textures[std::size_t(i)] : g.white; };
                SDL_GPUTextureSamplerBinding tb[2] = {{tex(d.tex0), g.sampler}, {tex(d.tex1), g.sampler}};
                SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
                SDL_DrawGPUIndexedPrimitives(rp, d.index_count, 1, d.index_start, d.base_vertex, 0);
            }
        }
        SDL_EndGPURenderPass(rp);
    };

    if (!opt.shot_path.empty()) {
        // One frame into an offscreen target, read back and saved as BMP.
        const Uint32 w = 1024, h = 768;
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = color_format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ci.width = w, ci.height = h, ci.layer_count_or_depth = 1, ci.num_levels = 1;
        SDL_GPUTexture* off = SDL_CreateGPUTexture(g.device, &ci);
        SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g.device, &ti);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(g.device);
        render(cb, off, w, h);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureRegion src{};
        src.texture = off;
        src.w = w, src.h = h, src.d = 1;
        SDL_GPUTextureTransferInfo dst{tb, 0, w, h};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        SDL_WaitForGPUFences(g.device, true, &fence, 1);
        SDL_ReleaseGPUFence(g.device, fence);
        void* px = SDL_MapGPUTransferBuffer(g.device, tb, false);
        const bool bgra = color_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        SDL_Surface* surf =
            SDL_CreateSurfaceFrom(int(w), int(h), bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888, px, int(w * 4));
        const bool ok = surf && SDL_SaveBMP(surf, opt.shot_path.c_str());
        SDL_DestroySurface(surf);
        SDL_UnmapGPUTransferBuffer(g.device, tb);
        SDL_ReleaseGPUTransferBuffer(g.device, tb);
        SDL_ReleaseGPUTexture(g.device, off);
        SDL_Log("screenshot %s: %s", opt.shot_path.c_str(), ok ? "saved" : SDL_GetError());
        running = false;
    }

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (opt.on_event) opt.on_event(ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: dragging = true; break;
            case SDL_EVENT_MOUSE_BUTTON_UP: dragging = false; break;
            case SDL_EVENT_MOUSE_MOTION:
                if (dragging) {
                    yaw -= ev.motion.xrel * 0.01f;
                    pitch = std::clamp(pitch + ev.motion.yrel * 0.01f, -1.5f, 1.5f);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: dist = std::max(radius * 0.05f, dist * (ev.wheel.y > 0 ? 0.9f : 1.1f)); break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) running = false;
                if (ev.key.key == SDLK_SPACE) paused = !paused;
                if (opt.cycle && (ev.key.key == SDLK_LEFTBRACKET || ev.key.key == SDLK_RIGHTBRACKET)) {
                    opt.cycle(ev.key.key == SDLK_RIGHTBRACKET ? 1 : -1);
                    anim_time = 0;
                    set_title();
                }
                if (opt.reload && (ev.key.key == SDLK_L || ev.key.key == SDLK_S)) {
                    const int ns = ev.key.key == SDLK_S ? (state + 1) % 3 : state;
                    const int nl = ev.key.key == SDLK_L ? (lod + 1) % 5 : lod;
                    try {
                        RenderMesh m = opt.reload(ns, nl);
                        release_mesh(g);
                        mesh = std::move(m);
                        upload_mesh(g, mesh);
                        state = ns, lod = nl;
                    } catch (const std::exception& ex) {
                        SDL_Log("state %d lod %d: %s", ns, nl, ex.what());
                    }
                    set_title();
                }
                break;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        if (!paused) anim_time += double(now - last_tick) * 1e-9;
        last_tick = now;
        if (opt.animate) opt.animate(anim_time, mesh.transforms);
        if (opt.follow) opt.follow(centre);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(g.device);
        SDL_GPUTexture* swap = nullptr;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, window, &swap, &w, &h) || !swap) {
            SDL_SubmitGPUCommandBuffer(cb);
            continue;
        }
        render(cb, swap, w, h);
        SDL_SubmitGPUCommandBuffer(cb);
    }

    release_mesh(g);
    if (g.depth) SDL_ReleaseGPUTexture(g.device, g.depth);
    SDL_ReleaseGPUTexture(g.device, g.white);
    SDL_ReleaseGPUSampler(g.device, g.sampler);
    SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline);
    SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline_blend);
    SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline_sky);
    SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline_sky_blend);
    if (g.sky_vbuf) SDL_ReleaseGPUBuffer(g.device, g.sky_vbuf);
    if (g.sky_ibuf) SDL_ReleaseGPUBuffer(g.device, g.sky_ibuf);
    for (auto* t : g.sky_tex)
        if (t && t != g.white) SDL_ReleaseGPUTexture(g.device, t);
    SDL_ReleaseWindowFromGPUDevice(g.device, window);
    SDL_DestroyGPUDevice(g.device);
    return 0;
}

} // namespace parkan
