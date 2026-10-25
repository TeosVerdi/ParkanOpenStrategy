#include "fx/effect.hpp"

#include "res/library.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::fx {

namespace {

// ---------------------------------------------------------------------------------------------
// Math helpers. Original matrices are row-major float[16] with the translation in [3], [7], [11];
// element i is at(i / 4, i % 4) here.

Vec3 col(const Mat4& m, int c) { return {m.at(0, c), m.at(1, c), m.at(2, c)}; }
void set_col(Mat4& m, int c, Vec3 v) { m.at(0, c) = v.x, m.at(1, c) = v.y, m.at(2, c) = v.z; }
Vec3 rotate(const Mat4& m, Vec3 v) {
    return col(m, 0) * v.x + col(m, 1) * v.y + col(m, 2) * v.z;
}
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

Mat4 inverse_affine(const Mat4& m) {
    const Vec3 a = col(m, 0), b = col(m, 1), c = col(m, 2);
    const Vec3 r0 = cross(b, c), r1 = cross(c, a), r2 = cross(a, b);
    const float det = dot(a, r0);
    Mat4 inv = Mat4::identity();
    if (det == 0) return inv;
    const float k = 1 / det;
    inv.at(0, 0) = r0.x * k, inv.at(0, 1) = r0.y * k, inv.at(0, 2) = r0.z * k;
    inv.at(1, 0) = r1.x * k, inv.at(1, 1) = r1.y * k, inv.at(1, 2) = r1.z * k;
    inv.at(2, 0) = r2.x * k, inv.at(2, 1) = r2.y * k, inv.at(2, 2) = r2.z * k;
    const Vec3 t = col(m, 3);
    set_col(inv, 3, Vec3{} - rotate(inv, t));
    return inv;
}

// pow() calls are skipped for an exponent of exactly 1.0 (the `== 0x3f800000` tests).
float curve(float x, float e) { return e == 1.f ? x : std::pow(x, e); }
Vec3 curve3(float x, const std::array<float, 3>& e) { return {curve(x, e[0]), curve(x, e[1]), curve(x, e[2])}; }
float frac(float x) { return x - std::floor(x); }
float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// FxSettings_RandomLodThreshold (1000ec50) with the default detail level (FxSettings_SetDetailLevel
// 1000e7c0, level 1): table key >> 8 → [0,3), [0,3), [2,5.5), [3,8.5). A particle is drawn while
// the CShade distance class is <= its threshold.
float lod_threshold(Lfsr16& rng, std::uint32_t key) {
    static constexpr float range[4] = {3.f, 3.f, 3.5f, 5.5f}, offset[4] = {0.f, 0.f, -2.f, -3.f};
    const std::size_t k = std::min<std::size_t>((key >> 8) & 0xff, 3);
    const float scale = range[k] * 1.5258789e-05f;
    return float(double(rng.next()) * double(scale) - double(offset[k]));
}

// Fallback axes (FUN_100038d0 / FUN_1000d110; constants 10023880/10023890 = X/Y axes).
Vec3 perpendicular(Vec3 n) {
    if (n.x == 0) return {1, 0, 0};
    if (n.y == 0) return {0, 1, 0};
    return normalize(Vec3{-n.y, n.x, 0});
}

// ---------------------------------------------------------------------------------------------
// The billboard element (FxBillboard_* 100092d0/100093d0/10009930, 0xcc bytes): position, axis and
// size each lerp between a start and an end by a (vector) parameter; Orient builds the quad frame.

struct Billboard {
    int mode = 0; // 0 camera-facing, 1 around `axis`, 2 plane ⟂ `axis`, 3 along the motion
    Vec3 p0, dp, a0, da, s0, ds;
    Vec3 pos, axis, size;
    Vec3 prev_world;

    void set_pos(Vec3 u) { pos = p0 + mul(dp, u); }   // slot 3 (1000d390)
    void set_axis(float u) { axis = a0 + da * u; }    // slot 4 (1000d510)
    void set_size(Vec3 u) { size = s0 + mul(ds, u); } // slot 7 (1000d450)
    void set_size(float u) { size = s0 + ds * u; }    // slot 6 (1000d580)

