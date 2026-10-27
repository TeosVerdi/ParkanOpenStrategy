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

EnergyHost Combatant::energy_host() const {
    EnergyHost host;
    host.part_ratio = [this](std::int32_t t) { return part_ratio(t); };
    return host;
}

void Combatant::recharge(float a, float now_ms) {
    // 0x204 GetProperty(1) → SetProperty(1, min(1, f + a)); a unit without a battery has no property.
    if (const auto f = energy.battery_fraction()) energy.set_battery_fraction(std::min(1.f, *f + a));
    // Room_RechargeLifeAndShield: life property 0x31 → ApplyDamageTotal(maxHP·v − hp) (CControlBase::SetProperty).
    if (damage.initial > 0 && !damage.killed) {
        const float v = std::min(1.f, damage.hp_ratio() + a);
        const float amount = damage.initial * v - damage.total;
        if (amount != 0) damage.apply_damage_total(amount, true, false, now_ms);
    }
    if (const auto m = energy.shield_mean()) energy.set_shield_mean(std::min(1.f, *m + a));
    // Room_RechargeGuns: every gun device of type 2 (not the building guns, 0x1e).
    for (auto& g : guns) {
        if (g.spec().type != 2) continue;
        if (const auto n = std::int32_t(std::lround(g.param(0))); n > 0) g.set_ammo(recharged_ammo(g.ammo(), n, a));
        g.set_energy_ratio(std::min(1.f, g.energy_ratio() + a));
    }
}

std::optional<Combatant::Signature> Combatant::signature() const {
    if (!mass) return std::nullopt;
    const EnergyHost host = energy_host();
    return Signature{mass->area * energy.drain_a * energy.signature_factor(1, host),
                     mass->volume * energy.drain_b * energy.signature_factor(2, host),
                     mass->mass * energy.signature_factor(0, host)};
}

bool Combatant::radar_detects(float d, const Signature& s) const {
    if (energy.radar < 0) return false;
    const PowerDevice& r = energy.devices[std::size_t(energy.radar)];
    const EnergyHost host = energy_host();
    const float range = r.param[3];
    if (r.state == 0 || part_ratio(r.part) == 0 || !(d < range)) return false;
    const float f = 1 - d * d / (range * range);
    return EnergyBudget::get_param(r, 0x300, host) * s.p45 * f >= 1 || EnergyBudget::get_param(r, 0x301, host) * s.p43 * f >= 1 ||
           EnergyBudget::get_param(r, 0x302, host) * s.p44 * f >= 1;
}

std::optional<float> Combatant::radar_param(std::size_t i) const {
    if (energy.radar < 0 || i >= 16) return std::nullopt;
    return energy.devices[std::size_t(energy.radar)].param[i];
}

bool Combatant::gun_broken(std::size_t g) const {
    for (const auto& d : energy.devices)
        if (d.gun == std::int32_t(g)) return d.part >= 0 && part_ratio(d.part) == 0;
    return false;
}

std::optional<std::uint32_t> Combatant::device_state(std::uint32_t type) const {
    for (const auto& d : energy.devices)
        if (d.type == type) return d.state;
    return std::nullopt;
}

