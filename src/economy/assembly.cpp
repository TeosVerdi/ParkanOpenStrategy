#include "economy/assembly.hpp"

#include "economy/econ_data.hpp"
#include "res/fx.hpp"
#include "res/mesh.hpp"
#include "res/nres.hpp"
#include "res/weapons.hpp"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace parkan::economy {

namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 | std::uint32_t(std::uint8_t(s[2])) << 16 |
           std::uint32_t(std::uint8_t(s[3])) << 24;
}

std::string field32(std::span<const std::uint8_t> d, std::size_t off) {
    if (off + 32 > d.size()) return {};
    const char* p = reinterpret_cast<const char*>(d.data() + off);
    return std::string(p, strnlen(p, 32));
}

bool ieq(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

// CDevice header fields (weapons-damage.md §1): +0x1c mass (device property 0x100), +0x2c + 4·n param[n].
float header_f(const CtldDevice& d, std::size_t off) {
    float v;
    std::memcpy(&v, d.header.data() + off, 4);
    return v;
}
float mass_of(const CtldDevice& d) { return header_f(d, 0x1c); }
float param(const CtldDevice& d, std::size_t n) { return header_f(d, 0x2c + 4 * n); }

const NResEntry* find_entry(AssetLibrary& lib, const std::string& l, const std::string& e) {
    if (l.empty() || e.empty()) return nullptr;
    try {
        return lib.find(l, e);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

std::optional<PrototypeRecord> load_prototype(AssetLibrary& lib, std::string_view entry) {
    std::string key(entry);
    for (int depth = 0; depth < 5; ++depth) {
        const NResEntry* e = lib.find("objects.rlb", key);
        if (!e || e->data.size() < 0x40) return std::nullopt;
        PrototypeRecord r;
        r.type = e->type;
        r.mesh_lib = field32(e->data, 0x00), r.mesh = field32(e->data, 0x20);
        // An alias record (e.g. a FORT naming its agent) names another objects.rlb record.
        if (ieq(r.mesh_lib, "objects.rlb")) {
            key = r.mesh;
            continue;
        }
        if (e->data.size() < 0x140) return std::nullopt;
        r.ndp_lib = field32(e->data, 0xc0), r.ndp = field32(e->data, 0xe0);
        r.ctl_lib = field32(e->data, 0x100), r.ctl = field32(e->data, 0x120);
        if (e->data.size() >= 0x180) r.sheet_lib = field32(e->data, 0x140), r.sheet = field32(e->data, 0x160);
        return r;
    }
    return std::nullopt;
}

std::optional<std::int32_t> read_sheet_int(AssetLibrary& lib, std::string_view sheet_lib, std::string_view sheet, std::string_view name) {
    const NResEntry* e = find_entry(lib, std::string(sheet_lib), std::string(sheet));
    if (!e) return std::nullopt;
    // u32 count; records {u32 size, kind, reserved, nameLen; name; u32 flags, value, default, min, max}.
    try {
        Reader r(e->data);
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            r.u32();
            const std::uint32_t kind = r.u32();
            r.u32();
            const std::string f = r.str(r.u32());
            r.u32();
            const std::uint32_t value = r.u32();
            r.u32(), r.u32(), r.u32();
            if (kind == 5 && f == name) return std::int32_t(value);
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

bool Assembly::load(Component& c) {
    auto proto = load_prototype(lib_, c.entry);
    if (!proto) return false;
    c.proto = *proto;
    // CAgent_AttachComponent: an EXTO record brings a mesh and devices, anything else under a parent is an INTO module.
    const bool into = c.parent >= 0 && c.proto.type != fourcc("EXTO");
    std::optional<Ctld> ctl;
    if (const NResEntry* e = find_entry(lib_, c.proto.ctl_lib, c.proto.ctl)) try {
            ctl = decode_ctld(e->data);
        } catch (const std::exception&) {
        }
    if (into) {
        // CControlBase_ReloadDeviceFromModule 1002d890: the parent's device `mount` re-Loads the module's first
        // device header; the device keeps its class (+0x00), part (+0x04) and name.
        const std::int32_t j = first_device(c.parent) + c.mount;
        if (ctl && !ctl->devices.empty() && j >= 0 && j < device_count() && c.mount >= 0) {
            CtldDevice& d = devices_[std::size_t(j)].dev;
            std::array<std::uint8_t, 8> keep;
            std::memcpy(keep.data(), d.header.data(), 8);
            d.header = ctl->devices.front().header;
            std::memcpy(d.header.data(), keep.data(), 8);
        }
        return true;
    }
    // Mesh parts (CAniMesh_AttachComponentMesh 1000a460): the chassis keeps every node, a component skips its root.
    std::optional<Mesh> mesh;
    if (const NResEntry* e = find_entry(lib_, c.proto.mesh_lib, c.proto.mesh)) try {
            mesh = decode_mesh(e->data);
        } catch (const std::exception&) {
        }
    std::vector<NdprPart> ndpr;
    if (const NResEntry* e = find_entry(lib_, c.proto.ndp_lib, c.proto.ndp)) try {
            ndpr = decode_ndpr(e->data);
        } catch (const std::exception&) {
        }
    if (mesh)
        for (std::size_t k = c.id == 0 ? 0 : 1; k < mesh->nodes.size(); ++k) {
            Part p;
            p.component = c.id, p.node = std::int32_t(k), p.name = mesh->nodes[k].name;
            const std::uint16_t slot = mesh->nodes[k].slots[0];
            if (slot != 0xFFFF && slot < mesh->slots.size()) p.area = mesh->slots[slot].area, p.volume = mesh->slots[slot].volume;
            if (k < ndpr.size()) p.density = ndpr[k].density, p.hit_points = ndpr[k].hit_points;
            parts_.push_back(std::move(p));
        }
    if (ctl) {
        if (c.id == 0) body_ = ctl->body;
        for (const auto& d : ctl->devices) devices_.push_back({c.id, d});
    }
    return true;
}

bool Assembly::set_root(std::string_view entry) {
    clear();
    components_.push_back({0, -1, -1, std::string(entry), {}});
    if (!load(components_.back())) {
        clear();
        return false;
    }
    return true;
}

std::int32_t Assembly::attach(std::string_view entry, std::int32_t parent, std::int32_t mount) {
    if (components_.empty()) return -1;
    std::int32_t id = 0;
    while (std::any_of(components_.begin(), components_.end(), [&](const Component& c) { return c.id == id; })) ++id;
    Component c{id, parent, mount, std::string(entry), {}};
    if (!load(c)) return -1;
    components_.push_back(std::move(c));
    return id;
}

void Assembly::detach(std::int32_t id) {
    for (auto it = components_.rbegin(); it != components_.rend(); ++it)
        if (it->id == id) {
            components_.erase(std::next(it).base());
            break;
        }
    std::erase_if(parts_, [&](const Part& p) { return p.component == id; });
    std::erase_if(devices_, [&](const Device& d) { return d.component == id; });
}

const std::string* Assembly::part_name(std::int32_t i) const {
    if (i < 0 || i >= part_count() || parts_[std::size_t(i)].name.empty()) return nullptr;
    return &parts_[std::size_t(i)].name;
}
std::int32_t Assembly::find_part(std::int32_t id) const {
    for (std::size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].component == id) return std::int32_t(i);
    return -1;
}
std::int32_t Assembly::count_parts(std::int32_t id) const {
    return std::int32_t(std::count_if(parts_.begin(), parts_.end(), [&](const Part& p) { return p.component == id; }));
}
const std::string* Assembly::device_name(std::int32_t j) const {
    if (j < 0 || j >= device_count() || devices_[std::size_t(j)].dev.text.empty()) return nullptr;
    return &devices_[std::size_t(j)].dev.text;
}
std::int32_t Assembly::first_device(std::int32_t id) const {
    for (std::size_t j = 0; j < devices_.size(); ++j)
        if (devices_[j].component == id) return std::int32_t(j);
    return -1;
}
std::int32_t Assembly::count_devices(std::int32_t id) const {
    return std::int32_t(std::count_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.component == id; }));
}

UnitStats Assembly::stats() const {
    UnitStats s;
    // The last device of a type wins (CreateDevice 1002d4b0 caches them).
    const CtldDevice *armour = nullptr, *shield = nullptr, *deflector = nullptr, *radar = nullptr;
    for (const auto& d : devices_) {
        switch (d.dev.type) {
        case 0x1b: armour = &d.dev; break;
        case 9: shield = &d.dev; break;
        case 0x15: deflector = &d.dev; break;
        case 8: radar = &d.dev; break;
        default: break;
        }
    }
    // CControlBase_InitMassAndPayload 1000fac0 (unit-stats.md §3–§4).
    s.payload = body_[26];
    float payload = body_[26], mass = 0;
    const float arm_m = armour ? param(*armour, 0) : 0.f;
    for (const auto& p : parts_) {
        float m = p.density * p.volume;
        if (p.component == 0) payload += m; // the chassis' own bare mass is free
        if (armour) m += arm_m * p.area;
        mass += m;
    }
    for (const auto& d : devices_) mass += mass_of(d.dev);
    s.mass = mass;
    s.payload_free = std::max(0.f, payload - mass);
    // RecomputePerformance 1000fca0 (§5): P = Σ engine param0 (full HP), L = R = 1, s = 1.
    s.speed_nominal = body_[7];
    float P = 0;
    for (const auto& d : devices_)
        if (d.dev.type == 5) P += param(d.dev, 0);
    const float f = body_[26] > 0 ? s.payload_free / body_[26] : 0.f;
    const float file7 = body_[7], run7 = file7 * P * (1 + f) / 2;
    s.speed = file7 >= 0 ? std::min(file7, run7) : std::max(file7, run7);
    // CControlBase_GetDefenceValue 13940 (§6): the first part with an area; armour divides; deflector × shield adds.
    float hp = 0;
    for (const auto& p : parts_)
        if (p.area != 0) {
            hp = p.hit_points;
            break;
        }
    if (armour && param(*armour, 1) == 0) hp = FLT_MAX;
    else {
        if (armour) hp /= param(*armour, 1);
        if (shield && deflector) hp += param(*deflector, 0) * param(*shield, 0);
    }
    s.defence = hp;
    // CDeviceSet::GetProperty(6) 1002b62b (§7): Σ guns (type 2) of the bullet's hit value × 1000 / max(1, param3).
    float offence = 0;
    for (const auto& d : devices_) {
        if (d.dev.type != 2) continue;
        const GunDeviceSpec g = decode_gun_device(d.dev);
        float per_hit = 0;
        try {
            const BulletProto b = load_bullet(lib_, g.bullet.second);
            // SumPartHP(bullet, 1) 10013620: every NDPR part's HP + the EXPL damage of every part with an effect.
            for (const auto& part : b.parts) {
                per_hit += part.hit_points;
                if (part.has_effect())
                    if (const NResEntry* e = lib_.find(part.effect.first, part.effect.second)) per_hit += decode_expl(e->data).damage;
            }
        } catch (const std::exception&) {
        }
        offence += per_hit * 1000.f / std::max(1.f, g.param[3]);
    }
    s.offence = offence;
    if (radar) s.radar = param(*radar, 3);
    return s;
}

bool assemble(Assembly& a, const mission::Scheme& s) {
    if (!a.set_root(s.root.entry)) return false;
    auto walk = [&](auto&& self, const mission::SchemeNode& n, std::int32_t id) -> void {
        for (const auto& c : n.children) {
            const std::int32_t cid = a.attach(c.entry, id, c.mount);
            if (cid >= 0) self(self, c, cid);
        }
    };
    walk(walk, s.root, 0);
    return true;
}

std::optional<UnitStats> design_stats(AssetLibrary& lib, const mission::Scheme& s) {
    Assembly a(lib);
    if (!assemble(a, s)) return std::nullopt;
    UnitStats st = a.stats();
    const auto& root = a.components().front().proto;
    st.chassis_type = read_sheet_int(lib, root.sheet_lib, root.sheet, "ChassisType").value_or(0);
    st.size_class = robot_size(s);
    return st;
}

std::string unit_debug_code(std::uint32_t size_class, std::int32_t chassis_type, std::uint32_t type) {
    std::string c(3, '?');
    if (size_class >= 1 && size_class <= 4) c[0] = "TSML"[size_class - 1];
    if (chassis_type >= 1 && chassis_type <= 6) c[1] = "FSWTAU"[chassis_type - 1];
    switch (type) {
    case 0x1008000: c[2] = 'W'; break;
    case 0x1002000: c[2] = 'T'; break;
    case 0x1004000: c[2] = 'B'; break;
    case 0x1010000: c[2] = 'C'; break;
    case 0x1020000: c[2] = 'H'; break;
    default: break;
    }
    return c;
}

std::uint32_t unit_type_name_id(std::uint32_t type) {
    switch (type) {
    case 0x1008000: return 0x183a;
    case 0x1002000: return 0x1838;
    case 0x1004000: return 0x1839;
    case 0x1010000: return 0x183b;
    case 0x1020000: return 0x183c;
    default: return 0x183d;
    }
}

} // namespace parkan::economy