    // FxBillboard_Orient (100093d0): frame of the quad in the space of `m`, `eye` given in that space.
    bool quad(Vec3 eye, const Mat4& m, FxQuad& q) {
        Vec3 x, y;
        const Vec3 c = pos;
        switch (mode) {
        case 0: {
            const Vec3 d = eye - c;
            if (dot(d, d) == 0) return false;
            const Vec3 side = d.x == 0 ? Vec3{1, 0, 0} : d.y == 0 ? Vec3{0, 1, 0} : normalize(Vec3{-d.y, d.x, 0});
            x = side, y = normalize(cross(d, side));
            break;
        }
        case 1:
        case 3: {
            Vec3 a = axis;
            if (mode == 3) { // motion since the last frame, in the space of m
                const Vec3 cw = m.transform_point(c);
                a = c - inverse_affine(m).transform_point(prev_world);
                prev_world = cw;
            }
            if (dot(a, a) == 0) return false;
            a = normalize(a);
            const Vec3 side = cross(eye - c, a);
            x = a, y = dot(side, side) > 0 ? normalize(side) : perpendicular(a);
            break;
        }
        default: {
            const Vec3 n = normalize(axis);
            if (dot(n, n) == 0) return false;
            x = perpendicular(n), y = normalize(cross(n, x));
            break;
        }
        }
        // Unit quad ±0.5 in the frame's XY plane, columns scaled by the size [inferred from the beam
        // segments, which tile exactly with half-extent 0.5].
        const Vec3 hx = x * (0.5f * size.x), hy = y * (0.5f * size.y);
        q.p[0] = m.transform_point(c - hx - hy);
        q.p[1] = m.transform_point(c - hx + hy);
        q.p[2] = m.transform_point(c + hx + hy);
        q.p[3] = m.transform_point(c + hx - hy);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// Debris shapes: static tables built by Effect's initialisers (10009c30…1000b5d2), faces at
// 1001e8c0…1001ea40.

struct Shape {
    std::vector<Vec3> v;
    std::vector<std::array<std::uint16_t, 3>> f;
};

const Shape& shape(int i) {
    static const Shape shapes[6] = {
        {{{0, 0, 0.6f}, {0.5f, 0, -0.3f}, {-0.4f, 0.3f, -0.3f}, {-0.4f, -0.3f, -0.3f}},
         {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}},
        {{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}},
         {{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2}, {1, 4, 2}, {1, 2, 5}, {1, 5, 3}, {1, 3, 4}}},
        {{{1, 1, 1}, {-1, 1, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, -1}, {1, -1, -1}},
         {{0, 1, 2}, {0, 2, 3}, {0, 4, 5}, {0, 5, 1}, {0, 3, 7}, {0, 7, 4}, {6, 2, 1}, {6, 1, 5}, {6, 7, 3}, {6, 3, 2},
          {6, 5, 4}, {6, 4, 7}}},
        {{{0, 0.89f, 0.44f}, {0, 0, 1}, {0.85f, 0.27f, 0.44f}, {0.52f, 0.72f, -0.44f}, {-0.52f, 0.72f, -0.44f},
          {-0.85f, 0.27f, 0.44f}, {-0.52f, -0.72f, 0.44f}, {0.52f, -0.72f, 0.44f}, {0.85f, -0.27f, -0.44f},
          {0, -0.89f, -0.44f}, {0, 0, -1}, {-0.85f, -0.27f, -0.44f}},
         {{1, 2, 0}, {2, 3, 0}, {3, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 6, 1}, {6, 7, 1}, {7, 2, 1}, {2, 8, 3}, {2, 7, 8},
          {6, 9, 7}, {9, 8, 7}, {8, 10, 3}, {8, 9, 10}, {6, 11, 9}, {11, 10, 9}, {10, 4, 3}, {10, 11, 4}, {6, 5, 11},
          {5, 4, 11}}},
        {{{-0.1f, -0.2f, 3}, {0.2f, 0, 3}, {-0.1f, 0.2f, 3}, {0.2f, 0, 1}, {-0.1f, 0.2f, 1}, {-0.1f, -0.2f, 1},
          {0.2f, 0, -1}, {-0.1f, 0.2f, -1}, {-0.1f, -0.2f, -1}, {0.2f, 0, -3}, {-0.1f, 0.2f, -3}, {-0.1f, -0.2f, -3}},
         {{1, 3, 2}, {2, 3, 4}, {2, 4, 0}, {0, 4, 5}, {0, 5, 1}, {1, 5, 3}, {3, 6, 4}, {4, 6, 7}, {4, 7, 5}, {5, 7, 8},
          {5, 8, 3}, {3, 8, 6}, {6, 9, 7}, {7, 9, 10}, {7, 10, 8}, {8, 11, 6}, {6, 11, 9}, {8, 10, 11}, {0, 1, 2},
          {9, 11, 10}}},
        {{{0, 0, 0.1f}, {0, 0, -0.1f}, {1, -1, 0.1f}, {1, -1, -0.1f}, {0, -1, 0.1f}, {0, -1, -0.1f}, {-1, -1, 0.1f},
          {-1, -1, -0.1f}, {-1, 0, 0.1f}, {-1, 0, -0.1f}, {-1, 1, 0.1f}, {-1, 1, -0.1f}, {0, 1, 0.1f}, {0, 1, -0.1f},
          {1, 1, 0.1f}, {1, 1, -0.1f}, {1, 0, 0.1f}, {1, 0, -0.1f}},
         {{0, 4, 2}, {1, 3, 5}, {0, 2, 16}, {1, 5, 7}, {0, 16, 14}, {1, 7, 9}, {0, 14, 12}, {1, 9, 11}, {0, 12, 10},
          {1, 11, 13}, {0, 10, 8}, {1, 13, 15}, {0, 8, 6}, {1, 15, 17}, {0, 6, 4}, {1, 17, 3}, {3, 17, 16}, {16, 2, 3},
          {17, 15, 14}, {14, 16, 17}, {15, 13, 12}, {12, 14, 15}, {13, 11, 10}, {10, 12, 13}, {11, 9, 8}, {8, 10, 11},
          {9, 7, 6}, {6, 8, 9}, {7, 5, 4}, {4, 6, 7}, {5, 3, 2}, {2, 4, 5}}},
    };
    return shapes[i];
}

struct DebrisPiece {
    Vec3 p0, dp, pos;
    Vec3 scale{1, 1, 1};
    std::vector<Vec3> verts; // jittered and scaled shape (FxDebrisPiece_BuildShape 1000c1a0)
    const Shape* shape = nullptr;
    bool spin = false;
    Mat4 step = Mat4::identity(), orient = Mat4::identity();
};

// FxDebrisPiece_BuildShape (1000c1a0): optional random spin step, then every `group` vertices a
// random offset of up to ±50% of the vertex's own coordinates, then × `scale`.
void build_shape(DebrisPiece& p, FxRandom& rng, int which, unsigned group, float scale, bool spin) {
    p.shape = &shape(which);
    p.spin = spin;
    if (spin) {
        const float z = rng.debris.unit() - 0.5f;
        const float y = rng.shared.centered(1.f);
        const float x = rng.shared.centered(1.f);
        const Vec3 axis = normalize(Vec3{x, y, z});
        const float range = 1.5707964f / scale; // DAT_100244d0 = π/2 (initialiser 1000ce90)
        const float a = float(double(rng.debris.next()) * double(range * 1.5258789e-05f) - double(range) * 0.5);
        const float s = std::sin(a * 0.5f), c = std::cos(a * 0.5f);
        p.step = Mat4::rotation(Quat{axis.x * s, axis.y * s, axis.z * s, c});
    }
    Vec3 off;
    off.z = rng.debris.unit() - 0.5f; // drawn, then overwritten by the first group (vertex 0)
    off.y = rng.debris.unit() - 0.5f;
    off.x = rng.shared.centered(1.f);
    for (std::size_t i = 0; i < p.shape->v.size(); ++i) {
        const Vec3 v = p.shape->v[i];
        if (i % group == 0) {
            off.z = float(double(rng.debris.next()) * double(v.z * 1.5258789e-05f)) - v.z * 0.5f;
            off.y = float(double(rng.debris.next()) * double(v.y * 1.5258789e-05f)) - v.y * 0.5f;
            off.x = float(double(rng.debris.next()) * double(v.x * 1.5258789e-05f)) - v.x * 0.5f;
        }
        p.verts.push_back((v + off) * scale);
    }
}

// FxDebrisPiece_PickShape (1000bf30): returns the piece's LOD threshold.
float pick_shape(DebrisPiece& p, FxRandom& rng, bool metal) {
    const float f = rng.debris.unit();
    if (!metal) {
        if (f > 0.125f) return build_shape(p, rng, 0, 1, 1.f, false), 0.f;
        if (f > 0.015625f) return build_shape(p, rng, 1, 1, 1.5f, true), 1.f;
        if (f > 0.001953125f) return build_shape(p, rng, 2, 1, 2.f, true), 2.f;
        return build_shape(p, rng, 3, 1, 3.f, true), 3.f;
    }
    if (f > 0.0625f) return build_shape(p, rng, 0, 1, 1.f, false), 1.f;
    if (f > 0.03125f) return build_shape(p, rng, 4, 3, 3.f, true), 3.f;
    return build_shape(p, rng, 5, 2, 3.f, true), 3.f;
}

// ---------------------------------------------------------------------------------------------

struct UpdateState {
    float age = 0; // seconds since start
    float t = 0;   // effect time (jittered for everything but sounds)
    Mat4 world;
    float duration = 0;
};

struct RenderState {
    const Mat4& world;
    Vec3 eye_local, eye_world;
    unsigned lod = 0;
    bool visible = true; // occlusion ray clear (no occluders in the port)
    FxContext& ctx;
    FxFrame& out;
};

// FxBillboard_Draw (10009930) → CShade_RenderEffect (Terrain 10018d40): the material phase at
// `phase`, opacity replaced by `alpha`; nothing is drawn at opacity 0.
void emit(RenderState& rs, FxQuad q, std::int32_t material, float phase, float alpha, bool tested) {
    if (alpha == 0) return;
    phase = clamp01(phase);
    FxMaterialSample s;
    if (material >= 0) {
        s = rs.ctx.materials.sample(material, phase);
        q.blend = rs.ctx.materials.material(material).blend;
    }
    for (int k = 0; k < 3; ++k) q.rgba[k] = std::min(1.f, s.rgb[k] + s.diffuse[k]); // unlit: emissive (+ diffuse)
    q.rgba[3] = alpha;
    q.texture = s.texture;
    const float u0 = s.uv0[0], v0 = s.uv0[1], u1 = s.uv1[0], v1 = s.uv1[1];
    const float uv[4][2] = {{u0, v1}, {u0, v0}, {u1, v0}, {u1, v1}};
    std::copy(&uv[0][0], &uv[0][0] + 8, &q.uv[0][0]);
    q.on_top = tested && rs.visible;
    rs.out.quads.push_back(q);
}

std::int32_t material_of(FxContext& ctx, const FxResourceRef& r) {
    return r.name.empty() ? -1 : ctx.materials.get(r.name);
}

} // namespace

struct EffectInstance::Command {
    virtual ~Command() = default;
    virtual void update(const UpdateState&, FxContext&, std::vector<FxSoundEvent>*) {}
    virtual void render(RenderState&) {}
    bool sound = false;
    bool tested = false;
    std::size_t index = 0;
};

namespace {

using Cmd = EffectInstance::Command;

// Opcode 1: FxLight_Init/Update (1000f4b0 / 1000f6e0).
struct LightCmd : Cmd {
    const FxLightCmd& c;
    bool on = false;
    FxLight light;
    explicit LightCmd(const FxLightCmd& c) : c(c) {}
    void update(const UpdateState& s, FxContext& ctx, std::vector<FxSoundEvent>*) override {
        on = c.t0 <= s.t && s.t <= c.t1;
        if (!on) return;
        const float u = c.t1 > c.t0 ? (s.t - c.t0) / (c.t1 - c.t0) : 0;
        light.kind = c.kind;
        light.pos = s.world.transform_point(c.pos0 + (c.pos1 - c.pos0) * u);
        light.dir = normalize(rotate(s.world, c.dir0 + (c.dir1 - c.dir0) * u));
        const float ja = ctx.rng.light.centered(c.color_jitter[3]);
        const float jb = ctx.rng.light.centered(c.color_jitter[2]);
        const float jg = ctx.rng.light.centered(c.color_jitter[1]);
        const float jr = ctx.rng.shared.centered(c.color_jitter[0]);
        const float j[4] = {jr, jg, jb, ja};
        for (int k = 0; k < 4; ++k) light.rgba[k] = c.color0[std::size_t(k)] + (c.color1[std::size_t(k)] - c.color0[std::size_t(k)]) * u + j[k];
        // Range: lerp plus [0, range_jitter) flicker [inferred: the call's argument is not recovered].
        light.range = c.range0 + (c.range1 - c.range0) * u + ctx.rng.light.unit() * c.range_jitter;
        if (light.range <= 0) light.range = 0.01f;
        light.attenuation = c.attenuation;
    }
    void render(RenderState& rs) override {
        if (on) rs.out.lights.push_back(light);
    }
};

// Opcode 2: UpdateFxSoundCommand (10012eb0), notes/audio.md §3.1.
struct SoundCmd : Cmd {
    const FxSound& c;
    float prev = 0;
    bool playing = false;
    explicit SoundCmd(const FxSound& c) : c(c) { sound = true; }
    void update(const UpdateState& s, FxContext&, std::vector<FxSoundEvent>* ev) override {
        const float t = s.t >= 1 ? 0.f : s.t; // t = 1 counts as 0: cyclic effects re-trigger
        const float u = c.t1 > c.t0 ? clamp01((s.t - c.t0) / (c.t1 - c.t0)) : 0;
        FxSoundEvent e;
        e.sound = &c, e.command = index;
        e.pos = s.world.transform_point(c.pos0 + (c.pos1 - c.pos0) * u);
        e.pitch = c.pitch0 + (c.pitch1 - c.pitch0) * u;
        auto push = [&](FxSoundEvent::Kind k) {
            if (!ev) return;
            e.kind = k;
            ev->push_back(e);
        };
        if (c.loop()) {
            const bool want = c.t0 <= s.t && s.t <= c.t1;
            if (want && !playing) push(FxSoundEvent::Play);
            else if (!want && playing) push(FxSoundEvent::Stop);
            else if (want) push(FxSoundEvent::Move);
            playing = want;
        } else if (prev <= c.t0 && c.t0 < t) {
            if (playing) push(FxSoundEvent::Stop);
            push(FxSoundEvent::Play);
            playing = true;
        }
        prev = t;
    }
};

// Opcodes 3, 4, 9: FxSprite_* (100103b0 / 100105f0 / 100106c0), FxDistSprite_* (100108c0…),
// FxFlare_Render (100138c0).
struct SpriteCmd : Cmd {
    const FxSpriteCmd& c;
    std::uint32_t op;
    Billboard bb;
    std::int32_t material = -1;
    float u = -1, frame = 0, f_start = 0, f_range = 0;
    SpriteCmd(const FxSpriteCmd& c, std::uint32_t op, FxContext& ctx) : c(c), op(op) {
        material = material_of(ctx, c.material);
        bb.mode = int(c.align);
        bb.p0 = bb.pos = c.pos0, bb.dp = c.pos1 - c.pos0;
        bb.a0 = bb.axis = c.axis0, bb.da = c.axis1 - c.axis0;
        bb.s0 = bb.size = c.size0, bb.ds = c.size1 - c.size0;
        f_start = std::max(0.f, c.frame0), f_range = c.frame1 - f_start;
    }
    void update(const UpdateState& s, FxContext&, std::vector<FxSoundEvent>*) override {
        if (!(c.t0 <= s.t && s.t <= c.t1)) {
            u = -1;
            return;
        }
        u = (s.t - c.t0) / (c.t1 - c.t0);
        if (op != 4 || c.frame0 < 0) frame = curve(c.frame0 < 0 ? s.age : u, c.frame_exp) * f_range + f_start;
    }
    void render(RenderState& rs) override {
        if (u < 0 || rs.lod >= 8) return;
        float uu = u;
        if (op == 4) { // the sprite grows with the camera distance (engine glows)
            uu = std::min(1.f, length(rs.eye_local) * (1 / c.distance_norm) * u);
            if (c.frame0 >= 0) frame = curve(uu, c.frame_exp) * f_range + f_start;
        }
        bb.set_pos(curve3(uu, c.pos_exp));
        bb.set_axis(uu);
        bb.set_size(curve3(uu, c.size_exp));
        FxQuad q;
        if (!bb.quad(rs.eye_local, rs.world, q)) return;
        // Opcode 9 asks CShade for draw mode 3/5/6, which repeats the sprite a LOD-dependent number of
        // times (tables at CShade +0x1730/+0x1760); drawn once here (deviation).
        emit(rs, q, material, frac(frame), c.alpha0 + (c.alpha1 - c.alpha0) * curve(uu, c.alpha_exp), tested);
    }
};

// Opcode 5: FxBeam_Init/Update/Render (100028a0 / 10002a20 / 10002be0).
struct BeamCmd : Cmd {
    const FxBeamCmd& c;
    std::int32_t material = -1;
    bool started = false, changed = false, active = true;
    Vec3 from, to;
    float u = 0, frame = 0, f_start = 0, f_range = 0;
    BeamCmd(const FxBeamCmd& c, FxContext& ctx) : c(c) {
        material = material_of(ctx, c.material);
        f_start = std::max(0.f, c.frame0), f_range = c.frame1 - f_start;
    }
    void update(const UpdateState& s, FxContext&, std::vector<FxSoundEvent>*) override {
        if (!(c.t0 <= s.t && s.t <= c.t1)) {
            active = false;
            return;
        }
        active = true;
        u = (s.t - c.t0) / (c.t1 - c.t0);
        frame = (c.frame0 < 0 ? s.age : u) * f_range + f_start;
        const Vec3 p = col(s.world, 3);
        if (!started) { // the first position the effect is seen at
            from = p, started = true;
            return;
        }
        if (p.x == to.x && p.y == to.y && p.z == to.z) return;
        to = p, changed = true;
    }
    void render(RenderState& rs) override {
        if (!started || !changed || !active) return;
        Vec3 d = to - from;
        const float len = length(d);
        if (len == 0) return;
        d = d * (1 / len);
        unsigned n = unsigned(std::floor(len / c.segment_length));
        n = std::clamp(n, 1u, c.max_segments);
        const float seg = len / float(n), step = seg / c.uv_length;
        const float width = c.width0 + (c.width1 - c.width0) * u;
        const float phase = c.frame1 >= 0 ? frac(frame) : rs.ctx.rng.beam.unit();
        const float alpha = c.alpha0 + (c.alpha1 - c.alpha0) * u;
        float u0 = 0;
        for (unsigned i = 0; i < n; ++i, u0 += step) {
            // Segment element: mode 1 around the beam axis, centred at (i + 0.5) segments.
            const Vec3 centre = from + d * (seg * (float(i) + 0.5f));
            Vec3 side = cross(rs.eye_world - centre, d);
            side = dot(side, side) > 0 ? normalize(side) : perpendicular(d);
            const Vec3 hx = d * (seg * 0.5f), hy = side * (width * 0.5f);
            FxQuad q;
            q.p[0] = centre - hx - hy, q.p[1] = centre - hx + hy, q.p[2] = centre + hx + hy, q.p[3] = centre + hx - hy;
            const std::size_t before = rs.out.quads.size();
            emit(rs, q, material, phase, alpha, tested);
            if (rs.out.quads.size() == before) continue;
            // FxBeamSegment_SetUV (10009b90): u runs frac(i·step) … + step along the beam, v 1 → 0.
            FxQuad& o = rs.out.quads.back();
            const float a = frac(u0), b = a + step;
            const float fu0 = o.uv[0][0], fu1 = o.uv[2][0];
            auto lerp_u = [&](float x) { return fu0 + (fu1 - fu0) * x; };
            o.uv[0][0] = o.uv[1][0] = lerp_u(a);
            o.uv[2][0] = o.uv[3][0] = lerp_u(b);
        }
    }
};

// Opcodes 7 and 10: FxParticleCmd_InitBase (10001000), FxSpriteBurst_Init (10001720),
// FxDebris_Init (10001a40), FxParticleCmd_Update/Render (10001230 / 10001300).
struct BurstCmd : Cmd {
    const FxBurstCmd& c;
    bool debris;
    std::int32_t material = -1;
    std::vector<float> start, fall_time, lod;
    std::vector<Billboard> parts;
    std::vector<DebrisPiece> pieces;
    float u = -1, prev_age = 0, dt = 0, fall_k = 0;

