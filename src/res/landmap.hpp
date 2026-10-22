// Land.map: the ArealMap data (one NRes entry of type 12, attr1 = areal count). Loader: ArealMap.dll
// SystemArealMap::LoadLandMap (1001e0d0), per areal MAreal::BindFileRecord (10007640). Spec:
// notes/unit-ai-runtime.md §3; reference parser tools/landmap.py.
//
// Kept: every file field. Not modelled: the anchor jitter at load (an anchor outside its own polygon
// is moved at random until it is inside; the jitter amplitude is not traced), the polygon payload
// (`npoly` is 0 in every shipped map), subdivision cells (made when buildings are placed).
#pragma once

#include "core/bytes.hpp"
#include "core/math.hpp"

#include <array>
#include <optional>

namespace parkan {

struct LandAreal {
    Vec3 anchor{};       // a point inside the areal: its graph centre
    float f12 = 0;       // 0 in the shipped maps
    float area = 0;      // ≈ polygon area (m²)
    Vec3 normal{};
    std::uint32_t walkable = 0; // prefix+0x20: graph edges exist only between walkable areals
    std::uint32_t r36 = 0;
    std::uint32_t class_id = 0; // prefix+0x28 (meaning open)
    std::uint32_t r44 = 0;
    std::vector<Vec3> verts; // polygon, counter-clockwise in xy
    // Per polygon edge k (v_k → v_k+1), then 3 per extra polygon: (neighbour areal, its edge) or (−1, −1).
    std::vector<std::array<std::int32_t, 2>> links;
    std::uint32_t poly_count = 0;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0; // xy box (MAreal +0x14..+0x20)

    // Polygon2D::ContainsPoint (10017b90): even-odd crossing test in xy.
    bool contains(float x, float y) const;
};

struct LandMap {
    std::vector<LandAreal> areals;
    std::uint32_t cells_x = 0, cells_y = 0;
    // Grid in file order (x-major: cell (ix, iy) is ix·cells_y + iy): ids[first[c] .. first[c + 1]).
    std::vector<std::uint32_t> cell_first;
    std::vector<std::uint16_t> cell_ids;
    // Grid bounds (SystemArealMap +0x198..+0x1a8). The original takes them from the landscape bounding
    // box; decode_landmap starts with the areal vertex box, set_bounds replaces it.
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;

    void set_bounds(float x0, float y0, float x1, float y1) { min_x = x0, min_y = y0, max_x = x1, max_y = y1; }
    std::span<const std::uint16_t> cell(std::uint32_t ix, std::uint32_t iy) const;
    // Inside the grid box by more than 0.1 m (InsideMapBounds 100150c0).
    bool inside_bounds(float x, float y) const;
    // GetCoverArealID (10020370): the cell's single candidate as it is, else the first candidate whose
    // polygon contains the point; nullopt off the grid or when no candidate contains it.
    std::optional<std::uint32_t> areal_at(float x, float y) const;
};

LandMap decode_landmap(std::span<const std::uint8_t> nres_file);

} // namespace parkan
