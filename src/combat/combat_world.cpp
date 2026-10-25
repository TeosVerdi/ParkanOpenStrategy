#include "combat/combat_world.hpp"

#include "core/paths.hpp"
#include "fx/effect.hpp"
#include "mission/scheme.hpp"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace parkan::combat {

namespace {

constexpr float kGravity = 10.f;        // IWorld::GetGravity default (control-physics.md)
constexpr float kLateralDamping = 3.f;  // CBulletControl::Update: v_x, v_z → 0 at 3 m/s² (body[21] = 0)
constexpr float kSightNear = 5.f;       // ConvergeOnSight: the sight ray starts 5 m out
constexpr float kSightFar = 4000.f;     // … and runs to 1e6 m (port: 4 km, past every map)
constexpr float kSightMin = 100.f;      // 100424b4: a hit nearer than this aims at the 100 m point
constexpr float kSlowTickMs = 250.f;    // the slow damage tick (10042480 = 250 ms, jitter not modelled)

std::string record_field(std::span<const std::uint8_t> rec, std::size_t off) {
    const auto* p = reinterpret_cast<const char*>(rec.data() + off);
    return std::string(p, ::strnlen(p, 32));
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

Vec3 rotate(const Mat4& m, Vec3 v) { return m.transform_point(v) - m.transform_point({}); }

bool segment_hits_sphere(Vec3 a, Vec3 b, Vec3 c, float r) {
    const Vec3 d = b - a;
    const float len2 = dot(d, d);
    const float t = len2 > 0 ? std::clamp(dot(c - a, d) / len2, 0.f, 1.f) : 0.f;
    const Vec3 q = a + d * t - c;
    return dot(q, q) <= r * r;
}

} // namespace

// ---------------------------------------------------------------------------------------- Combatant

void Combatant::select(std::int32_t index) {
    if (index == -1) {
        for (std::size_t i = 0; i < guns.size(); ++i)
            if (!selected[i]) {
                selected[i] = true;
                guns[i].set_state(kGunReset);
            }
        return;
    }
    if (index < 1 || std::size_t(index) > guns.size()) return; // no such component: the binding does nothing
    const std::size_t i = std::size_t(index - 1);
    selected[i] = !selected[i];
    guns[i].set_state(selected[i] ? kGunReset : kCeaseFire);
}

void Combatant::set_state_selected(std::uint32_t state) {
    for (std::size_t i = 0; i < guns.size(); ++i)
        if (selected[i]) guns[i].set_state(state);
}

void Combatant::set_gun_state(std::size_t gun, std::uint32_t state) {
    if (gun < guns.size()) guns[gun].set_state(state);
}

void Combatant::fire_mask(std::uint32_t mask) {
    for (std::size_t i = 0; i < guns.size() && i < 32; ++i)
        if (mask & (1u << i)) guns[i].set_state(kFireSingle);
}

Vec3 Combatant::centre() const { return unit->frame.matrix.transform_point(bound_centre_); }

Vec3 Combatant::aim_centre() const {
    const std::size_t r = damage.root;
    if (r < damage.parts.size() && r < unit->posed.size() && damage.parts[r].sphere_radius > 0)
        return unit->posed[r].transform_point(damage.parts[r].sphere_centre);
    return centre();
}

// GetContactPoint(point, space 2): CTPT point `index` of `points` (part `part`'s) through its posed node.
static bool part_point(const Combatant& c, std::size_t part, const std::vector<ContactPoint>& points, std::int32_t index, Vec3& pos,
                       Vec3* dir) {
    if (index < 0 || std::size_t(index) >= points.size() || part >= c.unit->model.parts.size()) return false;
    const ContactPoint& p = points[std::size_t(index)];
    const std::size_t t = c.unit->model.parts[part].transform_base + p.part;
    if (t >= c.unit->posed.size()) return false;
    const Mat4& mat = c.unit->posed[t];
    pos = mat.transform_point(p.pos);
    if (dir) *dir = normalize(rotate(mat, p.vec));
    return true;
}

static bool mount_point(const Combatant& c, const GunMount& m, std::int32_t index, Vec3& pos, Vec3* dir) {
    return part_point(c, m.part, m.points, index, pos, dir);
}

// The sight ray of a turret gun: origin (yaw joint point A) and direction (pitch joint point B).
static bool sight(const Combatant& c, const GunMount& m, Vec3& origin, Vec3& dir) {
    Vec3 unused;
    return m.on_turret() && part_point(c, m.sight_part, m.sight_points, m.sight_origin, origin, &unused) &&
           part_point(c, m.sight_part, m.sight_points, m.sight_dir, unused, &dir);
}

// ------------------------------------------------------------------------------------ the gun host

class CombatWorld::Host final : public GunHost {
public:
    Host(CombatWorld& w, std::size_t unit, std::size_t gun) : w_(w), u_(unit), g_(gun) {}