    BurstCmd(const FxBurstCmd& c, bool debris, FxContext& ctx, std::uint32_t settings_key) : c(c), debris(debris) {
        const std::uint32_t n = c.emissions, m = c.per_emission;
        for (std::uint32_t i = 0; i < n; ++i) {
            start.push_back((1.f - c.life) * float(i) / float(n));
            fall_time.push_back(0);
            for (std::uint32_t j = 0; j < m; ++j) lod.push_back(lod_threshold(ctx.rng.lod, settings_key));
        }
        fall_k = c.gravity * ctx.gravity * 0.5f;
        material = material_of(ctx, c.material);
        if (!debris) {
            for (std::uint32_t k = 0; k < n * m; ++k) {
                Billboard b;
                b.mode = c.align == 1 ? 3 : 0;
                Vec3 r;
                r.z = ctx.rng.shared.centered(c.pos_rand.z);
                r.y = ctx.rng.shared.centered(c.pos_rand.y);
                r.x = ctx.rng.shared.centered(c.pos_rand.x);
                b.p0 = b.pos = c.pos0, b.dp = c.pos1 + r - c.pos0;
                r.z = ctx.rng.shared.centered(c.size_rand.z);
                r.y = ctx.rng.shared.centered(c.size_rand.y);
                r.x = ctx.rng.shared.centered(c.size_rand.x);
                b.s0 = b.size = c.size0, b.ds = c.size1 + r - c.size0;
                parts.push_back(b);
            }
        } else {
            // No material: the original picks a random row of the owner's own wear (one draw).
            if (material < 0) ctx.rng.shared.next();
            for (std::uint32_t k = 0; k < n * m; ++k) {
                DebrisPiece p;
                lod[k] = pick_shape(p, ctx.rng, c.align == 0x20);
                Vec3 r;
                r.z = ctx.rng.shared.centered(c.pos_rand.z);
                r.y = ctx.rng.shared.centered(c.pos_rand.y);
                r.x = ctx.rng.shared.centered(c.pos_rand.x);
                p.p0 = p.pos = c.pos0, p.dp = c.pos1 + r - c.pos0;
                r.z = ctx.rng.shared.centered(c.size_rand.z);
                r.y = ctx.rng.shared.centered(c.size_rand.y);
                r.x = ctx.rng.shared.centered(c.size_rand.x);
                p.scale = c.size0 + r;
                pieces.push_back(std::move(p));
            }
        }
    }
    float age_of(std::size_t i) const { return (u - start[i]) / c.life; }
    bool live(float b) const { return b >= 0 && (debris || b <= 1); }

