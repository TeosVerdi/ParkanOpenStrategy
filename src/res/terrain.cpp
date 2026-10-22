#include "res/terrain.hpp"

#include "res/nres.hpp"

namespace parkan {

Terrain decode_terrain(std::span<const std::uint8_t> file) {
    NResArchive a(Bytes(file.begin(), file.end()));
    Terrain t;
    t.positions = read_positions(a);
    const std::size_t nv = t.positions.size();
    t.normals = read_normals(a, nv);
    t.uv_map = read_uv(a, 5, nv, false);
    t.uv_detail = read_uv(a, 18, nv, false);
    {
        auto d = mesh_stream(a, 14, 4, true);
        t.blend.resize(d.size() / 4);
        std::memcpy(t.blend.data(), d.data(), d.size());
        if (t.blend.size() != nv) throw FormatError("blend count differs from vertex count");
    }
    {
        auto d = mesh_stream(a, 21, 28, true);
        Reader r(d);
        t.faces.resize(d.size() / 28);
        for (auto& f : t.faces) {
            f.flags = r.u32();
            f.material = r.read<std::uint16_t>();
            r.read<std::uint16_t>(); // always 0xFFFF in the shipped maps
            for (auto& v : f.v) {
                v = r.read<std::uint16_t>();
                if (v >= nv) throw FormatError("terrain face vertex out of range");
            }
            for (auto& n : f.neighbor) n = r.read<std::uint16_t>();
            for (auto& n : f.normal) n = r.read<std::int16_t>();
            f.cls = r.read<std::uint16_t>();
        }
        for (const auto& f : t.faces)
            for (auto n : f.neighbor)
                if (n != 0xFFFF && n >= t.faces.size()) throw FormatError("terrain neighbour out of range");
    }
    {
        auto d = mesh_stream(a, 11, 4, true);
        t.draw_list.resize(d.size() / 4);
        std::memcpy(t.draw_list.data(), d.data(), d.size());
        for (const auto& e : t.draw_list)
            if (e.face >= t.faces.size()) throw FormatError("terrain draw list names a missing face");
    }
    t.slots = read_slots(a);
    for (const auto& s : t.slots)
        if (std::size_t(s.tri_start) + s.tri_count > t.draw_list.size()) throw FormatError("terrain slot outside draw list");
    bool legacy = false;
    t.nodes = read_nodes(a, t.slots.size(), legacy);
    if (legacy || t.nodes.empty()) throw FormatError("terrain without nodes");
    return t;
}

} // namespace parkan
