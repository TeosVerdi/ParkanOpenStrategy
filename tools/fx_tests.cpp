// Visual effects tests (notes/effects.md): the Effect.dll LFSR, FXID / EXPL decoding of the whole
// corpus, and a deterministic simulation of one explosion. Linked into parkan-tests.
#include "fx/effect.hpp"
#include "fx/spawn_frames.hpp"
#include "res/library.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <map>

using namespace parkan;
using namespace parkan::fx;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

void put32(Bytes& b, std::size_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
void putf(Bytes& b, std::size_t off, float v) { std::memcpy(b.data() + off, &v, 4); }

// Lfsr16Next (10002220) by hand: a = (a<<1)^b, b = (b>>1)^a.
void test_lfsr() {
    Lfsr16 r;
    r.a = 1, r.b = 0;
    CHECK(r.next() == 2);
    CHECK(r.next() == 7);
    CHECK(r.next() == 8);
    Lfsr16 c;
    c.a = 1, c.b = 0;
    CHECK(std::abs(c.centered(4.f) - (2 * (4.f / 65536) - 2)) < 1e-6f); // LfsrRandCentered (10002680)
    Lfsr16 z;
    z.seed(0);
    CHECK(z.a | z.b); // a zero pair would never move
}

// A hand-built FXID: header + one opcode-3 sprite with the visibility bit, then strictness.
void test_decode_synthetic() {
    Bytes b(0x3c + 200, 0);
    put32(b, 0, 1);         // one command
    put32(b, 4, 1);         // linear time
    putf(b, 8, 2.5f);       // duration
    put32(b, 0x10, 0x16);   // flags: one-shot, detach, always shown
    put32(b, 0x14, 0x311);  // settings key
    putf(b, 0x30, 2), putf(b, 0x34, 2), putf(b, 0x38, 2);
    const std::size_t c = 0x3c;
    put32(b, c, 0x103);     // opcode 3, tested
    put32(b, c + 4, 2);     // plane-aligned
    putf(b, c + 0x24, 0.5f); // t1
    putf(b, c + 0x70, 5);   // size1.x
    std::memcpy(b.data() + c + 0x88, "material.lib", 12);
    std::memcpy(b.data() + c + 0xa8, "GLOW_W\0garbage", 14);
    const FxEffect fx = decode_fx(b);
    CHECK(fx.list.size() == 1 && fx.list[0].opcode == 3 && fx.list[0].tested);
    CHECK(fx.header.duration == 2.5f && fx.header.flags == 0x16 && fx.header.scale.y == 2);
    const auto& s = std::get<FxSpriteCmd>(fx.list[0].data);
    CHECK(s.align == 2 && s.t1 == 0.5f && s.size1.x == 5 && s.material.name == "GLOW_W");
    Bytes bad = b;
    put32(bad, c, 6); // opcode 6 never ships
    bool threw = false;
    try {
        decode_fx(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    bad = b;
    put32(bad, c, 0x403); // unknown command-word bit
    threw = false;
    try {
        decode_fx(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void test_corpus_decode(AssetLibrary& lib) {
    std::map<std::uint32_t, int> ops;
    int effects = 0, failed = 0, tested = 0, materials = 0, missing_materials = 0;
    for (const auto& e : lib.archive("effects.rlb").entries()) {
        try {
            const FxEffect fx = decode_fx(e.data);
            ++effects;
            for (const auto& c : fx.list) {
                ++ops[c.opcode];
                tested += c.tested;
                const FxResourceRef* m = nullptr;
                if (auto* s = std::get_if<FxSpriteCmd>(&c.data)) m = &s->material;
                if (auto* s = std::get_if<FxBeamCmd>(&c.data)) m = &s->material;
                if (auto* s = std::get_if<FxBurstCmd>(&c.data)) m = &s->material;
                if (auto* s = std::get_if<FxTrailCmd>(&c.data)) m = &s->material;
                if (m && !m->name.empty()) {
                    ++materials;
                    if (!lib.archive("Material.lib").find_name(m->name)) ++missing_materials;
                }
            }
        } catch (const std::exception& ex) {
            if (failed++ < 5) std::printf("  FAILED effects.rlb:%s: %s\n", e.name.c_str(), ex.what());
        }
    }
    g_fail += failed;
    // Opcode census of the shipped data (both releases carry the same effects.rlb).
    CHECK(effects == 923);
    CHECK(ops[1] == 618 && ops[2] == 517 && ops[3] == 1545 && ops[4] == 202 && ops[5] == 31);
    CHECK(ops[7] == 1161 && ops[8] == 237 && ops[9] == 266 && ops[10] == 160);
    CHECK(missing_materials == 0);
    // EXPL definitions in the object archives; every default effect exists.
    int expl = 0, expl_failed = 0, unresolved = 0, surfaces = 0;
    for (const char* a : {"animals.rlb", "static.rlb", "system.rlb", "turrets.rlb", "weapon.rlb"}) {
        for (const auto& e : lib.archive(a).entries()) {
            if (e.type != fourcc("EXPL")) continue;
            try {
                const ExplosionDef x = decode_expl(e.data);
                ++expl;
                if (!x.fx[0].second.empty() && !lib.find(x.fx[0].first, x.fx[0].second)) ++unresolved;
                for (std::size_t k = 1; k < x.fx.size(); ++k)
                    if (x.fx_valid[k] && lib.find(x.fx[k].first, x.fx[k].second)) ++surfaces;
            } catch (const std::exception& ex) {
                if (expl_failed++ < 5) std::printf("  FAILED %s:%s: %s\n", a, e.name.c_str(), ex.what());
            }
        }
    }
    g_fail += expl_failed;
    CHECK(expl == 144);
    CHECK(unresolved == 0);
    std::printf("  fx: %d effects (%d tested commands), %d material refs; %d EXPL, %d surface variants\n", effects,
                tested, materials, expl, surfaces);
}

// Every effect runs for 2 s and produces finite geometry.
void test_corpus_run(AssetLibrary& lib) {
    FxContext ctx(lib);
    int bad = 0, quads = 0;
    FxFrame f;
    for (const auto& e : lib.archive("effects.rlb").entries()) {
        auto fx = load_effect(lib, e.name);
        EffectInstance inst(fx, ctx, Mat4::translation({0, 0, 1}), 0.0);
        inst.set_enabled(true, 0);
        for (int i = 1; i <= 30; ++i) {
            inst.set_param(float(i) / 30);
            inst.set_transform(Mat4::translation({float(i), 0, 1}));
            if (!inst.update(i / 15.0)) break;
            f.clear();
            inst.render(f, {0, -20, 5});
            for (const auto& q : f.quads) {
                ++quads;
                for (const auto& p : q.p) bad += !std::isfinite(p.x + p.y + p.z);
                bad += !(q.rgba[3] >= -1e-3f && q.rgba[3] <= 1.001f);
            }
            for (const auto& t : f.triangles)
                for (const auto& p : t.p) bad += !std::isfinite(p.x + p.y + p.z);
        }
    }
    CHECK(bad == 0);
    CHECK(quads > 10000);
}

// exp_m_mis (missile explosion: flash sprite, fire burst, sound, light) from a fixed seed.
void test_simulation(AssetLibrary& lib) {
    auto fx = load_effect(lib, "exp_m_mis");
    CHECK(fx != nullptr);
    if (!fx) return;
    auto run = [&](std::uint32_t seed, FxFrame& f, std::vector<FxSoundEvent>& snd) {
        FxContext ctx(lib, seed);
        EffectInstance inst(fx, ctx, Mat4::translation({10, 20, 0}), 0.0);
        bool alive = true;
        for (int i = 1; i <= 18 && alive; ++i) alive = inst.update(i / 30.0, &snd); // to 0.6 s
        f.clear();
        inst.render(f, {10, 0, 5});
        // Header: linear time over 3 s, one-shot.
        CHECK(std::abs(inst.t() - 0.2f) < 1e-4f);
        return alive;
    };
    FxFrame a, b, c;
    std::vector<FxSoundEvent> sa, sb, sc;
    CHECK(run(7, a, sa) && run(7, b, sb));
    run(8, c, sc);
    CHECK(a.quads.size() == b.quads.size() && !a.quads.empty());
    bool same = a.quads.size() == b.quads.size(), differs = a.quads.size() != c.quads.size();
    for (std::size_t i = 0; i < a.quads.size() && i < b.quads.size(); ++i)
        for (int k = 0; k < 4; ++k) {
            same &= a.quads[i].p[k].x == b.quads[i].p[k].x && a.quads[i].p[k].z == b.quads[i].p[k].z;
            if (i < c.quads.size()) differs |= a.quads[i].p[k].x != c.quads[i].p[k].x;
        }
    CHECK(same);    // deterministic for a seed
    CHECK(differs); // and the seed matters
    // The sound fires once at t0 = 0 (first update), the light lives over its window.
    CHECK(sa.size() == 1 && sa[0].kind == FxSoundEvent::Play && sa[0].sound->entry == "hit_mis_metal.wav");
    // Every particle stays inside its spawn sphere plus its own size around the origin.
    float far = 0;
    for (const auto& q : a.quads)
        for (const auto& p : q.p) far = std::max(far, std::sqrt(dot(p - Vec3{10, 20, 0}, p - Vec3{10, 20, 0})));
    CHECK(far < 15);
    std::printf("  exp_m_mis @0.6 s: %zu quads, %zu lights, farthest corner %.2f m\n", a.quads.size(), a.lights.size(), far);
    // A one-shot effect ends when t reaches 1 (3 s).
    FxContext ctx(lib);
    EffectInstance inst(fx, ctx, Mat4::identity(), 0.0);
    CHECK(inst.update(2.9));
    CHECK(!inst.update(3.0));
}

// Sizes: effects are authored in the units of the spawning event's CTPT frame (type-4 events,
// fx::contact_frame — bullets ×0.02…0.2, gun flashes ×0.3). Every effect spawned by a type-4 event
// whose points resolve (bullets through load_bullet, other objects through the .cpt of the same base
// name) runs 2 s in its largest spawn frame and must stay below 60 m (except the bridge light beam); the rest run at scale 1 under
// a looser bound, except the ones documented as scaled by other means (notes/effects.md §8, §9).
void test_sizes(AssetLibrary& lib) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::map<std::string, Mat4> frames = collect_spawn_frames(lib);
    int bullets = 0;
    for (const auto& e : lib.archive("objects.rlb").entries()) bullets += e.type == fourcc("BULL");
    FxContext ctx(lib);
    int framed = 0, too_big = 0;
    float worst_framed = 0;
    std::string worst_name;
    for (const auto& e : lib.archive("effects.rlb").entries()) {
        const std::string name = lower(e.name);
        auto it = frames.find(name);
        // Unit-scale exceptions: b_bullight_* are referenced by nothing; tree effects are scaled by
        // their (shared-.cpt) trees, B_Sphere_* by event type 5, selfexp_* through EXPL part scaling.
        const bool exempt = it == frames.end() &&
                            (name.rfind("b_bullight", 0) == 0 || name.rfind("tree_", 0) == 0 || name.rfind("exp_tree", 0) == 0);
        const bool big = name == "f_brige_ray"; // the bridge's 150 m light beam (radar/ray effects reach 40 m)
        auto fx = load_effect(lib, e.name);
        EffectInstance inst(fx, ctx, Mat4::identity(), 0.0);
        if (it != frames.end()) inst.set_spawn_frame(it->second), ++framed;
        inst.set_enabled(true, 0);
        float mx = 0;
        FxFrame f;
        for (int i = 1; i <= 60; ++i) {
            inst.set_param(float(i % 30) / 30);
            inst.set_transform(Mat4::translation({float(i) * 0.5f, 0, 1}));
            if (!inst.update(i / 30.0)) break;
            f.clear();
            inst.render(f, {0, -30, 5});
            for (const auto& q : f.quads) mx = std::max(mx, std::sqrt(dot(q.p[2] - q.p[0], q.p[2] - q.p[0])));
        }
        const float bound = it != frames.end() ? 60.f : 120.f;
        if (it != frames.end() && !big && mx > worst_framed) worst_framed = mx, worst_name = e.name;
        if (mx > bound && !exempt && !big) {
            ++too_big;
            std::printf("  FAILED size: %s quad diagonal %.1f m (bound %.0f)\n", e.name.c_str(), mx, bound);
        }
    }
    g_fail += too_big;
    CHECK(bullets >= 60 && framed >= 150);
    std::printf("  sizes: %zu effects with a spawn frame (of %d BULL records), largest %s %.1f m\n", frames.size(), bullets,
                worst_name.c_str(), worst_framed);
}

} // namespace

int fx_unit_tests() {
    g_fail = 0;
    test_lfsr();
    test_decode_synthetic();
    return g_fail;
}

int fx_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("effects\n");
    AssetLibrary lib(game);
    test_corpus_decode(lib);
    test_corpus_run(lib);
    test_simulation(lib);
    test_sizes(lib);
    return g_fail;
}