    void update(const UpdateState& s, FxContext&, std::vector<FxSoundEvent>*) override {
        if (!(c.t0 <= s.t && s.t <= c.t1)) {
            u = -1, prev_age = s.age;
            return;
        }
        u = (s.t - c.t0) / (c.t1 - c.t0);
        dt = s.age - prev_age;
        prev_age = s.age;
        // The fall clock of a live emission advances by the frame time; the original does this in
        // the render pass (one render per update is assumed here).
        if (fall_k != 0)
            for (std::size_t i = 0; i < start.size(); ++i)
                if (live(age_of(i))) fall_time[i] += dt;
    }

    void render(RenderState& rs) override {
        if (u < 0) return;
        const std::uint32_t m = c.per_emission;
        for (std::size_t i = 0; i < start.size(); ++i) {
            const float b = age_of(i);
            if (!live(b)) continue;
            const float alpha = c.alpha0 + (c.alpha1 - c.alpha0) * curve(b, c.alpha_exp);
            const Vec3 pu = curve3(b, c.pos_exp), su = curve3(b, c.size_exp);
            Mat4 mw = rs.world;
            if (fall_k != 0) mw.at(2, 3) -= fall_time[i] * fall_time[i] * fall_k; // world z
            const Vec3 eye = inverse_affine(mw).transform_point(rs.eye_world);
            for (std::uint32_t j = 0; j < m; ++j) {
                const std::size_t k = i * m + j;
                if (float(rs.lod) > lod[k]) continue;
                float ph = u - start[i];
                if (c.frame_rate > 1) ph *= c.frame_rate, ph -= std::floor(ph);
                else ph /= c.frame_rate;
                if (ph >= 1) ph = 0.99f;
                if (!debris) {
                    Billboard& bb = parts[k];
                    bb.set_pos(pu);
                    bb.set_size(su);
                    FxQuad q;
                    if (bb.quad(eye, mw, q)) emit(rs, q, material, ph, alpha, tested);
                } else {
                    draw_piece(rs, pieces[k], pu, mw);
                }
            }
        }
    }

