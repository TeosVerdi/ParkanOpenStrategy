#include "world/live_unit.hpp"

#include "res/ctld.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>

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

    for (std::size_t i = 1; i < u->model.parts.size() && !u->rig; ++i) {
        const UnitPart& p = u->model.parts[i];
        if (const NResEntry* te = lib.find(p.archive, p.ctl_entry)) {
            u->rig = TurretRig::from_ctld(decode_ctld(te->data));
            u->turret_part = i;
            if (const NResEntry* pe = lib.find(p.archive, p.cpt_entry)) u->turret_points = decode_ctpt(pe->data);
        }
    }

    if (u->rig)
        for (std::size_t i = 1; i < u->model.parts.size(); ++i) {
            if (i == u->turret_part) continue;
            bool under = false;
            for (int q = u->model.parts[i].parent; q >= 0 && !under; q = u->model.parts[std::size_t(q)].parent) under = std::size_t(q) == u->turret_part;
            const UnitPart& p = u->model.parts[i];
            const NResEntry* ge = under ? lib.find(p.archive, p.ctl_entry) : nullptr;
            if (!ge || ge->type != fourcc("CTLD")) continue;
            auto joints = decode_joints(decode_ctld(ge->data));
            if (std::any_of(joints.begin(), joints.end(), [](const CtldJoint& j) { return (j.flags & 8) != 0; }))
                u->gun_joints.push_back({i, std::move(joints)});
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
    if (!(motion->live().flags & 4)) {
        // Flyers (state flag 4 clear): FollowGround only keeps a minimum clearance above the ground
        // (its constant is not resolved; 1 m, as in tools/ai_tests.cpp).
        const Vec3 p = translation(f.matrix);
        auto g = ground.probe(p + Vec3{0, 0, 1000}, 2000.f);
        dz = g ? std::max(0.f, g->z + 1.f - p.z) : 0.f;
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
        for (const auto& [part, joints] : gun_joints) rig->gun_frames(joints, aim_y, model.parts[part].node_frames);
    }
    for (const auto& j : joint_frames) {
        if (j.part >= model.parts.size() || j.node < 0) continue;
        auto& frames = model.parts[j.part].node_frames;
        if (frames.size() <= std::size_t(j.node)) frames.resize(std::size_t(j.node) + 1, -1.f);
        frames[std::size_t(j.node)] = j.frame;
    }
    posed.resize(model.mesh.transforms.size());
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