    bool muzzle(std::uint32_t joint, Vec3& pos, Vec3& dir) override {
        const Combatant& c = *w_.units_[u_];
        const GunMount& m = c.mounts[g_];
        const auto& joints = joint_table();
        if (joint >= joints.size()) return false;
        return point(c, m, joints[joint].point_b, pos, &dir);
    }
    bool broken(std::int32_t part) override {
        const Combatant& c = *w_.units_[u_];
        if (part < 0) return false;
        const std::size_t t = c.unit->model.parts[c.mounts[g_].part].transform_base + std::size_t(part);
        if (t >= c.damage.parts.size()) return false;
        return c.damage.parts[c.damage.resolve(t)].destroyed();
    }
    std::optional<Vec3> target_position() override {
        const Combatant& c = *w_.units_[u_];
        if (c.target < 0 || std::size_t(c.target) >= w_.units_.size()) return std::nullopt;
        const Combatant& t = *w_.units_[std::size_t(c.target)];
        if (t.removed || !t.alive()) return std::nullopt; // Track: the target must exist and be alive
        return t.aim_centre();
    }
    void spawn(GunDevice&, Vec3 pos, Vec3 dir) override {
        Combatant& c = *w_.units_[u_];
        const GunMount& m = c.mounts[g_];
        const BulletProto* b = c.guns[g_].bullet();
        if (c.aim_point) {
            dir = normalize(*c.aim_point - pos);
        } else if (b && b->physics == 0 && m.on_turret()) {
            // ConvergeOnSight (1002a610): the sight ray from the yaw joint's point A along the pitch joint's
            // point B; on a hit, the bullet heads for it (or for the 100 m point when it is nearer).
            Vec3 o, sd;
            if (sight(c, m, o, sd)) {
                if (auto h = w_.trace(o + sd * kSightNear, o + sd * kSightFar, std::int32_t(u_))) {
                    Vec3 p = h->point;
                    const Vec3 d = p - o;
                    if (dot(d, d) < kSightMin * kSightMin) p = o + sd * kSightMin;
                    dir = normalize(p - pos);
                }
            }
        }
        w_.spawn(u_, g_, pos, dir);
    }

private:
    const std::vector<CtldJoint>& joint_table() { return w_.units_[u_]->guns[g_].joints(); }
    static bool point(const Combatant& c, const GunMount& m, std::int32_t index, Vec3& pos, Vec3* dir) {
        return mount_point(c, m, index, pos, dir);
    }
    CombatWorld& w_;
    std::size_t u_, g_;
};

// -------------------------------------------------------------------------------------- the world

CombatWorld::CombatWorld(AssetLibrary& lib, const TerrainGround& ground, const TraceMesh* statics)
    : lib_(lib), ground_(ground), statics_(statics) {}

const BulletProto* CombatWorld::bullet(const ResName& name) {
    const std::string key = lower(name.second);
    auto it = protos_.find(key);
    if (it != protos_.end()) return it->second.get();
    try {
        auto p = std::make_unique<BulletProto>(load_bullet(lib_, name.second));
        return protos_.emplace(key, std::move(p)).first->second.get();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "combat: bullet %s: %s\n", name.second.c_str(), ex.what());
        return nullptr;
    }
}