    // FxDebrisPiece_Orient/Draw (1000bc10 / 1000bd10): the spin step is applied once per draw.
    void draw_piece(RenderState& rs, DebrisPiece& p, Vec3 pu, const Mat4& mw) {
        p.pos = p.p0 + mul(p.dp, pu);
        if (p.spin) p.orient = p.step * p.orient;
        Mat4 r = mw * p.orient;
        set_col(r, 3, mw.transform_point(p.pos));
        FxMaterialSample s;
        FxBlend blend = FxBlend::Opaque;
        if (material >= 0) {
            s = rs.ctx.materials.sample(material, 0);
            blend = rs.ctx.materials.material(material).blend;
        } else {
            s.rgb[0] = s.rgb[1] = s.rgb[2] = 0.45f; // the owner's material in the original
        }
        for (const auto& f : p.shape->f) {
            FxTriangle t;
            for (int k = 0; k < 3; ++k) {
                const Vec3 v = p.verts[f[std::size_t(k)]];
                t.p[k] = r.transform_point(mul(v, p.scale));
                // UVs come from per-shape tables built at load (10009cb0…); approximated here by a
                // planar projection of the unjittered shape (deviation).
                const Vec3 sv = p.shape->v[f[std::size_t(k)]];
                t.uv[k][0] = s.uv0[0] + (s.uv1[0] - s.uv0[0]) * clamp01(0.5f + 0.35f * (sv.x + 0.5f * sv.z));
                t.uv[k][1] = s.uv0[1] + (s.uv1[1] - s.uv0[1]) * clamp01(0.5f + 0.35f * (sv.y + 0.5f * sv.z));
            }
            t.normal = normalize(cross(t.p[1] - t.p[0], t.p[2] - t.p[0]));
            for (int k = 0; k < 3; ++k) t.rgba[k] = std::min(1.f, s.rgb[k] + s.diffuse[k]);
            t.rgba[3] = s.opacity;
            t.texture = s.texture;
            t.blend = blend;
            rs.out.triangles.push_back(t);
        }
    }
};

// Opcode 8: FxTrail_Init/Update/Render (10011230 / 100115c0 / 10012030).
struct TrailCmd : Cmd {
    const FxTrailCmd& c;
    std::uint32_t key;
    std::int32_t material = -1;
    std::vector<Billboard> parts;
    std::vector<Mat4> frames;
    std::vector<float> lod, rnd;
    std::vector<std::uint8_t> alive;
    std::uint32_t head = 0, alive_count = 0;
    float last_time = 0, interval = 0, u = 0, age = 0;
    Vec3 last_pos, drift0, drift_delta;

