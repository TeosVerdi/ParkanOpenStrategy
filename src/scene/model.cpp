#include "scene/model.hpp"

#include <algorithm>
#include <cfloat>
#include <map>

namespace parkan {

namespace {

Mat4 pose_matrix(const NodePose& p) {
    // The native local matrix (AniMesh via g_FastProc) is the transpose of the usual quaternion
    // basis, i.e. it rotates by the conjugate (FParkan reference/msh).
    return Mat4::translation(p.pos) * Mat4::rotation(p.rot.conjugate());
}

NodePose key_pose(const MeshKey& k) {
    return {{k.pos[0], k.pos[1], k.pos[2]}, {k.quat[0], k.quat[1], k.quat[2], k.quat[3]}};
}

Mat4 key_transform(const MeshKey& k) { return pose_matrix(key_pose(k)); }

std::string base_name(std::string_view entry) {
    auto dot = entry.rfind('.');
    return std::string(entry.substr(0, dot));
}

} // namespace

NodePose sample_node(const Mesh& m, std::size_t node, float time) {
    if (node >= m.nodes.size() || m.keys.empty()) return {};
    const MeshNode& n = m.nodes[node];
    const auto fallback = [&] { return n.fallback_key < m.keys.size() ? key_pose(m.keys[n.fallback_key]) : NodePose{}; };
    const float frame_f = std::nearbyint(time - 0.5f); // x87 default rounding: nearest, ties to even
    if (!(frame_f >= 0) || frame_f >= float(m.frame_count) || n.anim_map_start == 0xFFFF) return fallback();
    const std::size_t map_index = std::size_t(n.anim_map_start) + std::size_t(frame_f);
    if (map_index >= m.frame_map.size()) return fallback();
    const std::uint16_t k = m.frame_map[map_index];
    if (k >= n.fallback_key || std::size_t(k) + 1 >= m.keys.size()) return fallback();
    const MeshKey &a = m.keys[k], &b = m.keys[k + 1];
    if (time == a.time) return key_pose(a);
    if (time == b.time) return key_pose(b);
    const float f = (time - a.time) / (b.time - a.time);
    const NodePose pa = key_pose(a), pb = key_pose(b);
    return {pa.pos * (1 - f) + pb.pos * f, slerp(pa.rot, pb.rot, f)};
}

NodePose sample_node_blend(const Mesh& m, std::size_t node, float time_a, float time_b, float blend) {
    const bool use_a = blend < 1 && time_a >= 0, use_b = blend > 0 && time_b >= 0;
    if (use_a && !use_b) return sample_node(m, node, time_a);
    if (use_b && !use_a) return sample_node(m, node, time_b);
    if (!use_a) return sample_node(m, node, -1); // neither: the native code reads garbage; use the fallback
    const NodePose a = sample_node(m, node, time_a), b = sample_node(m, node, time_b);
    return {a.pos * (1 - blend) + b.pos * blend, slerp(a.rot, b.rot, blend)};
}

void pose_model_blend(ModelAsset& model, float time_a, float time_b, float blend, const Mat4* attach) {
    const auto& nodes = model.mesh.nodes;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const bool root = nodes[i].parent == 0xFFFF;
        NodePose p = sample_node_blend(model.mesh, i, time_a, time_b, blend);
        if (root) p.pos = {};
        model.node_local[i] = pose_matrix(p);
        model.node_world[i] = root ? (attach ? *attach : model.node_local[i])
                                   : model.node_world[nodes[i].parent] * model.node_local[i];
    }
}

void pose_model(ModelAsset& model, float time, const Mat4* attach) { pose_model_blend(model, time, -1, 0, attach); }

