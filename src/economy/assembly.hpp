// An assembled unit as the engine builds it from a design (scheme): the agent's component list (AniMesh
// CAgent_AttachComponent 10003760 / CAgent_DetachComponent 10003c40), the global mesh-part list whose named
// nodes are the hardpoints (IAnimation slots 0x18 / 0x38 / 0x3c / 0x4c) and the global device list whose named
// devices are the internal slots (IItemManager slots 0x3c / 0x44 / 0x48 / 0x50), plus the control properties the
// HQ and the unit designer show: mass 0x7c, payload 0x88 / 0x89, speed 0x90 / 0x91, defence 0xb1, offence 0xb2
// and radar 0x50. Spec: notes/mounts-slots.md, notes/unit-stats.md, notes/designer.md §2.6 / §3.
//
// Port decisions:
// - A fresh object: every part at full HP, device ratios 1, object scale 1, no world (surface factor 1, the
//   locomotion HP means L = R = 1), as in the designer (unit-stats.md §5 [inferred]).
// - Building (FORT) designs are not assembled (their prototype layout differs; unit-stats.md §11).
#pragma once

#include "mission/scheme.hpp"
#include "res/ctld.hpp"
#include "res/library.hpp"

#include <optional>

namespace parkan::economy {

// The objects.rlb record pairs a component uses (lib, entry at 32-byte steps; aliases into objects.rlb followed).
struct PrototypeRecord {
    std::uint32_t type = 0; // NRes type: BTLU, EXTO, INTO, FORT, …
    std::string mesh_lib, mesh, ndp_lib, ndp, ctl_lib, ctl, sheet_lib, sheet; // +0x00/+0x20, +0xc0/+0xe0, +0x100/+0x120, +0x140/+0x160
};
std::optional<PrototypeRecord> load_prototype(AssetLibrary& lib, std::string_view entry);

// A behpsp.res sheet's int field (kind 5), e.g. BehChassis ChassisType of `chas_fly.var` (1 fly … 5 worm).
std::optional<std::int32_t> read_sheet_int(AssetLibrary& lib, std::string_view sheet_lib, std::string_view sheet, std::string_view field);

struct UnitStats {
    float mass = 0;            // 0x7c, kg
    float payload = 0;         // 0x88 = the chassis CTLD body[26] (capacity, kg)
    float payload_free = 0;    // 0x89 = max(0, body[26] + Σ chassis ρ·V − mass)
    float speed_nominal = 0;   // 0x90 = body[7], m/s
    float speed = 0;           // 0x91 = RecomputePerformance's body[7], m/s
    float defence = 0;         // 0xb1
    float offence = 0;         // 0xb2
    std::optional<float> radar; // 0x50 (no radar device: the property fails)
    std::int32_t chassis_type = 0; // property 0x207 (BehChassis ChassisType)
    std::uint32_t size_class = 0;  // property 0x201
    // Device property 0x75 (CDeviceSet::GetProperty(0xd) 1002b410, notes/device-props.md): the last created turret
    // device's (CTLD type 1) header +8 has bit 0x08000000 — IsHqCapable 100773a0.
    bool hq_turret = false;
    bool valid() const { return payload_free > 0; } // DrawUnitStats' return value
};

class Assembly {
public:
    struct Component {
        std::int32_t id = 0, parent = -1, mount = -1;
        std::string entry;
        PrototypeRecord proto;
    };
    struct Part {
        std::int32_t component = 0, node = 0;
        std::string name; // mesh stream 10; empty = not a hardpoint
        float area = 0, volume = 0, density = 0, hit_points = 0;
    };
    struct Device {
        std::int32_t component = 0;
        CtldDevice dev; // header replaced by an INTO module's first device (type, part and name kept)
    };

    explicit Assembly(AssetLibrary& lib) : lib_(lib) {}
    // CAgent::AssembleComponents: the base is component 0. False when the record does not resolve.
    bool set_root(std::string_view entry);
    // SendMsg(6, 0x80000020, {lib, entry, parent, mount}): the new component's id (the smallest free ≥ 0), or −1.
    std::int32_t attach(std::string_view entry, std::int32_t parent, std::int32_t mount);
    // SendMsg(6, 0x14, id): removes the component's parts and devices (its children are not detached).
    void detach(std::int32_t id);
    void clear() { components_.clear(), parts_.clear(), devices_.clear(); }
    bool empty() const { return components_.empty(); }

    // IAnimation (iid 0xb).
    std::int32_t part_count() const { return std::int32_t(parts_.size()); }                    // slot 0x4c
    const std::string* part_name(std::int32_t i) const;                                        // slot 0x18
    std::int32_t find_part(std::int32_t id) const;                                             // slot 0x38
    std::int32_t count_parts(std::int32_t id) const;                                           // slot 0x3c
    // IItemManager (iid 0x202).
    std::int32_t device_count() const { return std::int32_t(devices_.size()); }                // slot 0x50
    const std::string* device_name(std::int32_t j) const;                                      // slot 0x3c
    std::int32_t first_device(std::int32_t id) const;                                          // slot 0x44
    std::int32_t count_devices(std::int32_t id) const;                                         // slot 0x48

    const std::vector<Component>& components() const { return components_; }
    const std::vector<Part>& parts() const { return parts_; }
    const std::vector<Device>& devices() const { return devices_; }
    UnitStats stats() const;

private:
    AssetLibrary& lib_;
    std::vector<Component> components_;
    std::vector<Part> parts_;
    std::vector<Device> devices_;
    std::array<float, 27> body_{}; // the root's CTLD body
    bool load(Component& c);
};

// The design assembled as the engine does it (scheme tree, depth first) and its stats; nullopt when the root
// does not resolve. `chassis_type` and `size_class` come from the root record's sheet and the scheme.
std::optional<UnitStats> design_stats(AssetLibrary& lib, const mission::Scheme& s);
// Builds `a` from the scheme (a cleared first). False when the root does not resolve.
bool assemble(Assembly& a, const mission::Scheme& s);

// GetUnitDebugCode 100766c0: size letter (0x201: 1 T, 2 S, 3 M, 4 L), movement letter (0x207: 1 F, 2 S, 3 W, 4 T,
// 5 A, 6 U) and class letter (W warbot, T transport, B builder, C HQ warbot, H hero); '?' otherwise.
std::string unit_debug_code(std::uint32_t size_class, std::int32_t chassis_type, std::uint32_t type_mask);
// GetUnitTypeName 100768e0: the iron3d string id of a unit type (6202 Варбот … 6205 нет данных).
std::uint32_t unit_type_name_id(std::uint32_t type_mask);

} // namespace parkan::economy
