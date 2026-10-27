#include "app/fx_viewer.hpp"

#include "app/fx_draw.hpp"

#include "fx/effect.hpp"
#include "fx/spawn_frames.hpp"
#include "res/library.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace parkan {

namespace {

using namespace fx;

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

// Ground: a 60 m checkerboard in the XY plane (the world is Z-up), drawn as opaque debris triangles.
void add_ground(FxFrame& f) {
    for (int i = -6; i < 6; ++i)
        for (int j = -6; j < 6; ++j) {
            const float g = ((i + j) & 1) ? 0.26f : 0.31f;
            const float x0 = float(i) * 5, y0 = float(j) * 5, x1 = x0 + 5, y1 = y0 + 5;
            for (const auto& tri : {std::array<Vec3, 3>{{{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}}},
                                    std::array<Vec3, 3>{{{x0, y0, 0}, {x1, y1, 0}, {x0, y1, 0}}}}) {
                FxTriangle t;
                t.p[0] = tri[0], t.p[1] = tri[1], t.p[2] = tri[2];
                t.normal = {0, 0, 1};
                t.rgba[0] = t.rgba[1] = g, t.rgba[2] = g * 1.05f;
                f.triangles.push_back(t);
            }
        }
}

} // namespace

int run_effect_viewer(SDL_Window* window, AssetLibrary& lib, const EffectViewerOptions& opt) {
    auto fx = load_effect(lib, opt.effect);
    if (!fx) throw std::runtime_error("effect not found in effects.rlb: " + opt.effect);
    FxContext ctx(lib);
    const Mat4 placement = Mat4::identity();
    // Effects are authored in their spawner's CTPT frame (tracers ×0.03, gun flashes ×0.3): use the
    // largest one found for this effect.
    std::string key = opt.effect;
    for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto frames = collect_spawn_frames(lib);
    const auto frame_it = frames.find(key);
    const Mat4 spawn_frame = frame_it != frames.end() ? frame_it->second : Mat4::identity();
    if (frame_it != frames.end())
        SDL_Log("spawn frame: X (%.3f %.3f %.3f) Y (%.3f %.3f %.3f) Z (%.3f %.3f %.3f)", spawn_frame.at(0, 0),
                spawn_frame.at(1, 0), spawn_frame.at(2, 0), spawn_frame.at(0, 1), spawn_frame.at(1, 1), spawn_frame.at(2, 1),
                spawn_frame.at(0, 2), spawn_frame.at(1, 2), spawn_frame.at(2, 2));
    auto make = [&](std::optional<EffectInstance>& slot, FxContext& c) {
        slot.emplace(fx, c, placement, 0.0);
        slot->set_spawn_frame(spawn_frame);
    };
    std::optional<EffectInstance> inst;
    make(inst, ctx);
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
        if (moving) { // a bullet's frame: model +Y (forward) along world +X
            Mat4 m = Mat4::translation({float(-10 + 20 * t), 0, 3});
            m.at(0, 0) = 0, m.at(1, 0) = -1, m.at(0, 1) = 1, m.at(1, 1) = 0;
            e.set_transform(m);
        }
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
        std::optional<EffectInstance> probe_slot;
        make(probe_slot, probe_ctx);
        EffectInstance& probe = *probe_slot;
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
    std::optional<FxRenderer> renderer(std::in_place, dev, color_format, SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
    SDL_GPUTexture* depth = nullptr;
    Uint32 depth_w = 0, depth_h = 0;
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
        add_ground(frame);
        if (inst) inst->render(frame, eye);
        renderer->prepare(frame, eye, ctx.materials.textures);
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
        renderer->draw(cb, rp, vp);
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
        SDL_Surface* surf = SDL_CreateSurfaceFrom(int(w), int(h), (SDL_getenv("PARKAN_SHOT_KEEP_ALPHA") ? (bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888) : (bgra ? SDL_PIXELFORMAT_XRGB8888 : SDL_PIXELFORMAT_XBGR8888)), px, int(w * 4));
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
                make(inst, ctx);
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

    renderer.reset();
    if (depth) SDL_ReleaseGPUTexture(dev, depth);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    return rc;
}

} // namespace parkan
