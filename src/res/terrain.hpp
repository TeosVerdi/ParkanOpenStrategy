// Landscape geometry: DATA/MAPS/<map>/Land.msh. An MSH-like NRes with the stream order
// 1, 2, 3, 4, 5, 18, 14, 11, 21 (FParkan tomes/04-world, "Land.msh"). Native: Terrain.dll CLandscape.
#pragma once

#include "res/mesh.hpp"

namespace parkan {

struct TerrainFace {
    std::uint32_t flags = 0;          // 0x400 = detail texture on
    std::uint16_t material = 0xFFFF;  // low byte: base row, high byte: overlay row (0xff = none); both
                                      // index Land1.wea (texture) and Land2.wea (detail), terrain-render.md
    std::array<std::uint16_t, 3> v{};       // vertices
    std::array<std::uint16_t, 3> neighbor{}; // faces across each edge, 0xFFFF = none
    std::array<std::int16_t, 3> normal{};   // face normal / 32767 (inferred from the data)
    std::uint16_t cls = 0;                  // +0x1a (meaning open)
};

// Draw-list entry (CShade_DrawMeshFaces 10028a50). Flag 0x10 starts a run of faces with one material.
struct TerrainDrawEntry {
    std::uint16_t face = 0;
    std::uint8_t flags = 0;
    std::uint8_t pad = 0;
};

struct Terrain {
    std::vector<MeshNode> nodes; // slot[0] = LOD0 face range, slot[1] = LOD1
    std::vector<MeshSlot> slots; // tri_start/tri_count index `draw_list` (not `faces`)
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<std::int8_t, 4>> normals;
    std::vector<std::array<float, 2>> uv_map;    // type 5: base/overlay UV, planar (x / 256, (Ymax - y) / 256)
    std::vector<std::array<float, 2>> uv_detail; // type 18: "microtexture" (detail) UV, u16 / 1024
    std::vector<float> blend;                    // type 14: overlay alpha per vertex
    std::vector<TerrainDrawEntry> draw_list;     // type 11: what slot ranges index
    std::vector<TerrainFace> faces;              // type 21
};

Terrain decode_terrain(std::span<const std::uint8_t> nres_file);

} // namespace parkan