std::size_t CombatWorld::add_unit(LiveUnit& u, std::uint32_t logic_id, std::uint32_t clan, bool player) {
    auto c = std::make_unique<Combatant>();
    c->unit = &u;
    c->logic_id = logic_id;
    DamageState& d = c->damage;
    d.clan = clan;
    d.player = player;
    d.removal_delay_ms = std::bit_cast<std::int32_t>(u.ctld.body[18]);
    const float scale = u.placement ? u.placement->scale[2] : 1.f;
    UnitModel& model = u.model;

    // Damage parts: one per transform of the unit mesh (part base + node). A component's NDPR record 0 is
    // skipped and its root merged into the mount node (LoadControlData, formats-wea-cpt-ndp.md).
    d.parts.resize(model.mesh.transforms.size());
    d.root = model.parts.empty() ? 0 : model.parts[0].transform_base; // RenderMesh transform 0 is a spare identity
    for (std::size_t pi = 0; pi < model.parts.size(); ++pi) {
        const UnitPart& part = model.parts[pi];
        std::vector<NdprPart> ndpr;
        if (const NResEntry* e = lib_.find(part.ndp_archive, part.ndp_entry)) {
            try {
                ndpr = decode_ndpr(e->data);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "combat: %s: %s\n", part.ndp_entry.c_str(), ex.what());
            }
        }
        const Mesh& mesh = part.model.mesh;
        for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
            const std::size_t t = part.transform_base + n;
            if (t >= d.parts.size()) break;
            DamagePart& p = d.parts[t];
            if (pi > 0 && n == 0) {
                const UnitPart& parent = model.parts[std::size_t(part.parent)];
                p.alias = std::int32_t(parent.transform_base + std::size_t(part.mount));
                continue;
            }
            const MeshNode& node = mesh.nodes[n];
            p.present = true;
            if (node.parent != 0xFFFF) p.parent = std::int32_t(d.resolve(part.transform_base + node.parent));
            else if (pi > 0) p.parent = std::int32_t(d.resolve(model.parts[std::size_t(part.parent)].transform_base + std::size_t(part.mount)));
            // Runtime flags from the mesh node (IAniMesh property 0xe).
            if (node.flags & 0x200) p.flags |= 1;
            if (node.flags & 0x100) p.flags |= 2;
            if (node.flags & 0x400) p.flags |= 4;
            if ((node.flags & 1) && !(node.flags & 0x80)) p.flags |= 8;
            std::uint32_t states = 0;
            for (int s = 0; s < 3; ++s) states += node.slots[std::size_t(s) * 5] != 0xFFFF;
            p.stages = std::max(1u, states > 0 ? states - 1 : 0u);
            if (node.slots[0] != 0xFFFF && node.slots[0] < mesh.slots.size()) {
                const MeshSlot& sl = mesh.slots[node.slots[0]];
                p.sphere_centre = {sl.sphere_center[0], sl.sphere_center[1], sl.sphere_center[2]};
                p.sphere_radius = sl.sphere_radius * scale;
            }
            if (n < ndpr.size()) {
                const NdprPart& r = ndpr[n];
                p.ndpr_flags = r.flags;
                p.max_hp = r.hit_points * d.mult * scale;
                if (r.has_effect()) {
                    const std::string key = lower(r.effect.first + "/" + r.effect.second);
                    auto [it, fresh] = expl_cache_.try_emplace(key);
                    if (fresh) {
                        if (const NResEntry* e = lib_.find(r.effect.first, r.effect.second); e && e->type == fourcc("EXPL")) it->second = decode_expl(e->data);
                    }
                    if (it->second) {
                        for (std::size_t k = 0; k < d.effects.size() && p.effect < 0; ++k)
                            if (lower(d.effects[k].first.second) == lower(r.effect.second)) p.effect = std::int32_t(k);
                        if (p.effect < 0) {
                            p.effect = std::int32_t(d.effects.size());
                            d.effects.push_back({r.effect, *it->second});
                        }
                    }
                }
            }
            p.hp = p.prev_hp = p.max_hp;
        }
    }
    d.finish_init();

    // Armour: the scheme's device 0x1b (base i_arm_* or an armour module); the last one created wins.
    if (u.placement) {
        if (auto path = resolve_path(lib_.game_dir(), u.placement->resource)) {
            try {
                const mission::Scheme scheme = mission::decode_scheme(read_file(*path));
                auto walk = [&](auto&& self, const mission::SchemeNode& n) -> void {
                    if (const NResEntry* e = lib_.find("objects.rlb", n.entry); e && e->data.size() >= 0x140) {
                        if (const NResEntry* ce = lib_.find(record_field(e->data, 0x100), record_field(e->data, 0x120));
                            ce && ce->type == fourcc("CTLD")) {
                            for (const auto& dev : decode_ctld(ce->data).devices)
                                if (dev.type == 0x1b) {
                                    float prm[3];
                                    std::memcpy(prm, dev.header.data() + 0x2c, sizeof prm);
                                    d.armour = Armour{prm[1], prm[2], prm[0]};
                                }
                        }
                    }
                    for (const auto& ch : n.children) self(self, ch);
                };
                walk(walk, scheme.root);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "combat: armour of %s: %s\n", u.placement->resource.c_str(), ex.what());
            }
        }
    }

    // Guns: every weapon device of every part's CTLD.
    for (std::size_t pi = 0; pi < model.parts.size(); ++pi) {
        const UnitPart& part = model.parts[pi];
        const std::string archive = pi == 0 && !model.ctl_archive.empty() ? model.ctl_archive : part.archive;
        const std::string entry = pi == 0 && !model.ctl_entry.empty() ? model.ctl_entry : part.ctl_entry;
        const NResEntry* ce = entry.empty() ? nullptr : lib_.find(archive, entry);
        if (!ce || ce->type != fourcc("CTLD")) continue;
        const Ctld ctld = decode_ctld(ce->data);
        bool any = false;
        for (const auto& dev : ctld.devices) any |= is_weapon_device(dev);
        if (!any) continue;
        GunMount mount;
        mount.part = pi;
        if (const NResEntry* pe = lib_.find(part.archive, part.cpt_entry)) mount.points = decode_ctpt(pe->data);
        if (auto rig = TurretRig::from_ctld(ctld)) {
            mount.sight_origin = rig->sight_origin_point, mount.sight_dir = rig->sight_dir_point;
            mount.sight_part = pi, mount.sight_points = mount.points;
        } else if (u.rig && u.rig->sight_origin_point >= 0) {
            // A gun component mounted on the turret (o_gun_* on o_tur_*) takes the turret's sight.
            for (int q = int(pi); q >= 0; q = model.parts[std::size_t(q)].parent)
                if (std::size_t(q) == u.turret_part) {
                    mount.sight_origin = u.rig->sight_origin_point, mount.sight_dir = u.rig->sight_dir_point;
                    mount.sight_part = u.turret_part, mount.sight_points = u.turret_points;
                    break;
                }
        }
        const auto joints = decode_joints(ctld);
        for (const auto& dev : ctld.devices) {
            if (!is_weapon_device(dev)) continue;
            GunDeviceSpec spec = decode_gun_device(dev);
            const BulletProto* b = spec.bullet.second.empty() ? nullptr : bullet(spec.bullet);
            c->guns.emplace_back(std::move(spec), joints, b);
            c->mounts.push_back(mount);
        }
    }
    // SetComponentControl (World3D 1000ed00) selects each manual gun, except, for the hero, guns whose
    // device record lacks bit 4 (unresolved). Port: every AI gun selected; the hero starts with gun 1.
    c->selected.assign(c->guns.size(), !player);
    if (player && !c->selected.empty()) c->selected[0] = true;

    // Bounding sphere for the hit test: the static pose's vertices around the body sphere centre, with a
    // margin for animation.
    c->bound_centre_ = u.sphere_centre;
    float r2 = 0;
    for (const auto& dr : model.mesh.draws) {
        const Mat4& m = model.mesh.transforms[dr.transform];
        for (std::uint32_t i = 0; i < dr.index_count; ++i) {
            const auto& v = model.mesh.vertices[std::size_t(dr.base_vertex + std::int32_t(model.mesh.indices[dr.index_start + i]))];
            const Vec3 q = m.transform_point({v.pos[0], v.pos[1], v.pos[2]}) - u.sphere_centre;
            r2 = std::max(r2, dot(q, q));
        }
    }
    c->bound_radius_ = std::max(0.5f, std::sqrt(r2) * 1.25f);
    units_.push_back(std::move(c));
    return units_.size() - 1;
}

