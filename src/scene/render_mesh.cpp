#include "scene/render_mesh.hpp"

#include "core/paths.hpp"
#include "res/terrain.hpp"

#include <algorithm>
#include <cfloat>
#include <map>

namespace parkan {

RenderMesh to_render_mesh(ModelAsset model) {
    RenderMesh r;
    const Mesh& m = model.mesh;
    r.vertices.resize(m.positions.size());
    for (std::size_t i = 0; i < r.vertices.size(); ++i) {
        auto& v = r.vertices[i];
        std::memcpy(v.pos, m.positions[i].data(), 12);
        if (i < m.normals.size()) std::memcpy(v.nrm, m.normals[i].data(), 4);
        else v.nrm[0] = v.nrm[1] = 0, v.nrm[2] = 127, v.nrm[3] = 0;
        if (i < m.uv0.size()) v.uv0[0] = m.uv0[i][0], v.uv0[1] = m.uv0[i][1];
        else v.uv0[0] = v.uv0[1] = 0;
        v.uv1[0] = v.uv0[0], v.uv1[1] = v.uv0[1];
        v.blend = 0;
    }
    r.indices.assign(m.indices.begin(), m.indices.end());
    r.transforms = model.node_world;
    for (const auto& d : model.draws)
        r.draws.push_back({d.node, d.index_start, d.index_count, d.base_vertex, d.texture, -1,
                           d.blend_category != 0 ? std::uint32_t(kDrawAlphaTest) : 0u});
    r.textures = std::move(model.textures);
    r.bounds_min = model.bounds_min;
    r.bounds_max = model.bounds_max;
    return r;
}

namespace {

// WEAR row → MAT0 → phase 0 texture → Texm, with the original's fallbacks (DEFAULT, entry 0).
class MaterialTextures {
public:
    MaterialTextures(AssetLibrary& lib, std::vector<ModelTexture>& out) : lib_(lib), out_(out) {}

