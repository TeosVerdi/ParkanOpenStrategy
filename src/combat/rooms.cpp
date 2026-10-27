#include "combat/rooms.hpp"

#include "core/bytes.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::combat {

std::vector<HallwayVertex> decode_hallway(std::span<const std::uint8_t> msh) {
    std::vector<HallwayVertex> out;
    try {
        NResArchive a(Bytes(msh.begin(), msh.end()));
        const NResEntry* e = a.find_type(0x11);
        if (!e || std::size_t(e->attr1) * 20 > e->data.size()) return out;
        Reader r(e->data);
        out.resize(e->attr1);
        for (auto& v : out) {
            v.pos.x = r.f32(), v.pos.y = r.f32(), v.pos.z = r.f32();
            v.flags = r.u32();
            v.value = std::int32_t(r.u32());
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

std::vector<HallwayVertex> load_hallway(AssetLibrary& lib, std::string_view archive, std::string_view entry) {
    const NResEntry* e = lib.find(archive, entry);
    return e ? decode_hallway(e->data) : std::vector<HallwayVertex>{};
}

std::vector<Room> build_rooms(const std::vector<HallwayVertex>& vertices) {
    std::vector<Room> out;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const HallwayVertex& v = vertices[i];
        if (!(v.flags & 0xf8) && !(v.flags & 0xc600)) continue;
        Room r;
        r.vertex = std::uint32_t(i), r.pos = v.pos, r.flags = v.flags, r.value = v.value;
        r.speed_limit = (v.flags & 0x7c000) ? 1000.f : 2.f;
        if (v.flags & 0x10000000) r.radius = 10, r.height = 12;
        out.push_back(r);
    }
    return out;
}

bool room_contains(const Room& r, Vec3 room_world, Vec3 unit) {
    const float low = room_world.z - ((r.flags & 0x4040) ? 1.f : 0.7f) * r.height;
    if (unit.z < low || unit.z > room_world.z + r.height) return false;
    const float dx = unit.x - room_world.x, dy = unit.y - room_world.y;
    return dx * dx + dy * dy <= r.radius * r.radius;
}

std::int32_t recharged_ammo(std::int32_t ammo, std::int32_t max_ammo, float a) {
    if (max_ammo <= 0) return ammo;
    ammo += std::max<std::int32_t>(1, std::int32_t(std::lround(float(max_ammo) * a)));
    return std::min(ammo, max_ammo);
}

} // namespace parkan::combat
