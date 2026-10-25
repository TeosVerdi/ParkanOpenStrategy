#include "world/live_unit.hpp"

#include "core/paths.hpp"
#include "mission/scheme.hpp"
#include "res/ctld.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace parkan {

namespace {

Vec3 translation(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

std::string record_field(std::span<const std::uint8_t> rec, std::size_t off) {
    if (off + 32 > rec.size()) return {};
    const auto* p = reinterpret_cast<const char*>(rec.data() + off);
    return std::string(p, ::strnlen(p, 32));
}

// The turret (the first component whose CTLD has a turret device) with its marker points, and the gun
// components under it whose joints follow its pitch (flag 8).
void find_turret(AssetLibrary& lib, LiveUnit& u) {
    for (std::size_t i = 1; i < u.model.parts.size() && !u.rig; ++i) {
        const UnitPart& p = u.model.parts[i];
        if (const NResEntry* te = lib.find(p.archive, p.ctl_entry)) {
            u.rig = TurretRig::from_ctld(decode_ctld(te->data));
            u.turret_part = i;
            if (const NResEntry* pe = lib.find(p.archive, p.cpt_entry)) u.turret_points = decode_ctpt(pe->data);
        }
    }
    if (!u.rig) return;
    for (std::size_t i = 1; i < u.model.parts.size(); ++i) {
        if (i == u.turret_part) continue;
        bool under = false;
        for (int q = u.model.parts[i].parent; q >= 0 && !under; q = u.model.parts[std::size_t(q)].parent) under = std::size_t(q) == u.turret_part;
        const UnitPart& p = u.model.parts[i];
        const NResEntry* ge = under ? lib.find(p.archive, p.ctl_entry) : nullptr;
        if (!ge || ge->type != fourcc("CTLD")) continue;
        auto joints = decode_joints(decode_ctld(ge->data));
        if (std::any_of(joints.begin(), joints.end(), [](const CtldJoint& j) { return (j.flags & 8) != 0; }))
            u.gun_joints.push_back({i, std::move(joints)});
    }
}

} // namespace

std::unique_ptr<LiveUnit> LiveUnit::load_building(AssetLibrary& lib, const mission::TmaObject& o, const mission::Scheme* given) {
    std::optional<mission::Scheme> read;
    if (!given) {
        auto path = resolve_path(lib.game_dir(), o.resource);
        if (!path) return nullptr;
        read = mission::decode_scheme(read_file(*path));
    }
    const mission::Scheme& scheme = given ? *given : *read;
    // FORT (128 bytes): {objects.rlb, agent}, {lib, .bas} (CBuilding ctor reads +0x00/+0x20 and +0x40/+0x60).
    const NResEntry* fort = lib.find("objects.rlb", scheme.root.entry);
    if (!fort || fort->type != fourcc("FORT")) return nullptr;
    const NResEntry* agent = lib.find("objects.rlb", record_field(fort->data, 0x20));
    if (!agent || agent->data.size() < 0x140) return nullptr;
    auto model = load_unit_model(lib, scheme);
    if (!model || model->parts.empty()) return nullptr;
    // The agent's pairs: 0 mesh, 1 WEAR, 2 CTPT, 3 NDPR, 4 CTLD (load_unit_model reads them only from
    // records of 0x140 bytes, so the FORT root has none).
    UnitPart& root = model->parts.front();
    root.archive = record_field(agent->data, 0x80), root.cpt_entry = record_field(agent->data, 0xa0);
    root.ndp_archive = record_field(agent->data, 0xc0), root.ndp_entry = record_field(agent->data, 0xe0);
    root.ctl_entry = record_field(agent->data, 0x120);
    model->ctl_archive = record_field(agent->data, 0x100), model->ctl_entry = root.ctl_entry;
    std::unique_ptr<LiveUnit> u(new LiveUnit);
    u->placement = &o;
    u->model = std::move(*model);
    if (const NResEntry* ce = lib.find(u->model.ctl_archive, u->model.ctl_entry); ce && ce->type == fourcc("CTLD")) u->ctld = decode_ctld(ce->data);
    if (const NResEntry* pe = lib.find(root.archive, root.cpt_entry)) u->points = decode_ctpt(pe->data);
    // The body sphere: the static pose's bounds (port: IVisual GetBoundingSphere is not decoded; the
    // sphere around the box is taken).
    const ModelAsset& base = u->model.parts.front().model;
    u->sphere_centre = (base.bounds_min + base.bounds_max) * 0.5f;
    const Vec3 ext = base.bounds_max - base.bounds_min;
    u->sphere_radius = std::max(0.25f, 0.5f * std::sqrt(dot(ext, ext)));
    u->frame.matrix = tma_placement(o); // buildings keep their TMA pose (PlaceBuilding fits the ground to them)
    find_turret(lib, *u);                // bunker / tower turrets (e_bnt_*), aimed by their fire control
    u->pose_parts();
    return u;
}

std::unique_ptr<LiveUnit> LiveUnit::load(AssetLibrary& lib, const mission::TmaObject& o, const TerrainGround& ground,
                                         const mission::Scheme* scheme, float start_ms) {
    auto model = scheme ? load_unit_model(lib, *scheme) : load_unit_model(lib, o.resource);
    if (!model) return nullptr;
    const NResEntry* ce = lib.find(model->ctl_archive, model->ctl_entry);
    if (!ce) return nullptr;
    std::unique_ptr<LiveUnit> u(new LiveUnit);
    u->placement = &o;
    u->model = std::move(*model);
    u->ctld = decode_ctld(ce->data);
    const Ctld& ctld = u->ctld;
    const std::string base = u->model.ctl_entry.substr(0, u->model.ctl_entry.rfind('.'));
    if (const NResEntry* pe = lib.find(u->model.ctl_archive, base + ".cpt")) u->points = decode_ctpt(pe->data);
    const ModelAsset& chassis = u->model.parts.front().model;
    u->motion = std::make_unique<UnitMotion>(ctld, chassis.mesh, u->points);
    // AniMesh's local matrix rotates by the conjugate of the key (CAniNode::SampleKey; scene/model).
    u->root_rest = Mat4::rotation(sample_node(chassis.mesh, 0, -1).rot.conjugate());
    u->sphere_centre = (chassis.bounds_min + chassis.bounds_max) * 0.5f;
    const Vec3 ext = chassis.bounds_max - chassis.bounds_min;
    u->sphere_radius = std::max(0.25f, 0.5f * std::max(ext.x, ext.y));

    find_turret(lib, *u);

    u->motion->start(start_ms, tma_placement(o));
    u->drop_to_ground(start_ms, ground);
    u->frame = u->motion->interpolate(start_ms);
    return u;
}

namespace {

// FindNearestFace10 along Z over the landscape and the objects (Terrain 10017ae0 → each object's
// GetWalkFace, CLandscape 10014b20): of the faces at or above (mode bit 4) or at or below (bit 8) p, the
// nearest in height.
std::optional<GroundHit> nearest_face(const TerrainGround& ground, const LiveUnit::FloorProbe& floor, Vec3 p, bool above) {
    std::optional<GroundHit> best = ground.probe_vertical(p, above);
    if (floor)
        if (auto f = floor(p, p + Vec3{0, 0, above ? 1000.f : -1000.f}); f && (!best || std::abs(f->z - p.z) < std::abs(best->z - p.z)))
            best = f;
    return best;
}

constexpr float kMinGroundNz = 0.173648f; // 1003c040: faces steeper than 80° are not ground
constexpr float kStepLimit = 7.5f;        // FollowGround's rise limit (1001a4a5)

// FollowGround's ground probe (1001a6c7..1001a7ef; the FindWorldFace walk from the cached face that
// precedes it is not ported): the nearest face above, if not too steep and less than the step limit
// higher; else the nearest face below, if not too steep; else the face above whatever it is.
std::optional<GroundHit> follow_probe(const TerrainGround& ground, const LiveUnit::FloorProbe& floor, Vec3 p) {
    const auto up = nearest_face(ground, floor, p, true);
    if (up && up->normal.z > kMinGroundNz && up->z - p.z < kStepLimit) return up;
    if (auto down = nearest_face(ground, floor, p, false); down && down->normal.z > kMinGroundNz) return down;
    return up;
}

} // namespace

void LiveUnit::drop_to_ground(float now_ms, const TerrainGround& ground, const FloorProbe& floor) {
    // CControlBase::DropToGround (10018800): the contact points of the current state (state 0 at load)
    // are probed and the body shifts by the summed height to their faces divided by the number of
    // contacts; a point without a face adds nothing (flags 0x30 / 0xc0 tilt and upright are not ported).
    // Without contact points the body origin is probed.
    // ProbeGroundFace (100195a0) over the landscape and the objects (FindNearestFace10 → each object's
    // GetWalkFace along Z, the nearest kept): the nearest face at or above the point (mode 6) when it is
    // less than the unit's sphere radius above it (ctl+0x24 slot 0x24: port, the collision sphere),
    // else the nearest face at or below it (mode 0xa).
    MotionFrame f = motion->interpolate(now_ms);
    ModelAsset& chassis = model.parts.front().model;
    pose_model_blend(chassis, f.frame_a, f.frame_b, f.weight_b);
    auto probe = [&](Vec3 p) -> std::optional<GroundHit> {
        if (auto up = nearest_face(ground, floor, p, true); up && up->z - p.z < sphere_radius) return up;
        return nearest_face(ground, floor, p, false);
    };
    std::vector<Vec3> points_w;
    for (const auto& c : motion->contacts()) {
        const auto pi = c.point;
        if (pi >= points.size() || points[pi].part >= chassis.node_world.size()) continue;
        points_w.push_back((f.matrix * chassis.node_world[points[pi].part]).transform_point(points[pi].pos));
    }
    if (points_w.empty()) points_w.push_back(translation(f.matrix));
    float sum = 0;
    for (const Vec3& p : points_w)
        if (auto g = probe(p)) sum += g->z - p.z;
    motion->apply_offset({0, 0, sum / float(points_w.size())});
}

void LiveUnit::place(float now_ms, const TerrainGround& ground, const FloorProbe& floor) {
    motion->start(now_ms, tma_placement(*placement));
    drop_to_ground(now_ms, ground, floor);
    frame = motion->interpolate(now_ms);
}

Vec3 LiveUnit::settle(float now_ms, const TerrainGround& ground, const StaticColliders& statics, const FloorProbe& floor) {
    auto support = [&](Vec3 p) -> std::optional<float> {
        auto g = follow_probe(ground, floor, p);
        return g ? std::optional<float>(g->z) : std::nullopt;
    };
    // FollowGround, reduced: the feet (the CTPT contact points of the motion graph) are probed and the
    // body is moved so the lowest support stands on the ground (dz = max(ground − foot)).
    MotionFrame f = motion->interpolate(now_ms);
    ModelAsset& chassis = model.parts.front().model;
    pose_model_blend(chassis, f.frame_a, f.frame_b, f.weight_b);
    float dz = -FLT_MAX;
    std::vector<std::pair<std::size_t, float>> clearance;
    for (std::size_t c = 0; c < motion->contacts().size(); ++c) {
        const auto pi = motion->contacts()[c].point;
        if (pi >= points.size() || points[pi].part >= chassis.node_world.size()) continue;
        const Vec3 foot = (f.matrix * chassis.node_world[points[pi].part]).transform_point(points[pi].pos);
        if (auto g = support(foot)) {
            dz = std::max(dz, *g - foot.z);
            clearance.push_back({c, foot.z - *g});
        }
    }
    if (dz == -FLT_MAX) {
        const Vec3 p = translation(f.matrix);
        auto g = support(p);
        dz = g ? *g - p.z : 0.f;
    }
    if (!(motion->live().flags & 4)) {
        // Flyers (state flag 4 clear; FollowGround 1001b3c3): the bounding sphere's centre is kept at
        // least its radius above the ground under it: dz = max(0, ground − centre.z + r). The radius
        // (ctl+0x24 slot 0x24, space 2) is clamped to 7.5 when it lies between 7.5 and 20 (1001a48a).
        // Port: the sphere is LiveUnit's collision sphere [inferred: the same sphere].
        const Vec3 p = f.matrix.transform_point(sphere_centre);
        const float r = sphere_radius > 7.5f && sphere_radius < 20.f ? 7.5f : sphere_radius;
        auto g = support(p);
        dz = g ? std::max(0.f, *g + r - p.z) : 0.f;
        clearance.clear();
    }
    motion->apply_offset({0, 0, dz});
    // Touch-downs: a constraint flagged 0x1000 says the foot ends this motion step on the ground. When
    // the previous step's constraint did not (the foot was lifted), the foot lands during this step and
    // its touch-down list fires once, when it reaches the ground (FollowGround 1001a450; the landing
    // test is inferred from the flags — the original's per-contact state is not decoded).
    touchdowns.clear();
    const std::size_t n = motion->contacts().size();
    landing_.resize(n, false);
    down_at_end_.resize(n, true);
    if (motion->step_start() != last_step_) {
        last_step_ = motion->step_start();
        for (std::size_t c = 0; c < n && c < motion->live().contacts.size(); ++c) {
            const bool down = (motion->live().contacts[c].flags & 0x1000) != 0;
            landing_[c] = down && !down_at_end_[c];
            down_at_end_[c] = down;
        }
    }
    for (auto [c, gap] : clearance) {
        const bool touch = gap + dz < 0.1f;
        motion->set_contact_touch(c, touch);
        if (touch && landing_[c] && c < motion->live().contacts.size()) {
            landing_[c] = false;
            const auto& k = motion->live().contacts[c];
            const auto pi = motion->contacts()[c].point;
            if (k.event_list >= 0)
                touchdowns.push_back({k.event_list, (f.matrix * chassis.node_world[points[pi].part]).transform_point(points[pi].pos) +
                                                        Vec3{0, 0, dz}});
        }
    }

    // CCollManager::Update: out of static walls, inside the landscape box; a ground unit's push
    // (state flag 4) is made horizontal (ApplyCollisionPush).
    const Vec3 centre = motion->interpolate(now_ms).matrix.transform_point(sphere_centre);
    Vec3 push = statics.push_sphere(centre, sphere_radius);
    push = push + contain_in_world_bounds(centre + push, sphere_radius, ground.bounds_min(), ground.bounds_max());
    if (motion->live().flags & 4) push = horizontal_push(push);
    if (dot(push, push) > 0) motion->apply_offset(push);
    frame = motion->interpolate(now_ms);
    return push;
}

std::optional<std::pair<std::string, std::string>> LiveUnit::touchdown_effect(std::int32_t list) const {
    if (list < 0 || std::size_t(list) >= ctld.event_lists.size()) return std::nullopt;
    const CtldEvent* start = nullptr;
    for (const auto& e : ctld.event_lists[std::size_t(list)])
        if (e.type == 10 && (!start || (e.flags & 0x10000000))) start = &e;
    if (!start) return std::nullopt;
    std::int32_t handle = 0;
    std::memcpy(&handle, start->raw.data() + 0x10, 4);
    const std::int32_t create = ctld.situations[0];
    if (create < 0 || std::size_t(create) >= ctld.event_lists.size()) return std::nullopt;
    for (const auto& e : ctld.event_lists[std::size_t(create)]) {
        std::int32_t h = 0;
        std::memcpy(&h, e.raw.data() + 0x1c, 4);
        if (e.type == 3 && h == handle && !e.entry.empty()) return std::pair{e.lib, e.entry};
    }
    return std::nullopt;
}

void LiveUnit::pose_parts() {
    if (rig) {
        rig->node_frames(aim_x, aim_y, frame.attitude_offset.z, model.parts[turret_part].node_frames);
        for (const auto& [part, joints] : gun_joints) rig->gun_frames(joints, aim_y + gun_aim_y, model.parts[part].node_frames);
    }
    for (const auto* list : {&joint_frames, &device_frames})
        for (const auto& j : *list) {
            if (j.part >= model.parts.size() || j.node < 0) continue;
            auto& frames = model.parts[j.part].node_frames;
            if (frames.size() <= std::size_t(j.node)) frames.resize(std::size_t(j.node) + 1, -1.f);
            frames[std::size_t(j.node)] = j.frame;
        }
    posed.resize(model.mesh.transforms.size());
    if (!motion) {
        // A building: every part at its node frames (doors; -1 = the static pose), at the placement.
        for (auto& part : model.parts) {
            if (part.parent < 0) pose_model_frames(part.model, part.node_frames);
            else pose_model_frames(part.model, part.node_frames, &model.parts[std::size_t(part.parent)].model.node_world[std::size_t(part.mount)]);
            for (std::size_t i = 0; i < part.model.node_world.size(); ++i)
                if (part.transform_base + i < posed.size()) posed[part.transform_base + i] = frame.matrix * part.model.node_world[i];
        }
        return;
    }
    pose_unit(model, frame.matrix, frame.frame_a, frame.frame_b, frame.weight_b, posed, 0, steady_mounts ? &root_rest : nullptr);
}

void LiveUnit::pose(std::vector<Mat4>& transforms, bool hidden) {
    pose_parts();
    for (std::size_t i = 0; i < posed.size() && first_transform + i < transforms.size(); ++i)
        transforms[first_transform + i] = hidden || (i < detached.size() && detached[i]) ? Mat4{} : posed[i];
}

void collide_units(std::vector<LiveUnit*>& units, float now_ms) {
    // Positional form of SweptSphereTOI + PushApart: units that overlap at the end of the tick are
    // separated along the line of centres (the original's narrow phase on faces is not decoded).
    for (std::size_t i = 0; i < units.size(); ++i)
        for (std::size_t j = i + 1; j < units.size(); ++j) {
            LiveUnit &a = *units[i], &b = *units[j];
            const Vec3 ca = a.sphere_world(), cb = b.sphere_world();
            Vec3 d = cb - ca;
            const float dist = std::sqrt(dot(d, d)), r = a.sphere_radius + b.sphere_radius;
            if (dist >= r) continue;
            const Vec3 n = dist > 1e-4f ? d * (1 / dist) : Vec3{1, 0, 0};
            auto [pa, pb] = split_push(n * (r - dist), a.mass, b.mass);
            if (a.motion->live().flags & 4) pa = horizontal_push(pa);
            if (b.motion->live().flags & 4) pb = horizontal_push(pb);
            a.motion->apply_offset(pa);
            b.motion->apply_offset(pb);
            a.frame = a.motion->interpolate(now_ms);
            b.frame = b.motion->interpolate(now_ms);
        }
}

} // namespace parkan