float Combatant::part_ratio(std::int32_t t) const {
    if (t < 0 || std::size_t(t) >= damage.parts.size()) return 1.f;
    const DamagePart& p = damage.parts[damage.resolve(std::size_t(t))];
    return p.present ? p.ratio : 1.f; // a node without an NDPR record never breaks (port)
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

std::size_t CombatWorld::add_unit(LiveUnit& u, std::uint32_t logic_id, std::uint32_t clan, bool player, bool building) {
    auto c = std::make_unique<Combatant>();
    c->unit = &u;
    c->logic_id = logic_id;
    DamageState& d = c->damage;
    d.clan = clan;
    d.building = building;
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
    std::optional<mission::Scheme> scheme;
    if (u.placement) {
        if (auto path = resolve_path(lib_.game_dir(), u.placement->resource)) {
            try {
                scheme = mission::decode_scheme(read_file(*path));
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "combat: scheme of %s: %s\n", u.placement->resource.c_str(), ex.what());
            }
        }
    }
    // The module CTLD of an objects.rlb INTO / EXTO record (+0x100 library, +0x120 entry).
    auto module_ctld = [&](const std::string& entry) -> std::optional<Ctld> {
        const NResEntry* e = lib_.find("objects.rlb", entry);
        if (!e || e->data.size() < 0x140) return std::nullopt;
        const NResEntry* ce = lib_.find(record_field(e->data, 0x100), record_field(e->data, 0x120));
        if (!ce || ce->type != fourcc("CTLD")) return std::nullopt;
        try {
            return decode_ctld(ce->data);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "combat: %s: %s\n", entry.c_str(), ex.what());
            return std::nullopt;
        }
    };
    if (scheme) {
        auto walk = [&](auto&& self, const mission::SchemeNode& n) -> void {
            if (auto ctld = module_ctld(n.entry))
                for (const auto& dev : ctld->devices)
                    if (dev.type == 0x1b) {
                        float prm[3];
                        std::memcpy(prm, dev.header.data() + 0x2c, sizeof prm);
                        d.armour = Armour{prm[1], prm[2], prm[0]};
                    }
            for (const auto& ch : n.children) self(self, ch);
        };
        walk(walk, scheme->root);
    }

    // Devices: every device of every part's CTLD (LoadControlData 10008b10 with device index −1), in part
    // order; the guns among them also become GunDevices.
    std::vector<std::size_t> part_first(model.parts.size(), 0), part_count(model.parts.size(), 0);
    auto power_device = [&](const CtldDevice& dev, PowerDevice& pd) {
        std::int32_t st = -1;
        std::memcpy(&pd.type, dev.header.data(), 4);
        std::memcpy(&st, dev.header.data() + 0x18, 4);
        std::memcpy(&pd.power, dev.header.data() + 0x20, 4);
        std::memcpy(pd.param.data(), dev.header.data() + 0x2c, sizeof pd.param);
        pd.state = PowerDevice::initial_state(pd.cls, st);
        // CDevice::Load → slot 9 (+0x6c, +0x8c): the shield's effect (10025600); a gun's bullet is its own.
        if (pd.cls == 9) pd.effect = {record_field(dev.header, 0x6c), record_field(dev.header, 0x8c)};
    };
    for (std::size_t pi = 0; pi < model.parts.size(); ++pi) {
        const UnitPart& part = model.parts[pi];
        part_first[pi] = c->energy.devices.size();
        const std::string archive = pi == 0 && !model.ctl_archive.empty() ? model.ctl_archive : part.archive;
        const std::string entry = pi == 0 && !model.ctl_entry.empty() ? model.ctl_entry : part.ctl_entry;
        const NResEntry* ce = entry.empty() ? nullptr : lib_.find(archive, entry);
        if (!ce || ce->type != fourcc("CTLD")) continue;
        const Ctld ctld = decode_ctld(ce->data);
        for (const auto& dev : ctld.devices) {
            PowerDevice pd;
            pd.cls = dev.type;
            power_device(dev, pd);
            // dev+4 (LoadControlData 10009064): a component's id 0 is its mount node, else part base + id;
            // DamageState resolves the component's root node to the mount.
            std::int32_t id = 0;
            std::memcpy(&id, dev.header.data() + 4, 4);
            pd.part = id < 0 ? -1 : std::int32_t(part.transform_base) + id;
            // CreateDevice (1002d4b0): ctl+0x5cc / +0x5d0 hold the force shield / the deflector.
            if (pd.cls == 9) c->energy.shield = std::int32_t(c->energy.devices.size());
            if (pd.cls == 0x15) c->energy.deflector = std::int32_t(c->energy.devices.size());
            if (pd.cls == 8) c->energy.radar = std::int32_t(c->energy.devices.size());
            c->energy.devices.push_back(pd);
        }
        part_count[pi] = ctld.devices.size();
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
        for (std::size_t k = 0; k < ctld.devices.size(); ++k) {
            const CtldDevice& dev = ctld.devices[k];
            if (!is_weapon_device(dev)) continue;
            GunDeviceSpec spec = decode_gun_device(dev);
            const BulletProto* b = spec.bullet.second.empty() ? nullptr : bullet(spec.bullet);
            c->energy.devices[part_first[pi] + k].gun = std::int32_t(c->guns.size());
            c->guns.emplace_back(std::move(spec), joints, b);
            c->mounts.push_back(mount);
        }
    }
    // INTO modules (FUN_1002d890): module@N reloads device N of its owner's CTLD from the module's first
    // device (the device's own class Load, without dev+4). The owner is the part the scheme node became (load_unit_model's
    // depth-first EXTO order) [inferred: N is relative to the owner's devices; the data's placeholder texts
    // match, e.g. o_tur_ht_02 device 1 "i_rdr_l" ← i_rdr_l_df@1]. Nodes with count 0 are skipped as in the
    // economy's battery sum [inferred].
    if (scheme && !model.parts.empty()) {
        std::size_t next = 1;
        auto walk = [&](auto&& self, const mission::SchemeNode& n, std::size_t owner) -> void {
            for (const auto& ch : n.children) {
                const NResEntry* e = lib_.find("objects.rlb", ch.entry);
                if (!e) continue;
                if (e->type == fourcc("EXTO")) {
                    if (next < model.parts.size() && model.parts[next].parent == int(owner) && model.parts[next].mount == ch.mount)
                        self(self, ch, next++);
                    continue;
                }
                if (e->type != fourcc("INTO") || ch.one == 0 || ch.mount < 0 || std::size_t(ch.mount) >= part_count[owner]) continue;
                const auto ctld = module_ctld(ch.entry);
                if (!ctld || ctld->devices.empty()) continue;
                PowerDevice& pd = c->energy.devices[part_first[owner] + std::size_t(ch.mount)];
                const CtldDevice& mod = ctld->devices[0];
                power_device(mod, pd);
                if (pd.gun < 0 || !is_weapon_device(mod)) continue;
                // A clip (INTO clip@N on a gun): CGunDevice::Load (10029650) takes the clip's header — params,
                // flags, bullet names (SetBulletName 10029940) —, refills energy to its param1 and ammo to
                // round(param0); CDevice::Load appends the clip's joint refs to the channels (none in the data;
                // joint base 0). FUN_1002dc20 then runs ReloadBulletInfo (100296f0) on every device after the
                // load, so params 8..10 come from the clip's bullet: all of it is the GunDevice constructor.
                GunDevice& old = c->guns[std::size_t(pd.gun)];
                GunDeviceSpec spec = decode_gun_device(mod);
                spec.id = old.spec().id, spec.part = old.spec().part; // dev+4 is not reloaded
                std::vector<std::uint32_t> barrels = old.spec().barrels;
                barrels.insert(barrels.end(), spec.barrels.begin(), spec.barrels.end());
                spec.barrels = std::move(barrels);
                spec.module = old.spec().module;
                const BulletProto* b = spec.bullet.second.empty() ? nullptr : bullet(spec.bullet);
                old = GunDevice(std::move(spec), old.joints(), b);
            }
        };
        walk(walk, scheme->root, 0);
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
    // The mesh's bounding sphere (AniMesh 10009510): the part spheres in the static pose, centred at
    // their radius-weighted mean, reaching the farthest sphere's edge.
    {
        Vec3 sum{};
        float weight = 0;
        std::vector<std::pair<Vec3, float>> sph;
        for (std::size_t t = 0; t < d.parts.size() && t < model.mesh.transforms.size(); ++t) {
            const DamagePart& p = d.parts[t];
            if (!p.present || p.sphere_radius <= 0) continue;
            const Vec3 q = model.mesh.transforms[t].transform_point(p.sphere_centre);
            sph.push_back({q, p.sphere_radius});
            sum = sum + q * p.sphere_radius, weight += p.sphere_radius;
        }
        if (weight > 0) {
            c->shield_centre_ = sum * (1 / weight);
            for (const auto& [q, r] : sph) {
                const Vec3 e = q - c->shield_centre_;
                c->shield_radius_ = std::max(c->shield_radius_, std::sqrt(dot(e, e)) + r);
            }
        } else {
            c->shield_centre_ = c->bound_centre_, c->shield_radius_ = c->bound_radius_;
        }
    }
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

namespace {
// `d` turned up by `angle` in its vertical plane.
Vec3 raise(Vec3 d, float angle) {
    const float h = std::hypot(d.x, d.y), len = std::sqrt(dot(d, d));
    if (h <= 0 || len <= 0) return d;
    const float e = std::atan2(d.z, h) + angle;
    return Vec3{d.x / h * std::cos(e), d.y / h * std::cos(e), std::sin(e)} * len;
}
} // namespace

void CombatWorld::ai_aim(std::size_t u, Vec3 aim, float dt_s) {
    Combatant& c = *units_[u];
    LiveUnit& body = *c.unit;
    if (!body.rig) return;
    // The angles are taken about the turret's centre (AngleAboutAxis 10026e20 on the TurretCenter /
    // TurretDirect points: the yaw joint's point A, the sight origin the guns converge from), else from
    // the first gun's muzzle. p5 of AimGunChannel: the world z of that point's vector (1 for a level turret).
    Vec3 mount = body.position() + Vec3{0, 0, 2}, axis{0, 0, 1}, yaw_ref{};
    bool centred = false;
    for (const auto& gm : c.mounts)
        if (gm.on_turret() && part_point(c, gm.sight_part, gm.sight_points, gm.sight_origin, mount, &axis)) {
            // TurretDirect: the yaw joint's point B on the same part.
            centred = part_point(c, gm.sight_part, gm.sight_points, body.rig->yaw_dir_point, yaw_ref, nullptr);
            break;
        }
    for (std::size_t g = 0; g < c.guns.size() && !centred; ++g)
        if (auto mz = muzzle(u, g)) {
            mount = *mz;
            break;
        }
    const float up_z = axis.z;
    std::pair<float, float> t;
    if (centred) {
        // CTurretDevice::Update state 0x400 (10027470, turret-camera.md §1.3a): both angles right-handed
        // about point A's vector, so a turret hung under a flyer (axis down) flips their signs.
        t = body.rig->ai_point_inputs(mount, axis, yaw_ref, aim);
    } else {
        // No turret sight ([inferred] fallback): the angles in the body frame (yaw > 0 right, pitch > 0 up;
        // ai::aim_angles) from the first gun's muzzle.
        const Mat4& m = body.frame.matrix;
        const Vec3 fwd{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, right{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, up{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
        const Vec3 d = aim - mount;
        const float x = dot(d, right), y = dot(d, fwd), z = dot(d, up);
        t = body.rig->inputs_for_angles(std::atan2(x, y), std::atan2(z, std::hypot(x, y)));
    }
    body.rig->slew(body.aim_x, body.aim_y, t.first, t.second, dt_s);
    float elev = 0;
    bool found = false;
    for (std::size_t g = 0; g < c.guns.size(); ++g) {
        const BulletProto* b = c.guns[g].bullet();
        if (!c.mounts[g].on_turret() || !b) continue;
        const Vec3 from = muzzle(u, g).value_or(mount);
        const auto e = ballistic_elevation(aim - from, b->speed, b->physics == 3 ? kGravity : 0.f);
        c.guns[g].enabled = e.has_value();
        if (e && b->physics == 3 && !found) elev = *e, found = true;
    }
    // AimGunChannel: to += angle * 0.83 / range * p5, in the pitch channel's t space.
    body.gun_aim_y = found ? body.rig->gun_offset_input(kGunElevation * elev, up_z) : 0.f;
}

float CombatWorld::ai_aim_error(std::size_t u, std::size_t g, Vec3 aim) const {
    const Combatant& c = *units_[u];
    const BulletProto* b = g < c.guns.size() ? c.guns[g].bullet() : nullptr;
    const auto from = muzzle(u, g);
    Vec3 o, sd;
    if (b && b->physics == 0 && g < c.mounts.size() && sight(c, c.mounts[g], o, sd)) {
        // A free-flying bullet converges on the sight (ConvergeOnSight): the sight's error counts.
        const Vec3 to = aim - o;
        const float len = std::sqrt(dot(to, to));
        return len > 0 ? std::acos(std::clamp(dot(sd, to * (1 / len)), -1.f, 1.f)) : 0.f;
    }
    if (!b || b->physics != 3 || !from || !c.mounts[g].on_turret()) return aim_error(u, g, aim);
    // The barrel against the direction the turret commands from its centre (the yaw joint's point A): the
    // line to `aim` raised by AimGunChannel's share of the elevation; the guns sit parallel beside it.
    Vec3 centre = *from, unused;
    part_point(c, c.mounts[g].sight_part, c.mounts[g].sight_points, c.mounts[g].sight_origin, centre, &unused);
    const Vec3 d = aim - centre;
    const auto e = ballistic_elevation(aim - *from, b->speed, kGravity);
    return aim_error(u, g, *from + raise(d, kGunElevation * e.value_or(0.f)));
}

std::optional<float> CombatWorld::ballistic_elevation(Vec3 d, float v, float g) {
    if (g == 0 || v == 0) return 0.f;
    const float a = g * g * 0.25f;               // 1003b378
    const float bq = v * v - g * d.z;
    const float disc = bq * bq - dot(d, d) * a * 4; // 1003b384
    if (!(disc > 0)) return std::nullopt;
    const float k = 0.5f / a, root = std::sqrt(disc) * k; // 1003b190
    const float t2 = k * bq < root ? root + k * bq : k * bq - root;
    if (!(t2 > 0)) return std::nullopt;
    const float t = std::sqrt(t2);
    const Vec3 launch{d.x / t, d.y / t, (t2 * g * 0.5f + d.z) / t};
    return std::acos(std::clamp(dot(normalize(launch), normalize(d)), -1.f, 1.f)); // FUN_10033680 (inferred: acos)
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
    b.target = c.target; // QI(b, 0x204).SetTarget(&dev+0x108): the gun's target, part 0
    b.hp = b.mult * p->hit_points(); // part 0's max HP: hitPoints × mult (property 0xb4 → dev+0x180)
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
        if (auto h = ground_.trace(a, b)) best = WorldHit{WorldHit::Kind::Terrain, h->t, h->point, h->normal, -1, -1, h->face_normal};
    if (statics_)
        if (auto h = statics_->trace(a, b); h && (!best || h->t < best->t))
            best = WorldHit{WorldHit::Kind::Static, h->t, h->point, h->normal, -1, -1, h->face_normal};
    for (std::size_t i = 0; i < units_.size(); ++i) {
        const Combatant& c = *units_[i];
        // The dead leave collision (CCollObject message 7), except a building (class 3; the hero is class 4).
        if (std::int32_t(i) == skip || c.removed || (!c.alive() && !c.damage.building) || c.unit->posed.empty() ||
            (!everything && !c.building()))
            continue;
        if (!segment_hits_sphere(a, b, c.centre(), c.bound_radius_)) continue;
        const RenderMesh& mesh = c.unit->model.mesh;
        if (c.building()) c.draw_cache_.resize(mesh.draws.size());
        for (std::size_t di = 0; di < mesh.draws.size(); ++di) {
            const RenderDraw& d = mesh.draws[di];
            if (d.transform >= c.unit->posed.size()) continue;
            const std::size_t part = c.damage.resolve(d.transform);
            // Thrown off as debris (DetachPartDebris 100102a0; never a building, class 3).
            if (part < c.damage.parts.size() && c.damage.parts[part].destroyed() && !c.damage.building) continue;
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
                    const Vec3 face = normalize(cross(v[1] - v[0], v[2] - v[0]));
                    const Vec3 n = dot(face, b - a) > 0 ? face * -1.f : face;
                    best = WorldHit{WorldHit::Kind::Unit, *t, a + (b - a) * *t, n, std::int32_t(i), std::int32_t(part), face};
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
                const Vec3 face = normalize(cross(v[1] - v[0], v[2] - v[0]));
                const Vec3 n = dot(face, b - a) > 0 ? face * -1.f : face;
                best = WorldHit{WorldHit::Kind::Unit, *t, a + (b - a) * *t, n, std::int32_t(i), std::int32_t(part), face};
            }
        }
    }
    return best;
}

// CSeekerDevice::Track (100247c0): the target's position in the bullet frame while the seeker holds it.
// The target object must exist and its part 0 have HP (a destroyed aimed part falls back to part 0; the
// port's guns aim at part 0); the aim point is that part's origin; it must lie within the seeker range
// (param 1) and the cone (dot(forward, direction) ≥ cos(param 0), 100247a0).
std::optional<Vec3> CombatWorld::seek(const Bullet& b) const {
    const BulletProto& p = *b.proto;
    if (!p.seeker || b.target < 0 || std::size_t(b.target) >= units_.size()) return std::nullopt;
    const Combatant& t = *units_[std::size_t(b.target)];
    const std::size_t r = t.damage.root;
    if (t.removed || !t.alive() || r >= t.unit->posed.size()) return std::nullopt;
    const Vec3 aim = t.unit->posed[r].transform_point({});
    const Vec3 to = aim - b.pos;
    const float dist = std::sqrt(dot(to, to));
    if (!(dist <= (*p.seeker)[1])) return std::nullopt;
    if (dist > 0 && dot(to * (1 / dist), b.dir) < std::cos((*p.seeker)[0])) return std::nullopt;
    const Mat4 f = bullet_matrix(b.pos, b.dir); // orthonormal: the local coordinates are projections
    return Vec3{dot(to, normalize(rotate(f, {1, 0, 0}))), dot(to, normalize(rotate(f, {0, 1, 0}))), dot(to, normalize(rotate(f, {0, 0, 1})))};
}

void CombatWorld::fly(Bullet& b, float dt_ms, float now) {
    const float dt = dt_ms * 0.001f;
    const BulletProto& p = *b.proto;
    // Homing (CBulletControl::Update 1000cbb0): the angular command is −(1000/dt_ms)·error per axis, the
    // errors atan2(z, y) (about x) and atan2(x, y) (about z) of the target in the bullet frame (0 when
    // y = 0), clamped to ±body[9] / ±body[11] and scaled by |v_y| / body[7]; no roll. Port: the turn is
    // applied in this step to the direction and, the velocity being kept in the bullet frame, to it.
    if (auto local = seek(b); local && dt_ms > 0) {
        const float ex = local->y != 0 ? std::atan2(local->z, local->y) : 0.f;
        const float ez = local->y != 0 ? std::atan2(local->x, local->y) : 0.f;
        const float k = p.speed != 0 ? std::abs(dot(b.vel, b.dir) / p.speed) : 0.f;
        const float ax = std::clamp(ex, -p.turn[0] * dt, p.turn[0] * dt) * k;
        const float az = std::clamp(ez, -p.turn[2] * dt, p.turn[2] * dt) * k;
        const Mat4 f = bullet_matrix(b.pos, b.dir);
        const Vec3 xa = normalize(rotate(f, {1, 0, 0})), za = normalize(rotate(f, {0, 0, 1}));
        const Vec3 w = xa * ax - za * az; // +x turns forward toward +z; −z turns it toward +x
        if (const float angle = std::sqrt(dot(w, w)); angle > 0) {
            const Vec3 axis = w * (1 / angle);
            auto turn = [&](Vec3 v) {
                const float c = std::cos(angle), s = std::sin(angle);
                return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
            };
            b.dir = normalize(turn(b.dir));
            b.vel = turn(b.vel);
        }
    }
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
    auto h = trace(from, to, b.shooter);
    // OnCollision (1000d0c0), the object hits first, nearest first while nearer than the face hit: only a
    // shield side matters (GetShieldSide −1: skip). A bullet whose HP does not exceed the side's strength
    // stops there (situation 2, its explosion's face hit {object, −2, side}); a stronger one collapses the
    // side (DrainShieldSide(side, 0), after OnDamageMsg to the object, both only while its shooter exists),
    // loses that strength (ApplyDamageTotal(−s)) and flies on.
    {
        struct SphereHit {
            float t;
            std::size_t unit;
            Vec3 point;
        };
        std::vector<SphereHit> spheres;
        const Vec3 seg = to - from;
        const float len2 = dot(seg, seg);
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const Combatant& c = *units_[i];
            if (std::int32_t(i) == b.shooter || c.removed || !c.alive() || c.energy.shield < 0) continue;
            const Vec3 centre = c.shield_centre(), rel = from - centre;
            const float r = c.shield_radius(), c0 = dot(rel, rel) - r * r;
            if (c0 <= 0 || len2 <= 0) continue; // only a sphere the bullet enters this step (inferred)
            const float bq = dot(rel, seg), disc = bq * bq - len2 * c0;
            if (bq >= 0 || disc < 0) continue;
            const float t = (-bq - std::sqrt(disc)) / len2;
            if (t <= 1) spheres.push_back({t, i, from + seg * t});
        }
        std::sort(spheres.begin(), spheres.end(), [](const SphereHit& x, const SphereHit& y) { return x.t < y.t; });
        for (const SphereHit& sh : spheres) {
            if (h && h->t <= sh.t) break; // the face is nearer
            const std::int32_t side = shield_side(sh.unit, sh.point);
            if (side < 0) continue;
            Combatant& c = *units_[sh.unit];
            const float s = c.energy.side_strength(std::size_t(side), c.energy_host());
            if (b.hp <= s) {
                b.pos = sh.point;
                b.alive = false;
                events_.push_back({CombatEvent::Type::BulletEnded, -1, b.shooter, 2, sh.point, {}, b.id});
                if (p.expl) explode(b, *p.expl, sh.point, std::int32_t(sh.unit), kShieldPart, side);
                b.hp = 0;
                return;
            }
            if (b.shooter >= 0 && !units_[std::size_t(b.shooter)]->removed) {
                events_.push_back({CombatEvent::Type::Damaged, std::int32_t(sh.unit), b.shooter, 0, sh.point, {}});
                c.energy.set_side_strength(std::size_t(side), 0, c.energy_host());
            }
            b.hp -= s;
        }
    }
    if (h) {
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

std::int32_t CombatWorld::shield_side(std::size_t u, Vec3 p) {
    Combatant& c = *units_[u];
    const EnergyHost host = c.energy_host();
    if (!c.energy.shield_active(host)) return -1; // slot 0x30 fails
    // The hit point relative to the shield sphere's centre, in the object's frame (GetPlacement(2) and
    // FastProc +0x8c).
    const Vec3 centre = c.shield_centre(), d = p - centre;
    const Mat4& m = c.unit->frame.matrix;
    const Vec3 local{dot(d, normalize(rotate(m, {1, 0, 0}))), dot(d, normalize(rotate(m, {0, 1, 0}))), dot(d, normalize(rotate(m, {0, 0, 1})))};
    const std::size_t side = EnergyBudget::shield_side(local);
    const PowerDevice& dev = c.energy.devices[std::size_t(c.energy.shield)];
    if (c.energy.side_strength(side, host) > 0 && !dev.effect.first.empty() && !dev.effect.second.empty() && dot(d, d) > 0) {
        // 10025ca0: the next of the shield's three effects (handles from 100254e0) gets the matrix
        // {x = the direction to the hit, y ⟂ in the horizontal plane, z = x × y} at the shield centre
        // (SetEffectMatrix, space 2), the scale (r, r, r) (SetEffectScale) and StartEffect(mode 1).
        const Vec3 n = normalize(d);
        Vec3 v;
        if (n.x == 0) v = {1, 0, 0}; // 10048550
        else if (n.y == 0) v = {0, 1, 0}; // 10048560
        else {
            v = {-n.y, n.x, 0};
            if (n.z != 0) v = normalize(v);
        }
        const Vec3 w = cross(n, v);
        Mat4 frame = Mat4::identity();
        frame.at(0, 0) = n.x, frame.at(1, 0) = n.y, frame.at(2, 0) = n.z;
        frame.at(0, 1) = v.x, frame.at(1, 1) = v.y, frame.at(2, 1) = v.z;
        frame.at(0, 2) = w.x, frame.at(1, 2) = w.y, frame.at(2, 2) = w.z;
        frame.at(0, 3) = centre.x, frame.at(1, 3) = centre.y, frame.at(2, 3) = centre.z;
        CombatEvent ev{CombatEvent::Type::Effect, std::int32_t(u), -1, 0, centre, dev.effect, 0, n, c.shield_radius()};
        ev.placement = frame;
        ev.attached = true;
        ev.handle = std::uint32_t(u + 1) << 2 | c.shield_fx_;
        ev.start_mode = 1;
        events_.push_back(ev);
        c.shield_fx_ = (c.shield_fx_ + 1) % 3;
    }
    return std::int32_t(side);
}

void CombatWorld::absorb_by_shield(std::size_t v, DamageRecord& rec) {
    Combatant& c = *units_[v];
    if (c.energy.shield < 0) return; // ctl+0x38 has no shield
    std::int32_t side;
    if (rec.target == std::int32_t(v) && rec.target_part == kShieldPart) {
        side = rec.target_side; // the face hit is this shield's
    } else {
        // A splash whose centre lies outside the shield sphere but within the record's radius of it.
        const Vec3 d = rec.pos - c.shield_centre();
        const float dist = std::sqrt(dot(d, d)), r = c.shield_radius();
        if (!(dist < r + rec.radius) || !(dist > r)) return;
        side = shield_side(v, rec.pos);
    }
    if (side < 0 || side >= 6) return;
    const EnergyHost host = c.energy_host();
    const float s = c.energy.side_strength(std::size_t(side), host);
    const float taken = rec.damage < s ? rec.damage : s;
    rec.damage -= taken;
    c.energy.set_side_strength(std::size_t(side), s - taken, host);
}

void CombatWorld::explode(const Bullet& b, const Expl& e, Vec3 pos, std::int32_t target, std::int32_t part, std::int32_t side) {
    ++hits;
    events_.push_back({CombatEvent::Type::Hit, target, b.shooter, 0, pos, {}});
    if (!e.fx[0].second.empty()) events_.push_back({CombatEvent::Type::Effect, target, b.shooter, 0, pos, e.fx[0], 0, b.dir, 1});
    DamageRecord r;
    r.kind = e.kind;
    r.target = target, r.target_part = part, r.target_side = side;
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
        if (rec.target_part < 0) { // a shield-only hit: AbsorbByShield on a copy, no part damage
            DamageRecord copy = rec;
            absorb_by_shield(v, copy);
        } else if (!d.invulnerable && std::int32_t(v) != rec.attacker) {
            const std::size_t part = d.resolve(std::size_t(rec.target_part));
            float r;
            const Vec3 pc = part_centre(c, part, r);
            d.damage_part_from_record(part, std::int32_t(v), rec, pc, r, now);
            // A destroyed part is thrown off with the parts under it (DetachPartDebris; port reading:
            // they are destroyed with it). DetachPartDebris leaves class 3 (a building) alone.
            if (d.parts[part].destroyed() && !d.building) d.propagate_destruction(now);
            d.update_part_stage(part, true);
        }
    } else if (rec.kind == 3 || rec.kind == 4) {
        DamageRecord copy = rec; // the shield takes its share of the copy first
        absorb_by_shield(v, copy);
        if (!d.invulnerable && std::int32_t(v) != rec.attacker && copy.damage != 0) {
            for (std::size_t i = 0; i < d.parts.size(); ++i) {
                if (!d.parts[i].present) continue;
                float r;
                const Vec3 pc = part_centre(c, i, r);
                d.damage_part_from_record(i, std::int32_t(v), copy, pc, r, now);
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

void CombatWorld::update_energy(Combatant& c, float dt_s, float now) {
    EnergyHost host = c.energy_host();
    if (const UnitMotion* m = c.unit->motion.get()) {
        // 100265c0: max|ctl+0x1c8..0x1d0| / max|file body[6..8]| · ctl+0x154 (the live state's +0x54).
        const Vec3 v = m->velocity();
        const auto& b = c.unit->ctld.body;
        const float top = std::max({std::abs(b[6]), std::abs(b[7]), std::abs(b[8])});
        const float speed = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        host.engine_load = top > 0 ? speed / top * m->live().energy : 0.f; // port guard: no top speed
    }
    host.damage = &c.damage;
    host.now_ms = now;
    c.energy.update(dt_s, c.guns, host);
}

void CombatWorld::set_rooms(std::size_t unit, std::vector<Room> rooms) {
    std::erase_if(room_sets_, [&](const RoomSet& s) { return s.building == unit; });
    if (!rooms.empty()) room_sets_.push_back({unit, std::move(rooms), std::nullopt});
}

// MRoomManager_Takt (10018ac0) for every building's rooms: a = 0.1 · dt over the occupants of each reload room.
void CombatWorld::update_rooms(float now) {
    for (RoomSet& set : room_sets_) {
        const Combatant& b = *units_[set.building];
        if (!set.last_ms) set.last_ms = now;
        if (now - *set.last_ms < kRoomPeriodMs) continue;
        const float a = kRoomRate * (now - *set.last_ms) * 0.001f;
        set.last_ms = now;
        if (b.removed || !b.alive()) continue;
        for (const Room& r : set.rooms) {
            if (!r.reloads()) continue;
            const Vec3 at = b.unit->frame.matrix.transform_point(r.pos);
            for (std::size_t u = 0; u < units_.size(); ++u) {
                Combatant& c = *units_[u];
                // [inferred] mobile units only; allied = the same clan; behaviour property 0x208 taken as 0.
                if (u == set.building || c.removed || !c.alive() || c.building() || c.damage.clan != b.damage.clan) continue;
                const Vec3 v = c.unit->motion->world_velocity();
                if (std::sqrt(dot(v, v)) >= r.speed_limit || !room_contains(r, at, c.unit->position())) continue;
                c.recharge(a, now);
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
    update_rooms(now);
    // Slow damage tick and removal.
    for (std::size_t u = 0; u < units_.size(); ++u) {
        Combatant& c = *units_[u];
        if (c.removed) continue;
        if (now >= c.next_slow_tick_) {
            // Update (1000c7d8): dt = (now − the last slow tick)·0.001 s for SlowTickDamage (10012a40) and
            // UpdateEnergyBudget (1002d340). Port: the first tick measures from the unit's first update.
            const float dt_s = (now - c.last_slow_tick_.value_or(now)) * 0.001f;
            c.last_slow_tick_ = now;
            const bool was = c.damage.killed;
            c.damage.slow_tick(dt_s, now);
            update_energy(c, dt_s, now);
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
            det[t] = !c.damage.building && p != c.damage.root && p < c.damage.parts.size() && c.damage.parts[p].destroyed();
        }
    }
}

} // namespace parkan::combat
