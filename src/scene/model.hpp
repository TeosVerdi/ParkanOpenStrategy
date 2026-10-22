// A drawable model assembled from its resources: mesh geometry, WEAR → MAT0 materials, Texm
// textures and the static node pose. The original does this in AniMesh CAgent::AssembleComponents
// with World3D CMatManager (notes/formats-wea-cpt-ndp.md).
#pragma once

#include "core/math.hpp"
#include "res/library.hpp"
#include "res/material.hpp"
#include "res/mesh.hpp"
#include "res/texm.hpp"

namespace parkan {

struct ModelTexture {
    std::string name;
    std::uint32_t width = 0, height = 0;
    Bytes rgba; // level 0
};

struct ModelBatchDraw {
    std::uint32_t node = 0;
    std::uint32_t index_start = 0, index_count = 0;
    std::int32_t base_vertex = 0;
    std::int32_t texture = -1; // into ModelAsset::textures; -1 = untextured
    unsigned blend_category = 0;
    float opacity = 1;
};

struct ModelAsset {
    Mesh mesh;
    std::vector<Mat4> node_local; // static pose, relative to the parent node
    std::vector<Mat4> node_world; // static pose
    std::vector<ModelBatchDraw> draws;
    std::vector<ModelTexture> textures;
    Vec3 bounds_min{}, bounds_max{};
};

// Loads `mesh_entry` (e.g. "r_l_01.msh") from `archive` (e.g. "bases.rlb"); the WEAR with the same
// base name is taken from the same archive. `state`/`lod` pick each node's slot.
ModelAsset load_model(AssetLibrary& lib, std::string_view archive, std::string_view mesh_entry, int state = 0,
                      int lod = 0);

// One node's local transform before conversion to a matrix (source quaternion, not conjugated).
struct NodePose {
    Vec3 pos;
    Quat rot;
};

// AniMesh FUN_10012880: the node's pose at `time` (in frames). frame = round(time - 0.5) indexes the
// type-19 map from the node's anim_map_start; that key and the next are interpolated at `time` by
// their own times. The fallback key is used for a negative time, a frame past the frame count, a
// node without a map, or a mapped key not below the fallback key.
NodePose sample_node(const Mesh& mesh, std::size_t node, float time);

// AniMesh FUN_10012560: blend of two sampled times (Control's motion-state cross-fade). A side is
// used only when its weight is non-zero and its time is non-negative.
NodePose sample_node_blend(const Mesh& mesh, std::size_t node, float time_a, float time_b, float blend);

// Recomputes node_local/node_world with every node sampled at `time` (frames); a negative time gives
// the static pose. As in AniMesh (FUN_10008b30), a root node keeps its rotation but loses its
// translation: root motion moves the body through Control's mover instead. `attach`, when given,
// replaces the root as in attach_pose.
void pose_model(ModelAsset& model, float time, const Mat4* attach = nullptr);

// The same with every node cross-faded between two times (sample_node_blend; Control's tracks A/B).
void pose_model_blend(ModelAsset& model, float time_a, float time_b, float blend, const Mat4* attach = nullptr);

// Re-poses the model as a component attached to a parent's mesh node: like AniMesh, the model's
// root node is replaced by the parent node (`attach`, in the parent's space) and every other node
// keeps its local transform. Bounds are not updated.
void attach_pose(ModelAsset& model, const Mat4& attach);

} // namespace parkan
