#include "fx/fx_material.hpp"

#include "res/library.hpp"
#include "res/material.hpp"

#include <algorithm>

namespace parkan::fx {

namespace {

FxBlend blend_of(unsigned category) {
    switch (category) {
    case 1: return FxBlend::Alpha;
    case 2: return FxBlend::Additive;
    case 3: return FxBlend::Multiply;
    case 4: return FxBlend::Multiply2x;
    default: return FxBlend::Opaque;
    }
}

} // namespace

FxMaterialSample FxMaterial::sample(float p, const std::vector<ModelTexture>& textures,
                                    const std::vector<std::vector<TexmPageRect>>& pages) const {
    FxMaterialSample s;
    if (phases.empty()) return s;
    if (!(p >= 0 && p <= 1)) p = 0.5f; // GetMaterialPhaseAt 10003680
    std::size_t cur = 0, next = 0;
    float f = 0;
    if (keys.size() > 1) {
        const auto t = std::uint32_t(float(keys.back().end_ms) * p); // __ftol (truncation)
        std::size_t i = keys.size() - 1;
        for (; i > 0; --i)
            if (keys[i - 1].end_ms <= t && t < keys[i].end_ms) break;
        const std::uint32_t start = i ? keys[i - 1].end_ms : 0;
        const std::uint32_t span = keys[i].end_ms > start ? keys[i].end_ms - start : 1;
        f = float(t - std::min(t, start)) / float(span);
        cur = std::min<std::size_t>(keys[i].phase, phases.size() - 1);
        next = std::min<std::size_t>(keys[(i + 1) % keys.size()].phase, phases.size() - 1);
    } else if (keys.size() == 1) {
        cur = next = std::min<std::size_t>(keys[0].phase, phases.size() - 1);
    }
    const Phase &a = phases[cur], &b = phases[next];
    auto mix = [&](const float* x, const float* y, bool on, float* out) {
        for (int k = 0; k < 3; ++k) out[k] = on ? x[k] + (y[k] - x[k]) * f : x[k];
    };
    mix(a.emissive, b.emissive, blend_mask & 1, s.rgb); // FUN_10003030 (World3D)
    mix(a.diffuse, b.diffuse, blend_mask & 2, s.diffuse);
    s.opacity = blend_mask & 0x10 ? a.opacity + (b.opacity - a.opacity) * f : a.opacity;
    s.texture = a.texture;
    if (a.texture >= 0) {
        const auto& tex = textures[std::size_t(a.texture)];
        const auto& pg = pages[std::size_t(a.texture)];
        if (a.frame >= 0 && std::size_t(a.frame) < pg.size() && tex.width && tex.height) {
            const TexmPageRect& r = pg[std::size_t(a.frame)];
            s.uv0[0] = float(r.x) / float(tex.width), s.uv0[1] = float(r.y) / float(tex.height);
            s.uv1[0] = float(r.x + r.width) / float(tex.width), s.uv1[1] = float(r.y + r.height) / float(tex.height);
        }
    }
    return s;
}

std::int32_t FxMaterialLibrary::texture(const std::string& name) {
    if (name.empty()) return -1;
    if (auto it = tex_by_name_.find(name); it != tex_by_name_.end()) return it->second;
    std::int32_t idx = -1;
    if (const NResEntry* te = lib_.find("Textures.lib", name)) {
        Texm t = decode_texm(te->data);
        idx = std::int32_t(textures.size());
        textures.push_back(ModelTexture{name, t.width, t.height, std::move(t.mips_rgba.front())});
        pages.push_back(std::move(t.pages));
    }
    return tex_by_name_[name] = idx;
}

std::int32_t FxMaterialLibrary::get(const std::string& name) {
    std::string key = name;
    for (auto& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;
    const NResArchive& mats = lib_.archive("Material.lib");
    const NResEntry* e = mats.find_name(name);
    if (!e) e = mats.find_name("DEFAULT"); // LoadMat0Material 10004310: "Material %s not found."
    std::int32_t idx = -1;
    if (e) {
        const Material md = decode_mat0(e->data, e->attr1, e->attr2);
        FxMaterial m;
        m.name = e->name;
        m.blend = blend_of(md.category());
        constexpr float k = 1.f / 255;
        for (const auto& ph : md.phases) {
            FxMaterial::Phase p;
            for (int c = 0; c < 3; ++c) {
                p.emissive[c] = ph.params[std::size_t(c)] * k;
                p.diffuse[c] = ph.params[std::size_t(4 + c)] * k;
                p.specular[c] = ph.params[std::size_t(8 + c)] * k;
            }
            p.opacity = ph.opacity();
            p.frame = std::int8_t(ph.params[17]);
            p.texture = texture(ph.texture);
            m.phases.push_back(p);
        }
        if (!md.anims.empty()) {
            m.blend_mask = md.anims[0].header >> 3;
            for (const auto& key : md.anims[0].keys) m.keys.push_back({key.phase, key.end_time_ms});
        }
        idx = std::int32_t(materials_.size());
        materials_.push_back(std::move(m));
    }
    return by_name_[key] = idx;
}

} // namespace parkan::fx
