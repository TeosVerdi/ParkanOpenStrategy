#include "res/mesh.hpp"

#include "res/nres.hpp"

namespace parkan {

namespace {

enum : std::uint32_t {
    kNodes = 1, kSlots = 2, kPositions = 3, kNormals = 4, kUv0 = 5, kIndices = 6, kKeys = 8,
    kNames = 10, kBatches = 13, kUv1 = 18, kFrameMap = 19
};

std::span<const std::uint8_t> stream(const NResArchive& a, std::uint32_t type, std::size_t rec_size,
                                     bool required = true) {
    const NResEntry* e = a.find_type(type);
    if (!e) {
        if (required) throw FormatError("mesh lacks stream " + std::to_string(type));
        return {};
    }
    if (e->data.size() % rec_size) throw FormatError("mesh stream " + std::to_string(type) + " size mismatch");
    return e->data;
}

} // namespace

std::span<const std::uint8_t> mesh_stream(const NResArchive& a, std::uint32_t type, std::size_t rec_size, bool required) {
    return stream(a, type, rec_size, required);
}

std::vector<std::array<float, 3>> read_positions(const NResArchive& a) {
    auto d = stream(a, kPositions, 12);
    Reader r(d);
    std::vector<std::array<float, 3>> out(d.size() / 12);
    for (auto& p : out) p = {r.f32(), r.f32(), r.f32()};
    return out;
}

std::vector<std::array<std::int8_t, 4>> read_normals(const NResArchive& a, std::size_t vertex_count) {
    auto d = stream(a, kNormals, 4, false);
    std::vector<std::array<std::int8_t, 4>> out(d.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), d.data(), d.size());
    if (!out.empty() && out.size() != vertex_count) throw FormatError("normal count differs from vertex count");
    return out;
}

std::vector<std::array<float, 2>> read_uv(const NResArchive& a, std::uint32_t type, std::size_t vertex_count,
                                          bool is_signed) {
    auto d = stream(a, type, 4, false);
    Reader r(d);
    std::vector<std::array<float, 2>> out(d.size() / 4);
    for (auto& t : out) {
        if (is_signed) t = {r.read<std::int16_t>() / 1024.f, r.read<std::int16_t>() / 1024.f};
        else t = {r.read<std::uint16_t>() / 1024.f, r.read<std::uint16_t>() / 1024.f};
    }
    if (!out.empty() && out.size() != vertex_count) throw FormatError("UV count differs from vertex count");
    return out;
}

std::vector<MeshSlot> read_slots(const NResArchive& a) {
    const NResEntry* e = a.find_type(kSlots);
    if (!e || e->data.size() < 0x8C || (e->data.size() - 0x8C) % 68) throw FormatError("bad slot table");
    Reader r(e->data.subspan(0x8C));
    std::vector<MeshSlot> out((e->data.size() - 0x8C) / 68);
    for (auto& s : out) {
        s.tri_start = r.read<std::uint16_t>();
        s.tri_count = r.read<std::uint16_t>();
        s.batch_start = r.read<std::uint16_t>();
        s.batch_count = r.read<std::uint16_t>();
        for (auto* v : {&s.aabb_min, &s.aabb_max, &s.sphere_center}) *v = {r.f32(), r.f32(), r.f32()};
        s.sphere_radius = r.f32();
        r.bytes(20);
    }
    return out;
}

std::vector<MeshNode> read_nodes(const NResArchive& a, std::size_t slot_count, bool& legacy) {
    legacy = false;
    const NResEntry* e = a.find_type(kNodes);
    if (!e) return {};
    // Node records are 38 bytes (15 slots = 3 states x 5 LODs). One developer test mesh
    // (system.rlb MTCHECK.MSH) uses an older 24-byte layout that does not match; its nodes are dropped.
    const std::size_t rs = e->attr3;
    legacy = rs != 38;
    if (legacy) return {};
    if (e->data.size() % rs) throw FormatError("bad node stream");
    Reader r(e->data);
    std::vector<MeshNode> out(e->data.size() / rs);
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto& n = out[i];
        n.flags = r.read<std::uint16_t>();
        n.parent = r.read<std::uint16_t>();
        n.anim_map_start = r.read<std::uint16_t>();
        n.fallback_key = r.read<std::uint16_t>();
        for (auto& s : n.slots) {
            s = r.read<std::uint16_t>();
            if (s != 0xFFFF && s >= slot_count) throw FormatError("node refers to a missing slot");
        }
        if (n.parent != 0xFFFF && n.parent >= i) throw FormatError("node parent after child");
    }
    return out;
}