std::int32_t CombatWorld::sight_target(std::size_t u, float range) const {
    const Combatant& c = *units_[u];
    for (const auto& m : c.mounts) {
        Vec3 o, sd;
        if (!sight(c, m, o, sd)) continue;
        const auto h = trace(o + sd * kSightNear, o + sd * range, std::int32_t(u));
        return h && h->kind == WorldHit::Kind::Unit ? h->unit : -1;
    }
    return -1;
}

std::optional<Vec3> CombatWorld::muzzle(std::size_t u, std::size_t g) const {
    const Combatant& c = *units_[u];
    if (g >= c.guns.size() || c.guns[g].barrel_count() == 0) return std::nullopt;
    const auto& joints = c.guns[g].joints();
    const std::uint32_t j = c.guns[g].barrel_joint(c.guns[g].barrel());
    Vec3 pos, dir;
    if (j >= joints.size() || !mount_point(c, c.mounts[g], joints[j].point_b, pos, &dir)) return std::nullopt;
    return pos;
}

float CombatWorld::aim_error(std::size_t u, std::size_t g, Vec3 aim) const {
    const Combatant& c = *units_[u];
    if (g >= c.guns.size() || c.guns[g].barrel_count() == 0) return 3.1415927f;
    const auto& joints = c.guns[g].joints();
    const std::uint32_t j = c.guns[g].barrel_joint(c.guns[g].barrel());
    Vec3 pos, dir;
    if (j >= joints.size() || !mount_point(c, c.mounts[g], joints[j].point_b, pos, &dir)) return 3.1415927f;
    const Vec3 to = normalize(aim - pos);
    return std::acos(std::clamp(dot(dir, to), -1.f, 1.f));
}

