// Static economy data: the profession sheets' economy fields (behpsp.res prof_*), the BehConstants
// economy defaults, BuildDat.lst (building levels), the data.tma object parameters (ore, free bots,
// free research), the mineral lodes, scheme facts (robot size, battery capacity) and a scheme cache.
// Spec: notes/base-building.md §1–§3, notes/designs-research.md §1, §5.4, notes/behaviour-data.md.
#pragma once

#include "core/math.hpp"
#include "mission/scheme.hpp"
#include "mission/tma.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>

namespace parkan::economy {

// BehProfession economy fields (MBehaviour+0x820; behpsp.res prof_*.var).
struct Profession {
    float store_ore_max = 0, store_ore_now = 0; // +0x4c / +0x50
    float ore_onboard = 0;                      // Transfer_Ore_OnBoard +0x54 (property 0x1001)
    float ore_offboard = 0;                     // Transfer_Ore_OffBoard +0x58 (× pf = property 0x1002)
    float power_in = 0;                         // Transfer_Power_In +0x5c (property 0x1004; unread here)
    float power_out = 0;                        // Transfer_Power_Out +0x60 (property 0x1005)
    float use_power = 0;                        // Use_Power +0x64
    float use_ore = 0;                          // Use_Ore +0x68
};

// prof_* sheets of behpsp.res by entry name without ".var" ("prof_plant", ...). Throws on a bad file.
std::map<std::string, Profession> load_professions(const std::filesystem::path& behpsp);
// The sheet an ObjectTypeCode uses (Behavior 10008a80's switch; behaviour-data.md "Object type codes").
const char* profession_sheet(std::uint32_t type);

// BehConstants economy defaults (BehConstants::SetDefaults 10016250; no sheet ships).
struct BehConstants {
    float build_build_distance = 150;
    float building_cost = 100;
    float mine_ore_per_second = 50;
    float mine_max_ore = 500;
    float storage_max_ore = 4000;
    float transport_ore_onboard_per_second = 100;
    float transport_ore_offboard_per_second = 100;
    float transport_max_ore = 2000;
    float transport_building_dist = 80;
};

// BuildDat.lst (CSystemArealMap 10043038): level n of a building type = the n-th listed scheme.
struct BuildTable {
    std::map<std::uint32_t, std::vector<std::string>> levels; // type code → scheme paths (game-relative)
    // sysMap vtbl[0xe8] GetSchemeEntry(type, n) (10020e80); nullptr past the end.
    const std::string* scheme(std::uint32_t type, std::size_t level) const;
    // sysMap vtbl[0xec] GetSchemeField(type) (10020ed0): the number of levels.
    std::size_t level_count(std::uint32_t type) const;
};
BuildTable parse_build_table(std::string_view text);

// The economy parameters of a placed data.tma object (MBehaviour IParameters 1000b470; records
// {u32 kind (0 float, 1 int), value, default, max}; tools/tma_props.py).
struct ObjectParams {
    std::optional<float> current_ore, maximum_ore; // attribute 0x2000100 value / aux
    std::uint32_t free_bots = 0;      // FreeBotNum → MB+0x9ec (property 0x803)
    std::uint32_t free_techno = 0;    // FreeTechnoNum → MB+0x9f4 (property 0x804)
    float free_research_time = 2.0f;  // FreeResearchTime → MB+0x9f8: seconds per research item
};
ObjectParams object_params(const mission::TmaObject& o);

// A mineral record of the system areal map (slot 0x64 count / 0x68(i) → {x, y, z, flag (+0xc), ?, amount
// (+0x14)}). [inferred] They are the data.tma extras (28-byte records after the description: xyz,
// then raw[0] = flag, raw[1] = 0x10001000 on mineral fields (the HQ's mineral-search mask), raw[2] =
// amount (float), raw[3]); iron3d keeps them at mission+0x700 (MineralFieldAt 10073350).
struct Lode {
    Vec3 pos{};
    std::uint32_t flag = 0; // field 3: set when a mine claims the lode (M_Task_Mine::SetTarget)
    std::uint32_t kind = 0; // raw[1]
    float amount = 0;       // field +0x14
};
std::vector<Lode> lodes_from_tma(const mission::Tma& tma);

// Robot size from the root entry (M_Task_Construct::StartTask 10029ba0): units `entry[2]` t → 1,
// l/h → 2, m → 3, b/e → 4; buildings `entry[3]` t 1, l 2, m 3, b/e 4; else 0. Case-insensitive.
std::uint32_t robot_size(const mission::Scheme& s);

// A building's size class (property 0x201, MB+0x960: the plant limit). [inferred] The writer of MB+0x960
// was not traced; the FORT root's size letter is used (fr_l_plant 2, fr_m_plant 3, fr_b_plant 4), which
// matches the plant sizes 2..4 of the free-bot time table and "buildings: 2..5" of property 0x201.
inline std::uint32_t building_size_class(const mission::Scheme& s) { return robot_size(s); }

// Loads and caches scheme files by game-relative path (case-insensitive lookup through resolve_path).
class SchemeCache {
public:
    explicit SchemeCache(std::filesystem::path game_dir) : game_(std::move(game_dir)) {}
    // nullptr when the file is missing or malformed.
    const mission::Scheme* get(std::string_view path);
    // Adds or replaces an in-memory scheme (e.g. UNITS\tempbld.dat written by the HQ build screen,
    // bld_unit_<lid>.dat of the factory screen).
    void put(std::string_view path, mission::Scheme s);
    const std::filesystem::path& game_dir() const { return game_; }

private:
    std::filesystem::path game_;
    std::map<std::string, std::optional<mission::Scheme>> cache_;
};

// Σ battery capacity of a design: every scheme node whose objects.rlb record is an INTO module with a
// CTLD (record +0x100 library, +0x120 entry, e.g. intsys.rlb o_pws_b_01.ctl) contributes its type-0x13
// devices' property 0 (CDevice::GetParam 10021d00: header float +0x2c + 4·n; CControlBase::CreateDevice
// 1002d4b0 case 0x13). The lookup resolves (library, entry) to the entry bytes (nullptr = missing).
using ResourceLookup = std::function<std::span<const std::uint8_t>(std::string_view lib, std::string_view entry)>;
float battery_capacity(const mission::Scheme& s, const ResourceLookup& lookup);

std::string normalize_path(std::string_view p); // upper case, '/' → '\'

} // namespace parkan::economy
