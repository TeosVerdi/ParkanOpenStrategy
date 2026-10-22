// GPU-ready geometry shared by every drawable (models, terrain), independent of the graphics API.
#pragma once

#include "core/math.hpp"
#include "scene/model.hpp"

#include <optional>

namespace parkan {

struct Terrain;

struct RenderVertex {
    float pos[3];
    std::int8_t nrm[4];
    float uv0[2];
    float uv1[2];
    float blend; // weight of texture layer 1 over layer 0
    std::uint8_t color[4] = {255, 255, 255, 255}; // RGBA vertex colour (the sky gradient); white elsewhere
};

enum RenderDrawFlags : std::uint32_t {
    kDrawAlphaTest = 1,
    kDrawDetail = 2,      // tex1 at uv1 modulates the colour ×2 (Ngi shader mode 9, MODULATE2X)
    kDrawVertexAlpha = 4, // alpha = vertex blend, alpha-blended after the opaque draws (terrain overlay)
};

struct RenderDraw {
    std::uint32_t transform = 0;
    std::uint32_t index_start = 0, index_count = 0;
    std::int32_t base_vertex = 0;
    std::int32_t tex0 = -1, tex1 = -1; // into RenderMesh::textures, -1 = none
    std::uint32_t flags = 0;
};

struct RenderMesh {
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderDraw> draws;
    std::vector<ModelTexture> textures;
    std::vector<Mat4> transforms{Mat4::identity()};
    Vec3 bounds_min{}, bounds_max{};
};

RenderMesh to_render_mesh(ModelAsset model);

// Appends `src` to `dst` with every transform pre-multiplied by `world`; textures are shared by name.
// Returns the index of src's first transform in dst.
std::uint32_t append_render_mesh(RenderMesh& dst, const RenderMesh& src, const Mat4& world);

// A Material.lib (MAT0) material's texture from Textures.lib: phase `block`, or phase 0 when the
// material has fewer phases (CMatManager_GetMaterialPhase, World3D 100031f0). nullopt when missing.
std::optional<ModelTexture> load_material_texture(AssetLibrary& lib, const std::string& material, std::size_t block = 0);

// Builds LOD0 of a map's landscape: DATA/MAPS/<map>/Land.msh with Land1.wea / Land2.wea.
RenderMesh load_terrain_render_mesh(AssetLibrary& lib, std::string_view map_dir);

} // namespace parkan