Mat4 CombatWorld::bullet_matrix(Vec3 pos, Vec3 dir) {
    const Vec3 y = normalize(dir);
    Vec3 x = cross(y, Vec3{0, 0, 1});
    if (dot(x, x) < 1e-8f) x = {1, 0, 0};
    x = normalize(x);
    const Vec3 z = cross(x, y);
    Mat4 m = Mat4::identity();
    m.at(0, 0) = x.x, m.at(1, 0) = x.y, m.at(2, 0) = x.z;
    m.at(0, 1) = y.x, m.at(1, 1) = y.y, m.at(2, 1) = y.z;
    m.at(0, 2) = z.x, m.at(1, 2) = z.y, m.at(2, 2) = z.z;
    m.at(0, 3) = pos.x, m.at(1, 3) = pos.y, m.at(2, 3) = pos.z;
    return m;
}

void CombatWorld::spawn(std::size_t u, std::size_t g, Vec3 pos, Vec3 dir) {
    Combatant& c = *units_[u];
    GunDevice& gun = c.guns[g];
    const BulletProto* p = gun.bullet();
    if (!p) return;
    Bullet b;
    b.id = next_bullet_++;
    b.proto = p;
    b.shooter = std::int32_t(u);
    b.gun = std::int32_t(g);
    b.pos = pos;
    b.dir = normalize(dir);
    // v = R(M)·(0, speed, 0) + shooter world velocity (ctl+0x200).
    b.vel = b.dir * p->speed + (c.unit->motion ? c.unit->motion->world_velocity() : Vec3{}); // buildings stand still
    b.range_left = p->range;
    b.mult = gun.damage_mult;
    bullets_.push_back(b);
    ++shots;
    events_.push_back({CombatEvent::Type::Shot, std::int32_t(u), std::int32_t(g), 0, pos, {}});
    for (std::size_t i = 0; i < p->spawn_effects.size(); ++i) {
        const auto& sp = p->spawn_points[i];
        events_.push_back({CombatEvent::Type::Effect, std::int32_t(u), -1, 0, pos, p->spawn_effects[i], b.id, b.dir, 1,
                           fx::contact_frame(sp.pos, sp.vec), p->end_actions[i], float(p->removal_ms)});
    }
}

std::optional<WorldHit> CombatWorld::trace_floor(Vec3 a, Vec3 b) const { return trace(a, b, -1, false); }

std::optional<WorldHit> CombatWorld::trace(Vec3 a, Vec3 b, std::int32_t skip) const { return trace(a, b, skip, true); }