    TrailCmd(const FxTrailCmd& c, FxContext& ctx, std::uint32_t key) : c(c), key(key) {
        material = material_of(ctx, c.material);
        const std::uint32_t n = c.count;
        parts.resize(n), frames.resize(n, Mat4::identity()), lod.resize(n), rnd.resize(n), alive.resize(n);
        interval = c.interval0;
        Vec3 r;
        r.z = ctx.rng.trail.centered(c.drift_rand.z);
        r.y = ctx.rng.shared.centered(c.drift_rand.y);
        r.x = ctx.rng.shared.centered(c.drift_rand.x);
        drift0 = c.drift0, drift_delta = c.drift1 + r - c.drift0;
    }

    void update(const UpdateState& s, FxContext& ctx, std::vector<FxSoundEvent>*) override {
        age = s.age;
        u = s.t;
        const bool in = c.t0 <= s.t && s.t <= c.t1;
        if (!alive_count && !in) {
            last_time = 0;
            return;
        }
        const Vec3 p = col(s.world, 3);
        if (last_time == 0) {
            last_pos = p, last_time = s.age;
            return;
        }
        if (s.age < last_time) last_time = s.age;
        u = clamp01((s.t - c.t0) / (c.t1 - c.t0));
        const float di = c.interval1 - c.interval0;
        interval = u * di + c.interval0;
        if (!(interval + last_time < s.age)) return;
        // Spawn direction: towards the current position plus the drift offset.
        const Vec3 target = p + drift0 + mul(drift_delta, curve3(u, c.drift_exp));
        Vec3 vel = (target - last_pos) * (1 / (s.age - last_time));
        if (dot(vel, vel) == 0) vel = col(s.world, 0);
        const Vec3 dir = normalize(vel);
        while (interval + last_time < s.age) {
            head = (head + 1) % c.count;
            interval = u * di + c.interval0;
            last_pos = last_pos + vel * interval;
            interval = s.t * di + c.interval0; // the original advances the clock by the raw-t interval
            last_time += interval;
            const bool was = alive[head];
            alive[head] = in;
            if (!was && in) ++alive_count;
            if (was && !in) --alive_count;
            if (!in) continue;
            lod[head] = lod_threshold(ctx.rng.lod, key);
            rnd[head] = ctx.rng.trail.unit();
            // Spawn frame: X along the motion, Y ⟂ to it and the effect's Z axis.
            Mat4 f = Mat4::identity();
            const Vec3 side = cross(col(s.world, 2), dir);
            if (dot(side, side) > 0) {
                const Vec3 y = normalize(side);
                set_col(f, 0, dir), set_col(f, 1, y), set_col(f, 2, cross(dir, y));
            } else {
                const Vec3 y = perpendicular(dir);
                set_col(f, 0, dir), set_col(f, 1, y), set_col(f, 2, normalize(cross(dir, y)));
            }
            set_col(f, 3, last_pos);
            frames[head] = f;
            Billboard& b = parts[head];
            b.mode = 0;
            Vec3 r;
            r.z = ctx.rng.trail.centered(c.pos_rand.z);
            r.y = ctx.rng.shared.centered(c.pos_rand.y);
            r.x = ctx.rng.shared.centered(c.pos_rand.x);
            b.p0 = b.pos = c.pos0, b.dp = c.pos1 + r - c.pos0;
            r.z = ctx.rng.shared.centered(c.size_rand.z);
            r.y = ctx.rng.shared.centered(c.size_rand.y);
            r.x = ctx.rng.shared.centered(c.size_rand.x);
            b.s0 = b.size = c.size0, b.ds = c.size1 + r - c.size0;
        }
    }