ModelAsset load_model(AssetLibrary& lib, std::string_view archive, std::string_view mesh_entry, int state, int lod) {
    ModelAsset out;
    const NResEntry* me = lib.find(archive, mesh_entry);
    if (!me) throw std::runtime_error("mesh not found: " + std::string(mesh_entry));
    out.mesh = decode_mesh(me->data);
    const Mesh& m = out.mesh;

    // Static pose: fallback key per node, parents first.
    out.node_local.resize(std::max<std::size_t>(m.nodes.size(), 1), Mat4::identity());
    out.node_world.resize(out.node_local.size(), Mat4::identity());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const auto& n = m.nodes[i];
        if (!m.keys.empty()) out.node_local[i] = key_transform(m.keys[n.fallback_key]);
        out.node_world[i] = n.parent == 0xFFFF ? out.node_local[i] : out.node_world[n.parent] * out.node_local[i];
    }

    // Materials: WEAR row → MAT0 (by name, else DEFAULT, else entry 0) → phase 0 texture.
    std::vector<std::int32_t> row_texture;
    std::vector<unsigned> row_category;
    std::vector<float> row_opacity;
    std::map<std::string, std::int32_t> tex_index;
    if (const NResEntry* we = lib.find(archive, base_name(mesh_entry) + ".wea")) {
        Wear w = parse_wear({reinterpret_cast<const char*>(we->data.data()), we->data.size()});
        const NResArchive& mats = lib.archive("Material.lib");
        for (const auto& mname : w.materials) {
            const NResEntry* mat = mats.find_name(mname);
            if (!mat) mat = mats.find_name("DEFAULT");
            if (!mat && !mats.entries().empty()) mat = &mats.entries().front();
            std::int32_t ti = -1;
            unsigned cat = 0;
            float op = 1;
            if (mat) {
                Material md = decode_mat0(mat->data, mat->attr1, mat->attr2);
                cat = md.category();
                if (!md.phases.empty()) {
                    op = std::min(1.f, md.phases[0].opacity());
                    const std::string& tn = md.phases[0].texture;
                    if (!tn.empty()) {
                        if (auto it = tex_index.find(tn); it != tex_index.end()) ti = it->second;
                        else if (const NResEntry* te = lib.find("Textures.lib", tn)) {
                            Texm t = decode_texm(te->data);
                            ti = std::int32_t(out.textures.size());
                            out.textures.push_back({tn, t.width, t.height, std::move(t.mips_rgba.front())});
                            tex_index[tn] = ti;
                        }
                    }
                }
            }
            row_texture.push_back(ti);
            row_category.push_back(cat);
            row_opacity.push_back(op);
        }
    }

    // Draws: each node's selected slot.
    out.bounds_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    out.bounds_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const std::uint16_t si = m.nodes[i].slots[std::size_t(state * 5 + lod)];
        if (si == 0xFFFF) continue;
        const MeshSlot& s = m.slots[si];
        for (std::size_t b = s.batch_start; b < std::size_t(s.batch_start) + s.batch_count; ++b) {
            const MeshBatch& mb = m.batches[b];
            ModelBatchDraw d;
            d.node = std::uint32_t(i);
            d.index_start = mb.index_start;
            d.index_count = mb.index_count;
            d.base_vertex = std::int32_t(mb.base_vertex);
            if (mb.material < row_texture.size()) {
                d.texture = row_texture[mb.material];
                d.blend_category = row_category[mb.material];
                d.opacity = row_opacity[mb.material];
            }
            out.draws.push_back(d);
            for (std::size_t k = 0; k < mb.index_count; ++k) {
                const auto& p = m.positions[mb.base_vertex + m.indices[mb.index_start + k]];
                Vec3 w = out.node_world[i].transform_point({p[0], p[1], p[2]});
                out.bounds_min = {std::min(out.bounds_min.x, w.x), std::min(out.bounds_min.y, w.y),
                                  std::min(out.bounds_min.z, w.z)};
                out.bounds_max = {std::max(out.bounds_max.x, w.x), std::max(out.bounds_max.y, w.y),
                                  std::max(out.bounds_max.z, w.z)};
            }
        }
    }
    if (out.draws.empty()) throw std::runtime_error("model has nothing to draw in this state/LOD");
    return out;
}

void attach_pose(ModelAsset& model, const Mat4& attach) {
    const auto& nodes = model.mesh.nodes;
    for (std::size_t i = 0; i < model.node_world.size(); ++i) {
        const bool root = i >= nodes.size() || nodes[i].parent == 0xFFFF;
        model.node_world[i] = root ? attach : model.node_world[nodes[i].parent] * model.node_local[i];
    }
}

} // namespace parkan