std::optional<WorldHit> CombatWorld::trace(Vec3 a, Vec3 b, std::int32_t skip, bool everything) const {
    std::optional<WorldHit> best;
    if (everything)
        if (auto h = ground_.trace(a, b)) best = WorldHit{WorldHit::Kind::Terrain, h->t, h->point, h->normal};
    if (statics_)
        if (auto h = statics_->trace(a, b); h && (!best || h->t < best->t))
            best = WorldHit{WorldHit::Kind::Static, h->t, h->point, h->normal};
    for (std::size_t i = 0; i < units_.size(); ++i) {
        const Combatant& c = *units_[i];
        if (std::int32_t(i) == skip || c.removed || !c.alive() || c.unit->posed.empty() || (!everything && !c.building())) continue;
        if (!segment_hits_sphere(a, b, c.centre(), c.bound_radius_)) continue;
        const RenderMesh& mesh = c.unit->model.mesh;
        if (c.building()) c.draw_cache_.resize(mesh.draws.size());
        for (std::size_t di = 0; di < mesh.draws.size(); ++di) {
            const RenderDraw& d = mesh.draws[di];
            if (d.transform >= c.unit->posed.size()) continue;
            const std::size_t part = c.damage.resolve(d.transform);
            if (part < c.damage.parts.size() && c.damage.parts[part].destroyed()) continue; // thrown off as debris
            const Mat4& m = c.unit->posed[d.transform];
            if (c.building()) {
                Combatant::DrawCache& dc = c.draw_cache_[di];
                if (dc.radius < 0 || std::memcmp(&dc.matrix, &m, sizeof m) != 0) {
                    dc.matrix = m;
                    dc.verts.clear();
                    Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                    for (std::uint32_t k = 0; k + 2 < d.index_count; k += 3)
                        for (std::uint32_t j = 0; j < 3; ++j) {
                            const auto& rv = mesh.vertices[std::size_t(d.base_vertex + std::int32_t(mesh.indices[d.index_start + k + j]))];
                            const Vec3 w = m.transform_point({rv.pos[0], rv.pos[1], rv.pos[2]});
                            dc.verts.push_back(w);
                            lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                            hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                        }
                    dc.centre = (lo + hi) * 0.5f;
                    dc.radius = dc.verts.empty() ? 0.f : 0.5f * std::sqrt(dot(hi - lo, hi - lo));
                }
                if (dc.verts.empty() || !segment_hits_sphere(a, b, dc.centre, dc.radius)) continue;
                for (std::size_t k = 0; k + 2 < dc.verts.size(); k += 3) {
                    const Vec3* v = &dc.verts[k];
                    const auto t = segment_triangle(a, b, v[0], v[1], v[2]);
                    if (!t || (best && *t >= best->t)) continue;
                    Vec3 n = normalize(cross(v[1] - v[0], v[2] - v[0]));
                    if (dot(n, b - a) > 0) n = n * -1.f;
                    best = WorldHit{WorldHit::Kind::Unit, *t, a + (b - a) * *t, n, std::int32_t(i), std::int32_t(part)};
                }
                continue;
            }
            for (std::uint32_t k = 0; k + 2 < d.index_count; k += 3) {
                Vec3 v[3];
                for (int j = 0; j < 3; ++j) {
                    const auto& rv = mesh.vertices[std::size_t(d.base_vertex + std::int32_t(mesh.indices[d.index_start + k + std::uint32_t(j)]))];
                    v[j] = m.transform_point({rv.pos[0], rv.pos[1], rv.pos[2]});
                }
                const auto t = segment_triangle(a, b, v[0], v[1], v[2]);
                if (!t || (best && *t >= best->t)) continue;
                Vec3 n = normalize(cross(v[1] - v[0], v[2] - v[0]));
                if (dot(n, b - a) > 0) n = n * -1.f;
                best = WorldHit{WorldHit::Kind::Unit, *t, a + (b - a) * *t, n, std::int32_t(i), std::int32_t(part)};
            }
        }
    }
    return best;
}

void CombatWorld::fly(Bullet& b, float dt_ms, float now) {
    const float dt = dt_ms * 0.001f;
    const BulletProto& p = *b.proto;
    if (p.physics == 3) {
        b.vel.z -= kGravity * dt; // ballistic: gravity on v
        b.dir = normalize(b.vel);
    } else if (p.physics == 0) {
        // Lateral damping: the velocity across the bullet's axis decays at 3 m/s².
        const Vec3 along = b.dir * dot(b.vel, b.dir), side = b.vel - along;
        const float s = std::sqrt(dot(side, side));
        b.vel = along + (s > kLateralDamping * dt ? side * ((s - kLateralDamping * dt) / s) : Vec3{});
    }
    Vec3 step = b.vel * dt;
    float len = std::sqrt(dot(step, step));
    bool exhausted = false;
    if (len >= b.range_left) { // OnRangeExhausted (1000d390): clamped to the end point
        if (len > 0) step = step * (b.range_left / len);
        len = b.range_left;
        exhausted = true;
    }
    const Vec3 from = b.pos, to = b.pos + step;
    b.range_left -= len;
    if (auto h = trace(from, to, b.shooter)) {
        // OnCollision (1000d0c0) → situation 2: SelfExplode, part 0's EXPL.
        b.pos = h->point;
        b.alive = false;
        events_.push_back({CombatEvent::Type::BulletEnded, -1, b.shooter, 2, h->point, {}, b.id});
        if (p.expl) explode(b, *p.expl, h->point, h->unit, h->part);
        return;
    }
    b.pos = to;
    const Vec3 lo = ground_.bounds_min(), hi = ground_.bounds_max();
    if (to.x < lo.x || to.y < lo.y || to.x > hi.x || to.y > hi.y || to.z > hi.z + 20.f || to.z < lo.z - 50.f) {
        b.alive = false; // BulletExitWorldBounds → situation 3: removed without an explosion
        events_.push_back({CombatEvent::Type::BulletEnded, -1, b.shooter, 3, to, {}, b.id});
        return;
    }
    if (exhausted) {
        b.alive = false; // situation 4: the substitute explosion (event 0x1b), else nothing
        events_.push_back({CombatEvent::Type::BulletEnded, -1, b.shooter, 4, to, {}, b.id});
        if (p.range_expl) explode(b, *p.range_expl, to, -1, -1);
    }
    (void)now;
}

