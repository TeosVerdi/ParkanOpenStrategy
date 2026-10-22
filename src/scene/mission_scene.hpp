// A whole mission as one drawable: terrain plus every placed object in its static pose.
// Object resolution: TMA resource → objects.rlb prototype (STAT/BTLU/INTO/EXTO/BULL/WPNS: first pair is
// the mesh, second the WEAR; FORT: first pair names another prototype) or a unit .dat (its first
// scheme: the base prototype plus its turrets and weapons on their mount nodes).
#pragma once

#include "mission/tma.hpp"
#include "scene/render_mesh.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace parkan {

struct MissionSceneStats {
    std::size_t objects = 0, placed = 0, unresolved = 0;
    std::size_t components = 0, bad_components = 0; // external parts on unit schemes (per distinct design)
};

// One unit design (.dat scheme, game-relative path such as "UNITS/AUTO/swlklas.dat") in model space;
// nullopt when the file or its base prototype is missing. Adds to stats->components/bad_components.
std::optional<RenderMesh> load_scheme_render_mesh(AssetLibrary& lib, std::string_view scheme_path,
                                                  MissionSceneStats* stats = nullptr);

// A unit design as separately posable parts, for units that move: the chassis first, then its
// external components in scheme order, each attached to a node of its parent part.
struct UnitPart {
    ModelAsset model;
    int parent = -1;  // index into UnitModel::parts; -1 for the chassis
    int mount = -1;   // node of the parent part
    std::uint32_t transform_base = 0; // first transform of this part in UnitModel::mesh
    std::string archive, ctl_entry, cpt_entry; // the prototype's motion graph / marker points (may be empty)
    std::vector<float> node_frames;  // components: keyframe time per node (-1 = static), e.g. turret joints
};
struct UnitModel {
    std::vector<UnitPart> parts;
    RenderMesh mesh;                      // every part; transforms[part base + node]
    std::string ctl_archive, ctl_entry;   // the chassis prototype's motion graph (pair 4), if any
};
std::optional<UnitModel> load_unit_model(AssetLibrary& lib, std::string_view scheme_path);

// Poses the chassis at the two animation times (cross-faded by `blend`; root translation removed),
// re-attaches the components (each posed by its node_frames) and writes world · node matrices to
// `transforms[first + part base + node]`.
void pose_unit(UnitModel& unit, const Mat4& world, float time_a, float time_b, float blend,
               std::vector<Mat4>& transforms, std::size_t first);

// `mission_dir` is game-relative, e.g. "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02".
// Objects for which `skip(index)` is true are left out (the units that move are drawn separately).
RenderMesh load_mission_render_mesh(AssetLibrary& lib, std::string_view mission_dir, MissionSceneStats* stats = nullptr,
                                    const std::function<bool(std::size_t)>& skip = {});

// A placed object's world matrix: T · Rz·Ry·Rx · S (iron3d 0x36610).
Mat4 tma_placement(const mission::TmaObject& o);

} // namespace parkan