    // `block` 1 is the detail texture (LxxM) of a landscape material; a material with fewer
    // phases uses phase 0 (CMatManager_GetMaterialPhase, World3D 100031f0).
    std::int32_t texture_for_material(const std::string& name, std::size_t block = 0) {
        const std::string key = name + "#" + std::to_string(block);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        auto t = load_material_texture(lib_, name, block);
        if (!t) return cache_[key] = -1;
        for (std::size_t i = 0; i < out_.size(); ++i)
            if (out_[i].name == t->name) return cache_[key] = std::int32_t(i);
        out_.push_back(std::move(*t));
        return cache_[key] = std::int32_t(out_.size() - 1);
    }

private:
    AssetLibrary& lib_;
    std::vector<ModelTexture>& out_;
    std::map<std::string, std::int32_t> cache_;
};

Wear load_loose_wear(const std::filesystem::path& p) {
    auto b = read_file(p);
    return parse_wear({reinterpret_cast<const char*>(b.data()), b.size()});
}

} // namespace

std::uint32_t append_render_mesh(RenderMesh& dst, const RenderMesh& src, const Mat4& world) {
    const auto vbase = std::int32_t(dst.vertices.size());
    const auto ibase = std::uint32_t(dst.indices.size());
    const auto tbase = std::uint32_t(dst.transforms.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    dst.indices.insert(dst.indices.end(), src.indices.begin(), src.indices.end());
    for (const auto& t : src.transforms) dst.transforms.push_back(world * t);
    std::vector<std::int32_t> remap(src.textures.size(), -1);
    for (std::size_t i = 0; i < src.textures.size(); ++i) {
        for (std::size_t j = 0; j < dst.textures.size() && remap[i] < 0; ++j)
            if (dst.textures[j].name == src.textures[i].name) remap[i] = std::int32_t(j);
        if (remap[i] < 0) {
            remap[i] = std::int32_t(dst.textures.size());
            dst.textures.push_back(src.textures[i]);
        }
    }
    for (auto d : src.draws) {
        d.transform += tbase;
        d.index_start += ibase;
        d.base_vertex += vbase;
        if (d.tex0 >= 0) d.tex0 = remap[std::size_t(d.tex0)];
        if (d.tex1 >= 0) d.tex1 = remap[std::size_t(d.tex1)];
        dst.draws.push_back(d);
    }
    return tbase;
}

std::optional<ModelTexture> load_material_texture(AssetLibrary& lib, const std::string& material, std::size_t block) {
    const NResArchive& mats = lib.archive("Material.lib");
    const NResEntry* mat = mats.find_name(material);
    if (!mat) mat = mats.find_name("DEFAULT");
    if (!mat) return std::nullopt;
    Material md = decode_mat0(mat->data, mat->attr1, mat->attr2);
    if (md.phases.empty()) return std::nullopt;
    const std::string& tn = md.phases[block < md.phases.size() ? block : 0].texture;
    if (tn.empty()) return std::nullopt;
    const NResEntry* te = lib.find("Textures.lib", tn);
    if (!te) return std::nullopt;
    Texm t = decode_texm(te->data);
    return ModelTexture{tn, t.width, t.height, std::move(t.mips_rgba.front())};
}

RenderMesh load_terrain_render_mesh(AssetLibrary& lib, std::string_view map_dir) {
    auto dir = resolve_path(lib.game_dir(), map_dir);
    if (!dir) throw std::runtime_error("map folder not found: " + std::string(map_dir));
    auto land = resolve_path(*dir, "Land.msh");
    if (!land) throw std::runtime_error("no Land.msh in " + dir->string());
    Terrain t = decode_terrain(read_file(*land));

    RenderMesh r;
    MaterialTextures mt(lib, r.textures);
    // Land1.wea = texture page (phase 0), Land2.wea = detail page (phase 1), same row
    // (notes/terrain-render.md §2).
    std::vector<std::int32_t> texture, detail;
    if (auto w = resolve_path(*dir, "Land1.wea"))
        for (const auto& name : load_loose_wear(*w).materials) texture.push_back(mt.texture_for_material(name, 0));
    if (auto w = resolve_path(*dir, "Land2.wea"))
        for (const auto& name : load_loose_wear(*w).materials) detail.push_back(mt.texture_for_material(name, 1));

    r.vertices.resize(t.positions.size());
    for (std::size_t i = 0; i < r.vertices.size(); ++i) {
        auto& v = r.vertices[i];
        std::memcpy(v.pos, t.positions[i].data(), 12);
        if (i < t.normals.size()) std::memcpy(v.nrm, t.normals[i].data(), 4);
        v.uv0[0] = t.uv_map[i][0], v.uv0[1] = t.uv_map[i][1];
        v.uv1[0] = t.uv_detail[i][0], v.uv1[1] = t.uv_detail[i][1];
        v.blend = t.blend[i];
    }

    // LOD0: each node's first slot is a range of the type-11 draw list. Each face draws its base
    // layer (row lo) and, when hi != 0xff, an overlay (row hi) blended by the type-14 alpha; face
    // flag 0x400 adds the detail texture of the same row (MicroTexturingOn, on by default).
    struct Key {
        bool overlay;
        unsigned row;
        bool detail;
        auto operator<=>(const Key&) const = default;
    };
    std::map<Key, std::vector<std::uint32_t>> groups;
    for (const auto& n : t.nodes) {
        if (n.slots[0] == 0xFFFF) continue;
        const MeshSlot& s = t.slots[n.slots[0]];
        for (std::size_t e = s.tri_start; e < std::size_t(s.tri_start) + s.tri_count; ++e) {
            const auto& face = t.faces[t.draw_list[e].face];
            const unsigned lo = face.material & 0xFF, hi = face.material >> 8;
            const bool det = (face.flags & 0x400) != 0;
            for (const Key k : {Key{false, lo, det}, Key{true, hi, det}}) {
                if (k.overlay && hi == 0xFF) continue;
                auto& list = groups[k];
                list.insert(list.end(), {face.v[0], face.v[1], face.v[2]});
            }
        }
    }
    for (auto& [k, idx] : groups) {
        RenderDraw d;
        d.index_start = std::uint32_t(r.indices.size());
        d.index_count = std::uint32_t(idx.size());
        d.tex0 = k.row < texture.size() ? texture[k.row] : -1;
        if (k.detail && k.row < detail.size() && detail[k.row] >= 0) d.tex1 = detail[k.row], d.flags |= kDrawDetail;
        if (k.overlay) d.flags |= kDrawVertexAlpha;
        r.indices.insert(r.indices.end(), idx.begin(), idx.end());
        r.draws.push_back(d);
    }

    r.bounds_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    r.bounds_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& p : t.positions) {
        r.bounds_min = {std::min(r.bounds_min.x, p[0]), std::min(r.bounds_min.y, p[1]), std::min(r.bounds_min.z, p[2])};
        r.bounds_max = {std::max(r.bounds_max.x, p[0]), std::max(r.bounds_max.y, p[1]), std::max(r.bounds_max.z, p[2])};
    }
    return r;
}

} // namespace parkan
