#include "res/landmap.hpp"

#include "res/nres.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace parkan {

bool LandAreal::contains(float x, float y) const {
    bool in = false;
    const std::size_t n = verts.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec3 &a = verts[i], &b = verts[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

std::span<const std::uint16_t> LandMap::cell(std::uint32_t ix, std::uint32_t iy) const {
    const std::size_t c = std::size_t(ix) * cells_y + iy;
    return std::span<const std::uint16_t>(cell_ids).subspan(cell_first[c], cell_first[c + 1] - cell_first[c]);
}

bool LandMap::inside_bounds(float x, float y) const {
    return x > min_x + 0.1f && y > min_y + 0.1f && x < max_x - 0.1f && y < max_y - 0.1f;
}

std::optional<std::uint32_t> LandMap::areal_at(float x, float y) const {
    if (!cells_x || !cells_y || !inside_bounds(x, y)) return std::nullopt;
    // ix = round_even((x − minX)·cellsX/(maxX − minX) − 0.5), likewise y.
    const auto index = [](float v, float lo, float hi, std::uint32_t n) {
        const float f = std::nearbyint((v - lo) * float(n) / (hi - lo) - 0.5f);
        return std::uint32_t(std::clamp(f, 0.f, float(n - 1)));
    };
    const auto ids = cell(index(x, min_x, max_x, cells_x), index(y, min_y, max_y, cells_y));
    if (ids.size() == 1) return ids[0];
    for (std::uint16_t id : ids)
        if (id < areals.size() && areals[id].contains(x, y)) return id;
    return std::nullopt;
}

LandMap decode_landmap(std::span<const std::uint8_t> nres_file) {
    const NResArchive ar(Bytes(nres_file.begin(), nres_file.end()));
    const NResEntry* e = ar.find_type(12);
    if (!e) throw FormatError("Land.map has no ArealMap entry (type 12)");
    Reader r(e->data);
    LandMap m;
    m.areals.resize(e->attr1);
    float bx0 = FLT_MAX, by0 = FLT_MAX, bx1 = -FLT_MAX, by1 = -FLT_MAX;
    for (auto& a : m.areals) {
        a.anchor = {r.f32(), r.f32(), r.f32()};
        a.f12 = r.f32();
        a.area = r.f32();
        a.normal = {r.f32(), r.f32(), r.f32()};
        a.walkable = r.u32();
        a.r36 = r.u32();
        a.class_id = r.u32();
        a.r44 = r.u32();
        const std::uint32_t nv = r.u32();
        a.poly_count = r.u32();
        if (nv < 3 || std::size_t(nv) * 12 > r.remaining()) throw FormatError("bad areal vertex count");
        a.verts.resize(nv);
        a.min_x = a.min_y = FLT_MAX, a.max_x = a.max_y = -FLT_MAX;
        for (auto& v : a.verts) {
            v = {r.f32(), r.f32(), r.f32()};
            a.min_x = std::min(a.min_x, v.x), a.max_x = std::max(a.max_x, v.x);
            a.min_y = std::min(a.min_y, v.y), a.max_y = std::max(a.max_y, v.y);
        }
        bx0 = std::min(bx0, a.min_x), bx1 = std::max(bx1, a.max_x);
        by0 = std::min(by0, a.min_y), by1 = std::max(by1, a.max_y);
        const std::size_t nl = std::size_t(nv) + 3 * std::size_t(a.poly_count);
        if (nl * 8 > r.remaining()) throw FormatError("bad areal link count");
        a.links.resize(nl);
        for (auto& l : a.links) {
            l = {r.i32(), r.i32()};
            if (l[0] < -1 || l[0] >= std::int32_t(m.areals.size())) throw FormatError("areal link out of range");
        }
        for (std::uint32_t k = 0; k < a.poly_count; ++k) r.bytes(std::size_t(r.u32()) * 12);
    }
    m.cells_x = r.u32();
    m.cells_y = r.u32();
    const std::size_t cells = std::size_t(m.cells_x) * m.cells_y;
    if (cells * 2 > r.remaining()) throw FormatError("bad areal grid size");
    m.cell_first.reserve(cells + 1);
    for (std::size_t c = 0; c < cells; ++c) {
        m.cell_first.push_back(std::uint32_t(m.cell_ids.size()));
        for (std::uint16_t n = r.read<std::uint16_t>(); n > 0; --n) {
            const std::uint16_t id = r.read<std::uint16_t>();
            if (id >= m.areals.size()) throw FormatError("grid areal id out of range");
            m.cell_ids.push_back(id);
        }
    }
    m.cell_first.push_back(std::uint32_t(m.cell_ids.size()));
    if (!r.at_end()) throw FormatError("trailing bytes after the areal grid");
    m.set_bounds(bx0, by0, bx1, by1);
    return m;
}

} // namespace parkan
