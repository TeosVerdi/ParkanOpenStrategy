#include "world/live_unit.hpp"

#include "res/ctld.hpp"

#include <cfloat>

namespace parkan {

namespace {

Vec3 translation(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

} // namespace

std::unique_ptr<LiveUnit> LiveUnit::load(AssetLibrary& lib, const mission::TmaObject& o, const TerrainGround& ground) {
    auto model = load_unit_model(lib, o.resource);
    if (!model) return nullptr;
    const NResEntry* ce = lib.find(model->ctl_archive, model->ctl_entry);
    if (!ce) return nullptr;
    std::unique_ptr<LiveUnit> u(new LiveUnit);
    u->placement = &o;
    u->model = std::move(*model);
    const Ctld ctld = decode_ctld(ce->data);
    const std::string base = u->model.ctl_entry.substr(0, u->model.ctl_entry.rfind('.'));
    if (const NResEntry* pe = lib.find(u->model.ctl_archive, base + ".cpt")) u->points = decode_ctpt(pe->data);
    const ModelAsset& chassis = u->model.parts.front().model;
    u->motion = std::make_unique<UnitMotion>(ctld, chassis.mesh, u->points);
    u->sphere_centre = (chassis.bounds_min + chassis.bounds_max) * 0.5f;
    const Vec3 ext = chassis.bounds_max - chassis.bounds_min;
    u->sphere_radius = std::max(0.25f, 0.5f * std::max(ext.x, ext.y));

    for (std::size_t i = 1; i < u->model.parts.size() && !u->rig; ++i) {
        const UnitPart& p = u->model.parts[i];
        if (const NResEntry* te = lib.find(p.archive, p.ctl_entry)) {
            u->rig = TurretRig::from_ctld(decode_ctld(te->data));
            u->turret_part = i;
            if (const NResEntry* pe = lib.find(p.archive, p.cpt_entry)) u->turret_points = decode_ctpt(pe->data);
        }
    }

    Mat4 spawn = tma_placement(o);
    if (auto g = ground.probe(translation(spawn) + Vec3{0, 0, 50}, 100.f)) spawn.at(2, 3) = g->z;
    u->motion->start(0, spawn);
    u->frame = u->motion->interpolate(0);
    return u;
}

Vec3 LiveUnit::settle(float now_ms, const TerrainGround& ground, const StaticColliders& statics) {
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
        if (auto g = ground.probe(foot)) {
            dz = std::max(dz, g->z - foot.z);
            clearance.push_back({c, foot.z - g->z});
        }
    }
    if (dz == -FLT_MAX) {
        const Vec3 p = translation(f.matrix);
        auto g = ground.probe(p);
        dz = g ? g->z - p.z : 0.f;
    }
    motion->apply_offset({0, 0, dz});
    for (auto [c, gap] : clearance) motion->set_contact_touch(c, gap + dz < 0.1f);

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

void LiveUnit::pose(std::vector<Mat4>& transforms, float turret_x, float turret_y, bool hidden) {
    if (rig) rig->node_frames(turret_x, turret_y, frame.attitude_offset.z, model.parts[turret_part].node_frames);
    pose_unit(model, frame.matrix, frame.frame_a, frame.frame_b, frame.weight_b, transforms, first_transform);
    if (hidden) {
        const std::size_t end = std::min(transforms.size(), std::size_t(first_transform) + model.mesh.transforms.size());
        for (std::size_t i = first_transform; i < end; ++i) transforms[i] = Mat4{};
    }
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