    void render(RenderState& rs) override {
        if (!alive_count && !(c.t0 <= u && u <= c.t1)) return;
        const float n = float(c.count);
        const float base = (age - last_time) / (interval * n);
        for (std::uint32_t i = 0; i < c.count; ++i) {
            if (!alive[i] || float(rs.lod) > lod[i]) continue;
            float a = (float(head) - float(i)) / n + base;
            if (a < 0) a += 1;
            const float phase = c.frame_exp >= 0 ? curve(a, c.frame_exp) : rnd[i];
            Billboard& b = parts[i];
            b.set_pos(curve3(a, c.pos_exp));
            b.set_size(curve3(a, c.size_exp));
            const Vec3 eye = inverse_affine(frames[i]).transform_point(rs.eye_world);
            FxQuad q;
            if (b.quad(eye, frames[i], q))
                emit(rs, q, material, phase, c.alpha0 + (c.alpha1 - c.alpha0) * curve(a, c.alpha_exp), tested);
        }
    }
};

} // namespace

Mat4 contact_frame(const std::array<Vec3, 3>& pos, const std::array<Vec3, 3>& vec) {
    auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }; // FUN_10003620
    const Vec3 origin = (pos[0] + pos[1] + pos[2]) * (1.f / 3);
    Mat4 m = Mat4::identity();
    set_col(m, 3, origin);
    // FUN_10004070: X = a, Y = unit(b × a), Z = a × Y.
    auto two = [&](Vec3 a, Vec3 b) {
        Vec3 side = cross(b, a);
        side = dot(side, side) > 0 ? normalize(side) : perpendicular(a);
        set_col(m, 0, a), set_col(m, 1, side), set_col(m, 2, cross(a, side));
    };
    const Vec3 a = vec[0], b = vec[1], c = vec[2];
    if (same(a, b) && same(a, c)) { // FUN_10003ef0 on the unit vector, then × its length
        const float len = length(a);
        if (len == 0) return m;
        const Vec3 d = a * (1 / len), t = perpendicular(d);
        set_col(m, 0, d * len), set_col(m, 1, t * len), set_col(m, 2, cross(d, t) * len);
    } else if (same(a, c)) {
        two(a, b);
    } else if (same(a, b)) {
        two(a, c);
    } else if (same(b, c)) {
        two(a, b);
    } else {
        set_col(m, 0, a), set_col(m, 1, b), set_col(m, 2, c);
    }
    return m;
}

std::shared_ptr<const FxEffect> load_effect(AssetLibrary& lib, std::string_view name) {
    const NResEntry* e = lib.find("effects.rlb", name);
    if (!e) return nullptr;
    return std::make_shared<const FxEffect>(decode_fx(e->data));
}

EffectInstance::~EffectInstance() = default;
EffectInstance::EffectInstance(EffectInstance&&) noexcept = default;
EffectInstance& EffectInstance::operator=(EffectInstance&&) noexcept = default;

EffectInstance::EffectInstance(std::shared_ptr<const FxEffect> fx, FxContext& ctx, const Mat4& world, double now)
    : fx_(std::move(fx)), ctx_(&ctx) {
    // CFxInstance_Construct (10007470) + CFxInstance_BuildCommands (10007650).
    const FxHeader& h = fx_->header;
    start_ms_ = float(now * 1000);
    end_ms_ = 1000.f * h.duration + start_ms_;
    mode_ = h.time_mode;
    scale_ = h.scale;
    enabled_ = !(h.flags & kFxStartDisabled);
    for (std::size_t i = 0; i < fx_->list.size(); ++i) {
        const FxCommand& c = fx_->list[i];
        std::unique_ptr<Command> cmd;
        switch (c.opcode) {
        case 1: cmd = std::make_unique<LightCmd>(std::get<FxLightCmd>(c.data)); break;
        case 2: cmd = std::make_unique<SoundCmd>(std::get<FxSound>(c.data)); break;
        case 3: case 4: case 9: cmd = std::make_unique<SpriteCmd>(std::get<FxSpriteCmd>(c.data), c.opcode, ctx); break;
        case 5: cmd = std::make_unique<BeamCmd>(std::get<FxBeamCmd>(c.data), ctx); break;
        case 7: case 10: cmd = std::make_unique<BurstCmd>(std::get<FxBurstCmd>(c.data), c.opcode == 10, ctx, h.settings_key); break;
        case 8: cmd = std::make_unique<TrailCmd>(std::get<FxTrailCmd>(c.data), ctx, h.settings_key); break;
        default: continue;
        }
        cmd->tested = c.tested;
        cmd->index = i;
        cmds_.push_back(std::move(cmd));
    }
    set_transform(world);
}

