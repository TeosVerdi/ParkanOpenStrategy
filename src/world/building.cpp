#include "world/building.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace parkan {

namespace {

constexpr float kDoorOpenMs = 5000.f;  // 100335d0: a door stays open ≥ 5000 ms; the capture door rests 5000 ms
constexpr float kCaptureXy = 0.75f;     // 10034d70: 0.75 · sphere radius in xy (10065634)
constexpr float kCaptureBelow = 4.f;    // 10034d70: the z band reaches 4 m below the sphere (10065264)
constexpr std::uint32_t kTypeTeleport = 0x80000200;

bool spheres_touch(Vec3 a, float ra, Vec3 b, float rb) {
    const Vec3 d = a - b;
    return dot(d, d) <= (ra + rb) * (ra + rb);
}

} // namespace

Building::Building(std::unique_ptr<LiveUnit> body, std::uint32_t type) : type_code(type), body_(std::move(body)) {
    joints_ = decode_joints(body_->ctld);
    // FUN_10033d00 (message 0x80000020 / 0x14): the items are the agent's devices of type 0xb, 0xc and
    // 0xd; doors start closing (SetState 2, state 2), the capture door closed and armed (state 0).
    for (std::size_t i = 0; i < body_->ctld.devices.size(); ++i) {
        const CtldDevice& d = body_->ctld.devices[i];
        if (d.type != 0xb && d.type != 0xc && d.type != 0xd) continue;
        BuildingItem it;
        it.device = std::uint32_t(i), it.type = d.type, it.joints = d.joints;
        std::memcpy(&it.speed, d.header.data() + 0x24, 4);
        if (!(it.speed > 0)) it.speed = 1;
        it.state = d.type == 0xc ? 2 : 0;
        if (d.type == 0xd) {
            if (capture_ >= 0) continue; // CBuilding keeps one capture record (+0x74)
            capture_ = std::int32_t(items_.size());
        }
        items_.push_back(std::move(it));
    }
    apply_frames();
    body_->pose_parts();
    rest_spheres_ = capture_spheres();
}

void Building::set_state(BuildingItem& it, int device_state) { it.target = device_state == 1 ? 1.f : 0.f; }

void Building::move(float dt_ms) {
    // The generic device (10020900): the channel moves toward its end and stops there (state 0). The
    // joint rate of the item's first joint paces it (ExtendMoveEndTime 10022120: |Δt|·1000/(speed·rate)).
    for (auto& it : items_) {
        if (!it.busy() || it.joints.empty() || it.joints[0] >= joints_.size()) {
            it.pos = it.target;
            continue;
        }
        const float rate = std::max(1e-3f, joints_[it.joints[0]].rate) * it.speed;
        const float step = rate * dt_ms * 0.001f;
        it.pos = it.target > it.pos ? std::min(it.target, it.pos + step) : std::max(it.target, it.pos - step);
    }
}