void CombatWorld::explode(const Bullet& b, const Expl& e, Vec3 pos, std::int32_t target, std::int32_t part) {
    ++hits;
    events_.push_back({CombatEvent::Type::Hit, target, b.shooter, 0, pos, {}});
    if (!e.fx[0].second.empty()) events_.push_back({CombatEvent::Type::Effect, target, b.shooter, 0, pos, e.fx[0], 0, b.dir, 1});
    DamageRecord r;
    r.kind = e.kind;
    r.target = target, r.target_part = part;
    r.attacker = b.shooter;
    r.pos = pos;
    // mult·E.damage + the bullet's part-0 HP (hitPoints × mult; 0 when the attacker is gone).
    const bool attacker_present = b.shooter >= 0 && !units_[std::size_t(b.shooter)]->removed;
    r.damage = attacker_present ? b.mult * e.damage + b.mult * b.proto->hit_points() : 0.f;
    r.radius = e.radius; // class 9: raw
    records_.push_back(r);
}

Vec3 CombatWorld::part_centre(const Combatant& c, std::size_t part, float& radius) const {
    const DamagePart& p = c.damage.parts[part];
    radius = p.sphere_radius;
    if (part >= c.unit->posed.size()) return c.centre();
    return c.unit->posed[part].transform_point(p.sphere_centre);
}

void CombatWorld::note_death(std::size_t u, bool was_killed, float now) {
    Combatant& c = *units_[u];
    if (!was_killed && c.damage.killed) events_.push_back({CombatEvent::Type::Killed, std::int32_t(u), -1, now, c.centre(), {}});
}

void CombatWorld::apply_record(std::size_t v, const DamageRecord& rec, float now) {
    Combatant& c = *units_[v];
    DamageState& d = c.damage;
    const float before = d.total;
    const bool was_killed = d.killed;
    // OnDamageRecord (1000ebc0): the victim's Behaviour hears of the attacker first.
    if (rec.kind == 2) {
        if (rec.target_part < 0) return; // a shield-only hit (no shields here)
        if (!d.invulnerable && std::int32_t(v) != rec.attacker) {
            const std::size_t part = d.resolve(std::size_t(rec.target_part));
            float r;
            const Vec3 pc = part_centre(c, part, r);
            d.damage_part_from_record(part, std::int32_t(v), rec, pc, r, now);
            // A destroyed part is thrown off with the parts under it (DetachPartDebris; port reading:
            // they are destroyed with it).
            if (d.parts[part].destroyed()) d.propagate_destruction(now);
            d.update_part_stage(part, true);
        }
    } else if (rec.kind == 3 || rec.kind == 4) {
        if (!d.invulnerable && std::int32_t(v) != rec.attacker && rec.damage != 0) {
            for (std::size_t i = 0; i < d.parts.size(); ++i) {
                if (!d.parts[i].present) continue;
                float r;
                const Vec3 pc = part_centre(c, i, r);
                d.damage_part_from_record(i, std::int32_t(v), rec, pc, r, now);
            }
            d.propagate_destruction(now);
            for (std::size_t i = 0; i < d.parts.size(); ++i) d.update_part_stage(i, true);
        }
    } else {
        return;
    }
    // OnDamageMsg goes out even for the attacker's own splash; the port drops that one (no damage, and
    // a unit registering itself as its threat helps nobody).
    if (std::int32_t(v) != rec.attacker) events_.push_back({CombatEvent::Type::Damaged, std::int32_t(v), rec.attacker, before - d.total, rec.pos, {}});
    note_death(v, was_killed, now);
}

