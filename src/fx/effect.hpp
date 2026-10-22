// Effect runtime: one FXID template instantiated at a transform, updated with the game clock and
// turned into renderer-ready primitives. Port of Effect.dll's CFxManager instance (0x14c bytes) and
// its command objects; spec notes/effects.md.
//
//   FxContext ctx(lib);                        // materials, random streams, gravity
//   auto fx = load_effect(lib, "exp_rbr_b");   // LoadTemplate (shared, cached by the caller)
//   EffectInstance e(fx, ctx, world, now);     // CreateEffect + SetEffectMatrix
//   e.update(now, &sounds);                    // SendMsg(0x1c, ms): false once a one-shot ends
//   e.render(frame, eye);                      // Render: quads/triangles/lights in world space
//
// Times are seconds; the original works in milliseconds of the manager clock.
#pragma once

#include "fx/fx_material.hpp"
#include "fx/fx_random.hpp"
#include "res/fx.hpp"

#include <memory>
#include <optional>

namespace parkan {
class AssetLibrary;
}

namespace parkan::fx {

// A textured, coloured quad in world space. Corners go (−x −y), (−x +y), (+x +y), (+x −y) in the
// sprite's own frame; uv are absolute texture coordinates (the material frame is applied).
struct FxQuad {
    Vec3 p[4];
    float uv[4][2]{};
    float rgba[4] = {1, 1, 1, 1}; // material colour × the command's opacity in alpha
    std::int32_t texture = -1;    // FxMaterialLibrary::textures, -1 = untextured
    FxBlend blend = FxBlend::Alpha;
    bool on_top = false;          // visibility-tested glow whose ray is clear: drawn without depth test (inferred)
};

// Debris triangles (opcode 10), flat-shaded.
struct FxTriangle {
    Vec3 p[3];
    Vec3 normal;
    float uv[3][2]{};
    float rgba[4] = {1, 1, 1, 1};
    std::int32_t texture = -1;
    FxBlend blend = FxBlend::Opaque;
};

// Opcode 1 output: a dynamic light for this frame (ILightManager semantics, notes/effects.md §4.1).
struct FxLight {
    std::uint32_t kind = 0;        // template kind 1..7
    Vec3 pos, dir;
    float rgba[4]{};
    float range = 0;
    std::array<float, 3> attenuation{};
};

// Opcode 2 output: what the sound channel should do (UpdateFxSoundCommand 10012eb0).
struct FxSoundEvent {
    enum Kind : std::uint8_t { Play, Stop, Move } kind = Play;
    const FxSound* sound = nullptr;
    std::size_t command = 0; // index in the effect's command list (channel identity)
    Vec3 pos;                // world
    float pitch = 1;
};

struct FxFrame {
    std::vector<FxQuad> quads;
    std::vector<FxTriangle> triangles;
    std::vector<FxLight> lights;
    void clear() { quads.clear(), triangles.clear(), lights.clear(); }
};

// Shared state of the original's globals: materials (IMatManager), the random streams, the world's
// gravity (IWorld::GetGravity, 10 m/s²) and manager flag 1 (SendMsg 0x17 / 0x18).
struct FxContext {
    explicit FxContext(AssetLibrary& lib, std::uint32_t seed = 0x5eed1234u) : lib(lib), materials(lib), rng(seed) {}
    AssetLibrary& lib;
    FxMaterialLibrary materials;
    FxRandom rng;
    float gravity = 10.f;
    bool manager_flag = false;
};

// effects.rlb entry `name` (case-insensitive); nullptr when it does not exist. Throws FormatError.
std::shared_ptr<const FxEffect> load_effect(AssetLibrary& lib, std::string_view name);

class EffectInstance {
public:
    // CreateEffect: the instance starts at `now` (unless header flag 0x40) and sits at `world`.
    EffectInstance(std::shared_ptr<const FxEffect> fx, FxContext& ctx, const Mat4& world, double now);
    ~EffectInstance();
    EffectInstance(EffectInstance&&) noexcept;
    EffectInstance& operator=(EffectInstance&&) noexcept;

    // SetEffectMatrix / SetEffectPosition / SetEffectScale (scale multiplies the template's).
    void set_transform(const Mat4& m);
    void set_position(Vec3 p);
    void set_scale(Vec3 s);
    // The owner's (or its part's) world matrix: world = owner × local. Identity when unattached.
    void set_owner(const Mat4& owner);
    // StartEffect(handle, mode): restart now with time mode `mode` (default: the template's).
    void start(double now, std::optional<std::uint32_t> mode = std::nullopt);
    // StopEffect: let it reach its end, then restart in the template's mode (CFxInstance_Update).
    void stop(double now);
    void set_enabled(bool on, double now); // EnableEffect / DisableEffect
    void set_param(float v) { param_ = v; } // SetEffectParam: t for mode 0, the owner quantity for 4..17
    void set_duration_factor(float f);      // SetEffectDuration

    // One manager tick. Returns false when the instance is finished (one-shot with t >= 1).
    bool update(double now, std::vector<FxSoundEvent>* sounds = nullptr);
    // Appends this frame's primitives. `lod` is CShade's distance class (0 near … 7 far; ≥ 8 skips).
    void render(FxFrame& out, Vec3 eye, unsigned lod = 0);

    float t() const { return t_; }           // effect time after the time mode, before jitter
    float age() const { return age_; }       // seconds since start
    const Mat4& world() const { return world_; }
    const FxEffect& effect() const { return *fx_; }
    bool enabled() const { return enabled_; }

    struct Command; // per-opcode runtime (effect.cpp)

private:
    float eval_time(float now_ms);
    void compose_local();

    std::shared_ptr<const FxEffect> fx_;
    FxContext* ctx_;
    std::vector<std::unique_ptr<Command>> cmds_;
    float start_ms_ = 0, end_ms_ = 0, param_ = 0, last_value_ = 0;
    std::uint32_t mode_ = 0;
    bool enabled_ = true, stop_pending_ = false, attached_ = false, drawable_ = false;
    Mat4 base_ = Mat4::identity(), owner_ = Mat4::identity(), local_ = Mat4::identity(), world_ = Mat4::identity();
    Vec3 pos_{}, scale_{1, 1, 1};
    float t_ = 0, age_ = 0;
};

} // namespace parkan::fx