Mesh decode_mesh(std::span<const std::uint8_t> payload) {
    NResArchive a(Bytes(payload.begin(), payload.end()));
    Mesh m;

    m.positions = read_positions(a);
    const std::size_t nv = m.positions.size();
    m.normals = read_normals(a, nv);
    m.uv0 = read_uv(a, kUv0, nv, false);
    m.uv1 = read_uv(a, kUv1, nv, false);
    {
        auto d = stream(a, kIndices, 2);
        m.indices.resize(d.size() / 2);
        std::memcpy(m.indices.data(), d.data(), d.size());
    }
    {
        auto d = stream(a, kBatches, 20);
        Reader r(d);
        m.batches.resize(d.size() / 20);
        for (auto& b : m.batches) {
            b.flags = r.read<std::uint16_t>();
            b.material_hi = r.read<std::uint16_t>();
            b.material = r.u8();
            b.lightmap = r.u8();
            b.local_index = r.read<std::uint16_t>();
            b.index_count = r.read<std::uint16_t>();
            b.index_start = r.u32();
            b.vertex_count = r.read<std::uint16_t>();
            b.base_vertex = r.u32();
            if (std::size_t(b.index_start) + b.index_count > m.indices.size()) throw FormatError("batch outside index buffer");
            for (std::size_t i = 0; i < b.index_count; ++i)
                if (std::size_t(b.base_vertex) + m.indices[b.index_start + i] >= nv)
                    throw FormatError("batch index outside vertex buffer");
        }
    }
    m.slots = read_slots(a);
    for (const auto& sl : m.slots)
        if (std::size_t(sl.batch_start) + sl.batch_count > m.batches.size()) throw FormatError("slot outside batch table");
    m.nodes = read_nodes(a, m.slots.size(), m.legacy_nodes);
    if (auto d = stream(a, kKeys, 24, false); !d.empty()) {
        Reader r(d);
        m.keys.resize(d.size() / 24);
        for (auto& k : m.keys) {
            k.pos = {r.f32(), r.f32(), r.f32()};
            k.time = r.f32();
            // Stored W, X, Y, Z (an identity key reads 32767, 0, 0, 0).
            const float w = r.read<std::int16_t>() / 32767.f;
            const float x = r.read<std::int16_t>() / 32767.f;
            const float y = r.read<std::int16_t>() / 32767.f;
            const float z = r.read<std::int16_t>() / 32767.f;
            k.quat = {x, y, z, w};
        }
        for (const auto& n : m.nodes)
            if (n.fallback_key >= m.keys.size()) throw FormatError("node fallback key out of range");
    }
    if (const NResEntry* e = a.find_type(kFrameMap)) {
        if (e->data.size() % 2) throw FormatError("bad frame map");
        m.frame_map.resize(e->data.size() / 2);
        std::memcpy(m.frame_map.data(), e->data.data(), e->data.size());
        m.frame_count = e->attr2;
    }
    if (const NResEntry* e = a.find_type(kNames)) {
        // NUL-separated node names, one per node (best effort; the stream is also used for other strings).
        std::size_t i = 0, start = 0;
        for (std::size_t p = 0; p < e->data.size() && i < m.nodes.size(); ++p)
            if (e->data[p] == 0) {
                m.nodes[i++].name.assign(reinterpret_cast<const char*>(e->data.data()) + start, p - start);
                start = p + 1;
            }
    }
    return m;
}

} // namespace parkan
