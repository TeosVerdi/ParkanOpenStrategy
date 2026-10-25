#include "scene/mission_scene.hpp"

#include "core/paths.hpp"
#include "mission/scheme.hpp"
#include "mission/tma.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>

namespace parkan {

namespace {

std::string field(std::span<const std::uint8_t> rec, std::size_t off) {
    if (off + 32 > rec.size()) return {};
    auto p = reinterpret_cast<const char*>(rec.data() + off);
    return std::string(p, ::strnlen(p, 32));
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
                      [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

struct ModelRef {
    std::string archive, mesh;
};

// Follows objects.rlb prototypes until a mesh is found (FORT → building agent → .msh).
std::optional<ModelRef> resolve_prototype(AssetLibrary& lib, const std::string& key, int depth = 0) {
    if (depth > 4) return std::nullopt;
    const NResEntry* e = lib.find("objects.rlb", key);
    if (!e) return std::nullopt;
    const std::string lib0 = field(e->data, 0x00), entry0 = field(e->data, 0x20);
    if (ends_with_ci(entry0, ".msh")) return ModelRef{lib0, entry0};
    if (ends_with_ci(lib0, "objects.rlb")) return resolve_prototype(lib, entry0, depth + 1);
    return std::nullopt;
}

} // namespace

Mat4 tma_placement(const mission::TmaObject& o) {
    // iron3d 0x36610: Rz(z) * Ry(y) * Rx(x), then scale, then translation.
    auto axis = [](float a, int which) {
        const float c = std::cos(a), s = std::sin(a);
        Mat4 m = Mat4::identity();
        if (which == 0) m.at(1, 1) = c, m.at(1, 2) = -s, m.at(2, 1) = s, m.at(2, 2) = c;
        if (which == 1) m.at(0, 0) = c, m.at(0, 2) = s, m.at(2, 0) = -s, m.at(2, 2) = c;
        if (which == 2) m.at(0, 0) = c, m.at(0, 1) = -s, m.at(1, 0) = s, m.at(1, 1) = c;
        return m;
    };
    Mat4 scale = Mat4::identity();
    scale.at(0, 0) = o.scale[0], scale.at(1, 1) = o.scale[1], scale.at(2, 2) = o.scale[2];
    return Mat4::translation({o.position[0], o.position[1], o.position[2]}) * axis(o.orientation[2], 2) *
           axis(o.orientation[1], 1) * axis(o.orientation[0], 0) * scale;
}

namespace {

// Appends `src` to `dst`, every transform pre-multiplied by `world`.
void append(RenderMesh& dst, const RenderMesh& src, const Mat4& world, std::map<std::string, std::int32_t>& tex_ids) {
    const auto vbase = std::int32_t(dst.vertices.size());
    const auto ibase = std::uint32_t(dst.indices.size());
    const auto tbase = std::uint32_t(dst.transforms.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    dst.indices.insert(dst.indices.end(), src.indices.begin(), src.indices.end());
    for (const auto& t : src.transforms) dst.transforms.push_back(world * t);
    std::vector<std::int32_t> remap(src.textures.size(), -1);
    for (std::size_t i = 0; i < src.textures.size(); ++i) {
        auto [it, fresh] = tex_ids.try_emplace(src.textures[i].name, std::int32_t(dst.textures.size()));
        if (fresh) dst.textures.push_back(src.textures[i]);
        remap[i] = it->second;
    }
    for (auto d : src.draws) {
        d.transform += tbase;
        d.index_start += ibase;
        d.base_vertex += vbase;
        if (d.tex0 >= 0) d.tex0 = remap[std::size_t(d.tex0)];
        if (d.tex1 >= 0) d.tex1 = remap[std::size_t(d.tex1)];
        dst.draws.push_back(d);
    }
}

// Adds a scheme node's model and, recursively, its external components. A component's mount is a
// node of its parent's mesh (CAgent::AttachChild; the child's root node becomes that node).
// Internal components (INTO) have no place on the model and are not drawn.
bool add_component(AssetLibrary& lib, const mission::SchemeNode& node, const Mat4* attach, RenderMesh& out,
                   std::map<std::string, std::int32_t>& tex_ids, MissionSceneStats& st) {
    auto ref = resolve_prototype(lib, node.entry);
    if (!ref) return false;
    ModelAsset model = load_model(lib, ref->archive, ref->mesh);
    if (attach) attach_pose(model, *attach);
    for (const auto& c : node.children) {
        const NResEntry* e = lib.find("objects.rlb", c.entry);
        if (!e || e->type != fourcc("EXTO")) continue;
        if (c.mount < 0 || std::size_t(c.mount) >= model.node_world.size() ||
            !add_component(lib, c, &model.node_world[std::size_t(c.mount)], out, tex_ids, st)) {
            ++st.bad_components;
            continue;
        }
        ++st.components;
    }
    if (!attach) out.bounds_min = model.bounds_min, out.bounds_max = model.bounds_max; // frame the base
    append(out, to_render_mesh(std::move(model)), Mat4::identity(), tex_ids);
    return true;
}

} // namespace

std::optional<RenderMesh> load_scheme_render_mesh(AssetLibrary& lib, std::string_view rel, MissionSceneStats* stats) {
    auto path = resolve_path(lib.game_dir(), rel);
    if (!path) return std::nullopt;
    const mission::Scheme scheme = mission::decode_scheme(read_file(*path));
    if (!ends_with_ci(scheme.root.lib, "objects.rlb")) return std::nullopt;
    RenderMesh out;
    std::map<std::string, std::int32_t> tex_ids;
    MissionSceneStats local;
    MissionSceneStats& st = stats ? *stats : local;
    if (!add_component(lib, scheme.root, nullptr, out, tex_ids, st)) return std::nullopt;
    return out;
}

std::optional<UnitModel> load_unit_model(AssetLibrary& lib, std::string_view rel) {
    auto path = resolve_path(lib.game_dir(), rel);
    if (!path) return std::nullopt;
    return load_unit_model(lib, mission::decode_scheme(read_file(*path)));
}

std::optional<UnitModel> load_unit_model(AssetLibrary& lib, const mission::Scheme& scheme) {
    UnitModel u;
    std::map<std::string, std::int32_t> tex_ids;
    // Depth-first like add_component; parts are posed in this order, so parents come first.
    auto add = [&](auto&& self, const mission::SchemeNode& node, int parent) -> bool {
        auto ref = resolve_prototype(lib, node.entry);
        if (!ref) return false;
        UnitPart part;
        part.model = load_model(lib, ref->archive, ref->mesh);
        part.parent = parent;
        part.mount = node.mount;
        // Prototype pairs: 0 mesh, 1 WEAR, 2 CTPT, 3 NDPR, 4 CTLD (STAT/BTLU/EXTO records).
        if (const NResEntry* pe = lib.find("objects.rlb", node.entry); pe && pe->data.size() >= 0x140) {
            part.archive = field(pe->data, 0x80);
            part.cpt_entry = field(pe->data, 0xa0);
            part.ctl_entry = field(pe->data, 0x120);
            part.ndp_archive = field(pe->data, 0xc0);
            part.ndp_entry = field(pe->data, 0xe0);
        }
        if (parent >= 0) attach_pose(part.model, u.parts[std::size_t(parent)].model.node_world[std::size_t(node.mount)]);
        else if (const NResEntry* e = lib.find("objects.rlb", node.entry); e && e->data.size() >= 0x140) {
            u.ctl_archive = field(e->data, 0x100), u.ctl_entry = field(e->data, 0x120);
            if (e->data.size() >= 0x180) u.chassis_sheet = field(e->data, 0x160);
        }
        part.transform_base = std::uint32_t(u.mesh.transforms.size());
        append(u.mesh, to_render_mesh(part.model), Mat4::identity(), tex_ids);
        u.parts.push_back(std::move(part));
        const int self_index = int(u.parts.size()) - 1;
        for (const auto& c : node.children) {
            const NResEntry* e = lib.find("objects.rlb", c.entry);
            if (!e || e->type != fourcc("EXTO") || c.mount < 0 ||
                std::size_t(c.mount) >= u.parts[std::size_t(self_index)].model.node_world.size())
                continue;
            self(self, c, self_index);
        }
        return true;
    };
    if (!add(add, scheme.root, -1)) return std::nullopt;
    const auto& base = u.parts.front().model;
    u.mesh.bounds_min = base.bounds_min, u.mesh.bounds_max = base.bounds_max;
    return u;
}

void pose_unit(UnitModel& unit, const Mat4& world, float time_a, float time_b, float blend, std::vector<Mat4>& transforms,
               std::size_t first, const Mat4* steady_root) {
    Mat4 steady = Mat4::identity(); // rest rotation · inverse(current root rotation), applied to chassis mounts
    for (auto& part : unit.parts) {
        if (part.parent < 0) {
            pose_model_blend(part.model, time_a, time_b, blend);
            if (steady_root && !part.model.node_local.empty()) {
                Mat4 unroot = Mat4::identity();
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) unroot.at(r, c) = part.model.node_local[0].at(c, r); // rotation only: transpose
                steady = *steady_root * unroot;
            }
        } else if (steady_root && part.parent == 0) {
            const Mat4 mount = steady * unit.parts[0].model.node_world[std::size_t(part.mount)];
            pose_model_frames(part.model, part.node_frames, &mount);
        } else {
            pose_model_frames(part.model, part.node_frames,
                              &unit.parts[std::size_t(part.parent)].model.node_world[std::size_t(part.mount)]);
        }
        for (std::size_t i = 0; i < part.model.node_world.size(); ++i) {
            const std::size_t at = first + part.transform_base + i;
            if (at < transforms.size()) transforms[at] = world * part.model.node_world[i];
        }
    }
}

RenderMesh load_mission_render_mesh(AssetLibrary& lib, std::string_view mission_dir, MissionSceneStats* stats,
                                   const std::function<bool(std::size_t)>& skip) {
    auto dir = resolve_path(lib.game_dir(), mission_dir);
    if (!dir) throw std::runtime_error("mission folder not found: " + std::string(mission_dir));
    auto tma_path = resolve_path(*dir, "data.tma");
    if (!tma_path) throw std::runtime_error("no data.tma in " + dir->string());
    const mission::Tma tma = mission::decode_tma(read_file(*tma_path));

    // land_path is "DATA\MAPS\<map>\land": the map folder holds Land.msh and Land1/Land2.wea.
    std::string map_dir = tma.land_path;
    if (auto cut = map_dir.find_last_of("\\/"); cut != std::string::npos) map_dir.resize(cut);
    RenderMesh scene = load_terrain_render_mesh(lib, map_dir);
    std::map<std::string, std::int32_t> tex_ids;
    for (std::size_t i = 0; i < scene.textures.size(); ++i) tex_ids.emplace(scene.textures[i].name, std::int32_t(i));

    std::map<std::string, std::optional<RenderMesh>> cache; // per resource
    MissionSceneStats st;
    for (std::size_t oi = 0; oi < tma.objects.size(); ++oi) {
        const auto& o = tma.objects[oi];
        if (skip && skip(oi)) continue;
        ++st.objects;
        auto [it, fresh] = cache.try_emplace(o.resource);
        if (fresh) {
            try {
                if (ends_with_ci(o.resource, ".dat")) {
                    it->second = load_scheme_render_mesh(lib, o.resource, &st);
                } else if (auto ref = resolve_prototype(lib, o.resource)) {
                    it->second = to_render_mesh(load_model(lib, ref->archive, ref->mesh));
                }
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "object %s: %s\n", o.resource.c_str(), ex.what());
            }
        }
        if (!it->second) {
            ++st.unresolved;
            continue;
        }
        append(scene, *it->second, tma_placement(o), tex_ids);
        ++st.placed;
    }
    if (stats) *stats = st;
    return scene;
}

} // namespace parkan
