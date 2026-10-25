// MSH models: a nested NRes whose entries are typed streams. Spec: FParkan reference/msh, plus
// the static-pose details verified on the data (notes in docs/ARCHITECTURE.md).
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

struct MeshNode {
    std::uint16_t flags = 0;
    std::uint16_t parent = 0xFFFF;   // 0xFFFF = root; parents precede children
    std::uint16_t anim_map_start = 0;
    std::uint16_t fallback_key = 0;  // static pose
    std::array<std::uint16_t, 15> slots{}; // [state * 5 + lod], 0xFFFF = none
    std::string name;
};

struct MeshSlot {
    std::uint16_t tri_start = 0, tri_count = 0, batch_start = 0, batch_count = 0;
    std::array<float, 3> aabb_min{}, aabb_max{}, sphere_center{};
    float sphere_radius = 0;
    // +0x30 / +0x34: the part's surface area and volume (CAniMesh::GetPartProperty ids 0xf / 0x10, Control's mass
    // and armour sums; notes/unit-stats.md §3).
    float area = 0, volume = 0;
};

struct MeshBatch {
    std::uint16_t flags = 0;
    std::uint16_t material_hi = 0;
    std::uint8_t material = 0;        // WEAR row
    std::uint8_t lightmap = 0xFF;     // 0xFF = none
    std::uint16_t local_index = 0;
    std::uint16_t index_count = 0;
    std::uint32_t index_start = 0;
    std::uint16_t vertex_count = 0;
    std::uint32_t base_vertex = 0;
};

struct MeshKey {
    std::array<float, 3> pos{};
    float time = 0;
    std::array<float, 4> quat{}; // x, y, z, w (stored as int16 W, X, Y, Z / 32767)
};

struct Mesh {
    std::vector<MeshNode> nodes;
    std::vector<MeshSlot> slots;
    std::vector<MeshBatch> batches;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<std::int8_t, 4>> normals;
    std::vector<std::array<float, 2>> uv0, uv1;
    std::vector<std::uint16_t> indices;
    std::vector<MeshKey> keys;
    std::vector<std::uint16_t> frame_map; // type 19
    std::uint32_t frame_count = 0;        // type 19 attr2
    bool legacy_nodes = false;            // node stream in an unsupported old layout (dropped)
};

Mesh decode_mesh(std::span<const std::uint8_t> nres_payload);

// Stream readers shared with the terrain loader (Land.msh uses the same node/slot/vertex streams).
class NResArchive;
std::span<const std::uint8_t> mesh_stream(const NResArchive& a, std::uint32_t type, std::size_t rec_size, bool required);
std::vector<std::array<float, 3>> read_positions(const NResArchive& a);
std::vector<std::array<std::int8_t, 4>> read_normals(const NResArchive& a, std::size_t vertex_count);
std::vector<std::array<float, 2>> read_uv(const NResArchive& a, std::uint32_t type, std::size_t vertex_count, bool is_signed);
std::vector<MeshSlot> read_slots(const NResArchive& a);
std::vector<MeshNode> read_nodes(const NResArchive& a, std::size_t slot_count, bool& legacy);

} // namespace parkan