void CombatWorld::drain_explosions(std::size_t u) {
    Combatant& c = *units_[u];
    auto list = std::move(c.damage.explosions);
    c.damage.explosions.clear();
    for (const auto& x : list) {
        const auto& [name, e] = c.damage.effects[std::size_t(x.effect)];
        float pr;
        const Vec3 pos = part_centre(c, std::size_t(x.part), pr);
        if (!e.fx[0].second.empty())
            events_.push_back({CombatEvent::Type::Effect, std::int32_t(u), -1, 0, pos, e.fx[0], 0, {0, 1, 0}, std::max(pr, 0.5f)});
        if (e.kind == 1) continue; // effect only
        DamageRecord r;
        r.kind = e.kind;
        r.source = std::int32_t(u), r.source_part = x.part;
        r.attacker = std::int32_t(u);
        r.pos = pos;
        r.damage = c.removed ? 0.f : x.damage;
        r.radius = pr * e.radius; // other classes: part radius × EXPL radius
        records_.push_back(r);
    }
}

void CombatWorld::process_records(float now) {
    // ProcessDamageQueue (10012ce0), one record at a time; part explosions caused by a record queue
    // further records (bounded: each part explodes once per stage).
    for (std::size_t guard = 0; guard < 64; ++guard) {
        for (std::size_t u = 0; u < units_.size(); ++u) drain_explosions(u);
        if (records_.empty()) return;
        auto recs = std::move(records_);
        records_.clear();
        for (const auto& rec : recs) {
            if (rec.damage <= 0) continue;
            if (rec.kind == 2) {
                if (rec.target >= 0 && !units_[std::size_t(rec.target)]->removed) apply_record(std::size_t(rec.target), rec, now);
            } else if (rec.kind == 3 || rec.kind == 4) {
                for (std::size_t v = 0; v < units_.size(); ++v) {
                    const Combatant& c = *units_[v];
                    if (std::int32_t(v) == rec.source || c.removed || !c.alive()) continue;
                    const Vec3 d = c.centre() - rec.pos;
                    if (dot(d, d) > (rec.radius + c.bound_radius_) * (rec.radius + c.bound_radius_)) continue;
                    apply_record(v, rec, now);
                }
            }
        }
    }
}

void CombatWorld::update(float now) {
    const float dt = now - last_ms_;
    last_ms_ = now;
    // Guns (UpdateDevices); the barrel channels feed the next pose.
    for (std::size_t u = 0; u < units_.size(); ++u) {
        Combatant& c = *units_[u];
        if (c.removed) continue;
        c.unit->joint_frames.clear();
        for (std::size_t g = 0; g < c.guns.size(); ++g) {
            if (c.alive()) {
                Host host(*this, u, g);
                c.guns[g].update(now, host);
            }
            for (std::size_t b = 0; b < c.guns[g].barrel_count(); ++b)
                if (auto f = c.guns[g].barrel_frame(b, now)) c.unit->joint_frames.push_back({c.mounts[g].part, f->first, f->second});
        }
    }
    // Bullets.
    for (std::size_t i = 0; i < bullets_.size(); ++i) {
        Bullet& b = bullets_[i];
        if (b.fresh) {
            b.fresh = false;
            continue;
        }
        if (b.alive && dt > 0) fly(b, dt, now);
    }
    std::erase_if(bullets_, [](const Bullet& b) { return !b.alive; });
    process_records(now);
    // Slow damage tick and removal.
    for (std::size_t u = 0; u < units_.size(); ++u) {
        Combatant& c = *units_[u];
        if (c.removed) continue;
        if (now >= c.next_slow_tick_) {
            const bool was = c.damage.killed;
            c.damage.slow_tick(kSlowTickMs * 0.001f, now);
            c.next_slow_tick_ = now + kSlowTickMs;
            note_death(u, was, now);
        }
        if (c.damage.removal_at && now >= *c.damage.removal_at) {
            c.removed = true;
            events_.push_back({CombatEvent::Type::Removed, std::int32_t(u), -1, now, c.centre(), {}});
        }
    }
    process_records(now);
    // Destroyed parts leave the model (DetachPartDebris; the debris flight is not modelled). A part's
    // children follow it (propagate_destruction); a component root goes with its mount.
    for (auto& up : units_) {
        Combatant& c = *up;
        auto& det = c.unit->detached;
        det.resize(c.damage.parts.size(), false);
        for (std::size_t t = 0; t < det.size(); ++t) {
            const std::size_t p = c.damage.resolve(t);
            det[t] = p != c.damage.root && p < c.damage.parts.size() && c.damage.parts[p].destroyed();
        }
    }
}

} // namespace parkan::combat