// The world box of mesh node `node` (AniMesh 1000f760, IJointMesh slot 4): the 8 corners of its slot box
// through the node's matrix (a node without geometry: its origin), merged (100119c0) with the boxes of
// its direct children unless the node has flag 0x10; a child with flag 4 ends the merge.
static void part_box(const LiveUnit& body, const Mesh& mesh, std::size_t base, std::size_t node, Vec3& lo, Vec3& hi) {
    auto add = [&](Vec3 p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    const Mat4& m = body.posed[base + node];
    const std::uint16_t si = mesh.nodes[node].slots[0];
    if (si == 0xFFFF || si >= mesh.slots.size()) {
        add(m.transform_point({}));
    } else {
        const MeshSlot& s = mesh.slots[si];
        for (int c = 0; c < 8; ++c)
            add(m.transform_point({(c & 1) ? s.aabb_max[0] : s.aabb_min[0], (c & 2) ? s.aabb_max[1] : s.aabb_min[1],
                                   (c & 4) ? s.aabb_max[2] : s.aabb_min[2]}));
    }
    if (mesh.nodes[node].flags & 0x10) return;
    for (std::size_t k = 0; k < mesh.nodes.size() && base + k < body.posed.size(); ++k) {
        if (mesh.nodes[k].parent != node) continue;
        if (mesh.nodes[k].flags & 4) return;
        part_box(body, mesh, base, k, lo, hi);
    }
}

std::vector<Building::Sphere> Building::capture_spheres() const {
    std::vector<Sphere> out;
    const BuildingItem* cd = capture_door();
    if (!cd || body_->model.parts.empty()) return out;
    const Mesh& mesh = body_->model.parts.front().model.mesh;
    const std::size_t base = body_->model.parts.front().transform_base;
    for (std::uint32_t j : cd->joints) {
        if (j >= joints_.size() || joints_[j].part < 0) continue;
        const std::size_t node = std::size_t(joints_[j].part);
        if (node >= mesh.nodes.size() || base + node >= body_->posed.size()) continue;
        // IJointMesh slot 3 (AniMesh 1000f3b0) with a part: the sphere around the part's box (its centre,
        // half the diagonal).
        Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        part_box(*body_, mesh, base, node, lo, hi);
        out.push_back({(lo + hi) * 0.5f, 0.5f * std::sqrt(dot(hi - lo, hi - lo))});
    }
    return out;
}

bool Building::in_capture_volume(Vec3 p, float r) const {
    for (const Sphere& s : rest_spheres_) {
        const float dx = p.x - s.centre.x, dy = p.y - s.centre.y, R = s.radius * kCaptureXy;
        if (dx * dx + dy * dy >= R * R) continue;
        if (type_code == kTypeTeleport) return true;
        if (s.centre.z - s.radius - r - kCaptureBelow < p.z && p.z < s.centre.z + s.radius) return true;
    }
    return false;
}

std::optional<std::int32_t> Building::update(float now, const std::vector<Contact>& contacts) {
    const float dt = last_ms_ < 0 ? 0.f : now - last_ms_;
    last_ms_ = now;
    move(dt);
    const Mesh* mesh = body_->model.parts.empty() ? nullptr : &body_->model.parts.front().model.mesh;
    const std::size_t base = body_->model.parts.empty() ? 0 : body_->model.parts.front().transform_base;
    const float scale = body_->placement ? body_->placement->scale[2] : 1.f;
    // Doors (items of type 0xc): the contact callback (10034f00 kind 1) opens a closed or closing door an
    // object touches and holds it; the tick (100335fd..10033692) closes it 5 s after it opened.
    for (auto& it : items_) {
        if (it.type != 0xc) continue;
        it.held = false;
        for (std::uint32_t j : it.joints) {
            if (!mesh || j >= joints_.size() || joints_[j].part < 0 || base + std::size_t(joints_[j].part) >= body_->posed.size()) continue;
            const std::size_t node = std::size_t(joints_[j].part);
            const std::uint16_t si = node < mesh->nodes.size() ? mesh->nodes[node].slots[0] : 0xFFFF;
            if (si == 0xFFFF || si >= mesh->slots.size()) continue;
            const MeshSlot& s = mesh->slots[si];
            const Vec3 c = body_->posed[base + node].transform_point({s.sphere_center[0], s.sphere_center[1], s.sphere_center[2]});
            for (const auto& k : contacts)
                if (k.alive && spheres_touch(c, s.sphere_radius * scale, k.centre, k.radius)) it.held = true;
        }
        if (it.held && (it.state == 2 || it.state == 0)) {
            set_state(it, 1);
            it.state = 3;
        }
        const bool busy = it.busy();
        if (it.state == 1) {
            if (now - it.since >= kDoorOpenMs && !it.held) {
                set_state(it, 2);
                it.state = 2;
            }
        } else if (it.state == 2) {
            if (!busy) it.state = 0;
        } else if (it.state == 3 && !busy) {
            it.state = 1;
            it.since = now;
        }
    }
    if (capture_ < 0) return std::nullopt;
    BuildingItem& cd = items_[std::size_t(capture_)];
    // Every contact inside the volume becomes the capturer (the last one wins); a closed door opens.
    for (const auto& k : contacts)
        if (in_capture_volume(k.centre, k.radius)) {
            capturer_ = k.id;
            if (cd.state == 0) {
                set_state(cd, 1);
                cd.state = 2;
            }
        }
    auto capturer_inside = [&] {
        for (const auto& k : contacts)
            if (k.id == capturer_) return k.alive && in_capture_volume(k.centre, k.radius);
        return false; // out of contact: not inside
    };
    auto close = [&] { // iid 0x17 slot 4 (10035a10)
        cd.state = 3;
        set_state(cd, 2);
    };
    const bool busy = cd.busy();
    switch (cd.state) {
    case 1:
        if (!capturer_inside()) close();
        break;
    case 2:
        if (busy) break;
        cd.state = 1;
        if (capturer_inside()) return capturer_; // (*building+0xb4)(building, capturer, 0)
        close();
        break;
    case 3:
        if (!busy) {
            cd.state = 4;
            cd.since = now;
        }
        break;
    case 4:
        if (now - cd.since >= kDoorOpenMs) cd.state = 0;
        break;
    default:
        break;
    }
    return std::nullopt;
}

bool Building::moving_transform(std::size_t t) const {
    if (body_->model.parts.empty()) return false;
    const UnitPart& root = body_->model.parts.front();
    if (t < root.transform_base || t - root.transform_base >= root.model.mesh.nodes.size()) return false;
    const auto& nodes = root.model.mesh.nodes;
    for (std::size_t n = t - root.transform_base;; n = nodes[n].parent) {
        for (const auto& it : items_)
            for (std::uint32_t j : it.joints)
                if (j < joints_.size() && joints_[j].part == std::int32_t(n)) return true;
        if (nodes[n].parent == 0xFFFF || nodes[n].parent >= nodes.size()) return false;
    }
}

void Building::apply_frames() {
    body_->device_frames.clear();
    for (const auto& it : items_)
        for (std::uint32_t j : it.joints) {
            if (j >= joints_.size()) continue;
            const CtldJoint& jt = joints_[j];
            if (jt.part < 0 || (jt.flags & 4)) continue; // flag 4: no mesh output
            // 10020900, no driver: t = pos / range + default t; wrapping joints (flag 1) wrap, others clamp.
            float t = jt.default_t + it.pos / (jt.range != 0 ? jt.range : 1.f);
            if (jt.flags & 1) {
                if (t > 1) t -= 1;
                if (t < 0) t += 1;
            } else {
                t = std::clamp(t, 0.f, 1.f);
            }
            if (jt.flags & 2) t = 1 - t; // invert
            body_->device_frames.push_back({0, jt.part, jt.frame_from + t * (jt.frame_to - jt.frame_from)});
        }
}

} // namespace parkan
