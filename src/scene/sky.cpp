#include "scene/sky.hpp"

#include "core/paths.hpp"
#include "res/material.hpp"
#include "scene/night_vision.hpp"

#include <cmath>
#include <numbers>

namespace parkan {

namespace {

void set_argb(RenderVertex& v, std::uint32_t argb) {
    v.color[0] = std::uint8_t(argb >> 16), v.color[1] = std::uint8_t(argb >> 8), v.color[2] = std::uint8_t(argb);
    v.color[3] = std::uint8_t(argb >> 24);
}

} // namespace

Vec3 argb_to_rgb(std::uint32_t argb) {
    return {float((argb >> 16) & 0xFF) / 255.f, float((argb >> 8) & 0xFF) / 255.f, float(argb & 0xFF) / 255.f};
}

SkyDome build_sky_dome(int slices) {
    SkyDome d;
    d.slices = slices;
    const float pi = std::numbers::pi_v<float>, cap = pi / 4, h = SkyDome::kHeight;
    const float half = std::sin(cap / 2);
    d.radius = h / (2 * half * half);
    const float R = d.radius;
    auto vertex = [&](Vec3 p) {
        RenderVertex v{};
        v.pos[0] = p.x, v.pos[1] = p.y, v.pos[2] = p.z;
        const Vec3 n = normalize(p - Vec3{0, 0, h - R}); // radial
        v.nrm[0] = std::int8_t(n.x * 127), v.nrm[1] = std::int8_t(n.y * 127), v.nrm[2] = std::int8_t(n.z * 127);
        v.uv0[0] = p.x / R, v.uv0[1] = p.y / R;
        v.uv1[0] = v.uv0[0] * 15, v.uv1[1] = v.uv0[1] * 15;
        return v;
    };
    d.vertices.push_back(vertex({0, 0, h}));
    for (int s = 0; s < slices; ++s) {
        const float phi = 2 * pi * float(s) / float(slices);
        for (int r = 0; r < SkyDome::kRings; ++r) {
            const float theta = float(r + 1) / SkyDome::kRings * cap;
            d.vertices.push_back(vertex({R * std::sin(phi) * std::sin(theta), R * std::cos(phi) * std::sin(theta),
                                         R * std::cos(theta) + h - R}));
        }
    }
    auto at = [&](int s, int r) { return std::uint32_t(1 + (s % slices) * SkyDome::kRings + r); };
    for (int s = 0; s < slices; ++s) {
        d.indices.insert(d.indices.end(), {0u, at(s, 0), at(s + 1, 0)});
        for (int r = 0; r + 1 < SkyDome::kRings; ++r) {
            const std::uint32_t a = at(s, r), b = at(s + 1, r);
            d.indices.insert(d.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    return d;
}

void colour_sky_dome(SkyDome& dome, const mission::AtmosphereState& a, std::uint32_t rim) {
    set_argb(dome.vertices[0], a.zenith);
    const int per_quadrant = std::max(1, dome.slices / 4);
    for (int s = 0; s < dome.slices; ++s) {
        const int q = (s / per_quadrant) & 3, k = s % per_quadrant;
        const float f = float(k) / float(per_quadrant);
        auto around = [&](const std::array<std::uint32_t, 4>& c) { return mission::lerp_argb(c[q], c[(q + 1) & 3], f); };
        const std::uint32_t ring[SkyDome::kRings] = {a.zenith, around(a.high), around(a.mid), around(a.low), rim};
        for (int r = 0; r < SkyDome::kRings; ++r) set_argb(dome.vertices[1 + s * SkyDome::kRings + r], ring[r]);
    }
}

void night_vision_dome(SkyDome& dome) {
    for (RenderVertex& v : dome.vertices) {
        const std::uint32_t argb = std::uint32_t(v.color[3]) << 24 | std::uint32_t(v.color[0]) << 16 | std::uint32_t(v.color[1]) << 8 | v.color[2];
        set_argb(v, night_vision_color(argb));
    }
}

void night_vision_lighting(SceneLighting& l) {
    l.fog_color = night_vision_rgb(l.fog_color);
    l.clear_color = l.fog_color;
    l.night_vision = true;
}

std::uint32_t horizon_colour(const mission::AtmosphereState& a, Vec3 forward) {
    const float pi = std::numbers::pi_v<float>;
    float phi = std::atan2(forward.x, forward.y); // quadrant q faces (sin q·90°, cos q·90°)
    if (phi < 0) phi += 2 * pi;
    const float x = phi / (pi / 2);
    const int q = int(x) & 3;
    return mission::lerp_argb(a.low[q], a.low[(q + 1) & 3], x - std::floor(x)) | 0xFF000000u;
}

SceneLighting scene_lighting(const mission::AtmosphereState& a, Vec3 forward) {
    SceneLighting l;
    l.sun_dir = a.light_dir;
    l.fill_dir = a.light_dir * -1.f;
    l.sun_color = a.sun ? a.light_color : Vec3{};
    l.fill_color = a.sun ? a.back_color : Vec3{};
    l.ambient = a.ambient;
    l.fog_color = argb_to_rgb(horizon_colour(a, forward));
    l.fog_start = a.fog_start;
    l.fog_end = a.fog_end;
    l.clear_color = l.fog_color; // RenderSkyDome's background quad
    return l;
}

SkyTextures load_sky_textures(AssetLibrary& lib, std::string_view mission_dir) {
    SkyTextures out;
    out.textures.resize(3);
    auto dir = resolve_path(lib.game_dir(), mission_dir);
    if (!dir) return out;
    auto wea = resolve_path(*dir, "sky.wea");
    if (!wea) return out;
    const Bytes b = read_file(*wea);
    const Wear w = parse_wear({reinterpret_cast<const char*>(b.data()), b.size()});
    for (std::size_t i = 0; i < 3 && i < w.materials.size(); ++i)
        if (auto t = load_material_texture(lib, w.materials[i], 0)) out.textures[i] = std::move(*t);
    return out;
}

} // namespace parkan