void EffectInstance::compose_local() { // CFxInstance_ComposeLocalMatrix (10007c90)
    local_ = base_;
    set_col(local_, 3, pos_);
    set_col(local_, 0, col(base_, 0) * scale_.x);
    set_col(local_, 1, col(base_, 1) * scale_.y);
    set_col(local_, 2, col(base_, 2) * scale_.z);
}

void EffectInstance::set_spawn_frame(const Mat4& frame) {
    frame_ = frame;
    set_transform(placed_);
}

void EffectInstance::set_transform(const Mat4& placed) {
    placed_ = placed;
    const Mat4 m = placed * frame_;
    base_ = m;
    pos_ = col(m, 3);
    compose_local();
    world_ = owner_ * local_;
}

void EffectInstance::set_position(Vec3 p) {
    pos_ = p;
    compose_local();
}

void EffectInstance::set_scale(Vec3 s) {
    scale_ = mul(s, fx_->header.scale);
    compose_local();
}

void EffectInstance::set_owner(const Mat4& owner) {
    owner_ = owner;
    attached_ = true;
}

void EffectInstance::start(double now, std::optional<std::uint32_t> mode) { // CFxInstance_Start (10007b00)
    start_ms_ = float(now * 1000);
    end_ms_ = 1000.f * fx_->header.duration + start_ms_;
    mode_ = mode.value_or(fx_->header.time_mode);
}

void EffectInstance::stop(double) { stop_pending_ = true; } // CFxInstance_Stop (10007b40)

void EffectInstance::set_enabled(bool on, double now) { // CFxInstance_SetEnabled (10007a30)
    enabled_ = on;
    age_ = float(now * 1000 - start_ms_) * 0.001f;
}

void EffectInstance::set_duration_factor(float f) { end_ms_ = 1000.f * fx_->header.duration * f + start_ms_; }

float EffectInstance::eval_time(float now) { // CFxManager_EvalTimeParam (10005c60)
    const std::uint32_t flags = fx_->header.flags;
    if ((flags & kFxNeedsMgrFlag && !ctx_->manager_flag) || (flags & kFxHideOnMgrFlag && ctx_->manager_flag)) return 0;
    // A zero-length effect is complete at once (the original divides 0/0 here; inferred).
    const float span = end_ms_ - start_ms_;
    const float lin = span != 0 ? (now - start_ms_) / span : 1.f;
    float v = 0;
    switch (mode_) {
    case 0: v = param_; break; // SetEffectParam
    case 1: v = lin; break;
    case 2: v = lin - std::floor(lin); break;
    case 3: v = 1 - lin; break;
    // 4..15 read the owner (IAniMesh, IControl speed ratios); the port takes that quantity from
    // set_param. 13 and 14 are 1 − x in the original.
    case 13: case 14: v = 1 - param_; break;
    case 16: // holds the last value unless the new one rises or is 0
        v = param_;
        if (v < last_value_ && v != 0) v = last_value_;
        break;
    case 17: // holds the last value unless the new one falls or is 1
        v = param_;
        if (v > last_value_ && v != 1) v = last_value_;
        break;
    default: v = param_; break;
    }
    if (flags & kFxScaleByLinear) v *= lin;
    if (flags & kFxTriangle) {
        if (v >= 0.5f) v = 1 - v;
        v += v;
    }
    last_value_ = v;
    return v;
}

bool EffectInstance::update(double now_s, std::vector<FxSoundEvent>* sounds) {
    if (!enabled_) return true;
    const float now = float(now_s * 1000);
    const FxHeader& h = fx_->header;
    const float v = eval_time(now);
    // CFxInstance_Update (10008120).
    if (stop_pending_ && end_ms_ <= now) {
        start_ms_ = now;
        end_ms_ = 1000.f * h.duration + now;
        mode_ = h.time_mode;
        stop_pending_ = false;
        stop_reached_ = true;
    }
    drawable_ = true; // detail category enabled (all are at the default level)
    const float clean = v == -1.f ? v : clamp01(v);
    float jit = v;
    if (v != -1.f) {
        if (h.flags & kFxJitterT) jit = ctx_->rng.instance.centered(h.t_jitter) + v;
        jit = clamp01(jit);
    }
    if (h.flags & kFxJitterPos) {
        Vec3 j;
        j.z = ctx_->rng.instance.centered(h.pos_jitter.z);
        j.y = ctx_->rng.shared.centered(h.pos_jitter.y);
        j.x = ctx_->rng.shared.centered(h.pos_jitter.x);
        set_col(local_, 3, col(local_, 3) + j);
    }
    world_ = owner_ * local_;
    age_ = (now - start_ms_) * 0.001f;
    t_ = v;
    UpdateState st{age_, jit, world_, h.duration};
    UpdateState snd{age_, clean, world_, h.duration};
    for (auto& c : cmds_) c->update(c->sound ? snd : st, *ctx_, sounds);
    // CFxManager_UpdateInstance (10006170).
    if (h.flags & kFxOneShot && v >= 1) {
        if (sounds)
            for (auto& c : cmds_)
                if (c->sound) sounds->push_back({FxSoundEvent::Stop, nullptr, c->index, col(world_, 3), 1});
        return false;
    }
    if (h.flags & kFxDetach && attached_) { // CFxInstance_Detach (10007ba0)
        base_ = world_;
        pos_ = col(world_, 3);
        scale_ = {1, 1, 1};
        owner_ = Mat4::identity();
        attached_ = false;
        compose_local();
    }
    return true;
}

void EffectInstance::render(FxFrame& out, Vec3 eye, unsigned lod) {
    // CFxInstance_Render (10007d10): frustum, distance class and occlusion tests are the caller's;
    // flag 0x800 effects are drawn only on forced renders in the original (not modelled).
    if (!enabled_ || !drawable_ || lod >= 8) return;
    RenderState rs{world_, inverse_affine(world_).transform_point(eye), eye, lod, true, *ctx_, out};
    for (auto& c : cmds_) c->render(rs);
}

} // namespace parkan::fx
