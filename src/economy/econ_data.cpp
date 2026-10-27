#include "economy/econ_data.hpp"

#include "core/paths.hpp"
#include "game/world.hpp"
#include "res/ctld.hpp"
#include "res/library.hpp"
#include "res/nres.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

namespace parkan::economy {

namespace {

char lower(char c) { return char(std::tolower((unsigned char)c)); }

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

} // namespace

std::string normalize_path(std::string_view p) {
    std::string r(p);
    for (char& c : r) c = c == '/' ? '\\' : char(std::toupper((unsigned char)c));
    return r;
}

std::map<std::string, Profession> load_professions(const std::filesystem::path& behpsp) {
    auto arc = NResArchive::open(behpsp);
    std::map<std::string, Profession> out;
    for (const auto& e : arc.entries()) {
        std::string name = e.name;
        for (char& c : name) c = lower(c);
        if (name.rfind("prof_", 0) != 0) continue;
        if (name.size() > 4 && name.ends_with(".var")) name.resize(name.size() - 4);
        // u32 count; records {u32 size, kind, reserved, nameLen; name; u32 flags, value, default, min, max}.
        Reader r(e.data);
        const std::uint32_t n = r.u32();
        Profession p;
        for (std::uint32_t i = 0; i < n; ++i) {
            r.u32();
            const std::uint32_t kind = r.u32();
            r.u32();
            const std::string field = r.str(r.u32());
            r.u32();
            const std::uint32_t value = r.u32();
            r.u32(), r.u32(), r.u32();
            if (kind != 3) continue;
            const float f = std::bit_cast<float>(value);
            if (field == "Store_Ore_Maximum") p.store_ore_max = f;
            else if (field == "Store_Ore_Now") p.store_ore_now = f;
            else if (field == "Transfer_Ore_OnBoard") p.ore_onboard = f;
            else if (field == "Transfer_Ore_OffBoard") p.ore_offboard = f;
            else if (field == "Transfer_Power_In") p.power_in = f;
            else if (field == "Transfer_Power_Out") p.power_out = f;
            else if (field == "Use_Power") p.use_power = f;
            else if (field == "Use_Ore") p.use_ore = f;
        }
        out[name] = p;
    }
    return out;
}

const char* profession_sheet(std::uint32_t type) {
    using namespace game;
    switch (type) {
    case kTypeExplorer: return "prof_exp";
    case kTypeTransport: return "prof_trn";
    case kTypeBuilder: return "prof_bld";
    case kTypeWarbot: return "prof_war";
    case kTypeHq: return "prof_hq";
    case kTypeHero: return "prof_hero";
    case kTypeAnimal: return "prof_animal";
    case kTypeGenerator: return "prof_generator";
    case kTypeMine: return "prof_mine";
    case kTypeStorage: return "prof_storage";
    case kTypePlant: return "prof_plant";
    case kTypeHangar: return "prof_angar";
    case kTypeTeleport: return "prof_mtp";
    case kTypeInstitute: return "prof_institute";
    case kTypeBunkerSmall:
    case kTypeBunkerMedium:
    case kTypeBunkerLarge: return "prof_bunker";
    case kTypeTowerMedium:
    case kTypeTowerLarge: return "prof_tower";
    default: return (type & kBuildingBit) ? "prof_universal" : "prof_war";
    }
}

const std::string* BuildTable::scheme(std::uint32_t type, std::size_t level) const {
    auto it = levels.find(type);
    return it != levels.end() && level < it->second.size() ? &it->second[level] : nullptr;
}

std::size_t BuildTable::level_count(std::uint32_t type) const {
    auto it = levels.find(type);
    return it == levels.end() ? 0 : it->second.size();
}

BuildTable parse_build_table(std::string_view text) {
    using namespace game;
    // The type names of BuildDat.lst in CSystemArealMap's table order.
    static const std::pair<const char*, std::uint32_t> kNames[] = {
        {"Bunker_Small", kTypeBunkerSmall}, {"Bunker_Medium", kTypeBunkerMedium}, {"Bunker_Large", kTypeBunkerLarge},
        {"Generator", kTypeGenerator},      {"Mine", kTypeMine},                   {"Storage", kTypeStorage},
        {"Plant", kTypePlant},              {"Hangar", kTypeHangar},               {"MainTeleport", kTypeTeleport},
        {"Institute", kTypeInstitute},      {"Tower_Medium", kTypeTowerMedium},    {"Tower_Large", kTypeTowerLarge},
    };
    BuildTable t;
    std::vector<std::string>* cur = nullptr;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        while (!line.empty() && std::isspace((unsigned char)line.front())) line.remove_prefix(1);
        while (!line.empty() && std::isspace((unsigned char)line.back())) line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        if (line.front() == '"') {
            const auto end = line.find('"', 1);
            if (cur && end != std::string_view::npos) cur->push_back(std::string(line.substr(1, end - 1)));
            continue;
        }
        cur = nullptr;
        const std::string_view name = line.substr(0, line.find_first_of(" \t"));
        for (const auto& [n, code] : kNames)
            if (iequal(name, n)) cur = &t.levels[code];
    }
    return t;
}

ObjectParams object_params(const mission::TmaObject& o) {
    ObjectParams p;
    for (const auto& prop : o.properties) {
        const std::uint32_t v = prop.raw[1];
        const float f = prop.raw[0] == 1 ? float(v) : std::bit_cast<float>(v);
        if (prop.name == "CurrentOre") p.current_ore = f;
        else if (prop.name == "MaximumOre") p.maximum_ore = f;
        else if (prop.name == "FreeBotNum") p.free_bots = std::uint32_t(f);
        else if (prop.name == "FreeTechnoNum") p.free_techno = std::uint32_t(f);
        else if (prop.name == "FreeResearchTime") p.free_research_time = f;
    }
    return p;
}

std::vector<Lode> lodes_from_tma(const mission::Tma& tma) {
    std::vector<Lode> out;
    for (const auto& e : tma.extras)
        out.push_back({{e.position[0], e.position[1], e.position[2]}, e.raw[0], e.raw[1], std::bit_cast<float>(e.raw[2])});
    return out;
}

std::uint32_t robot_size(const mission::Scheme& s) {
    const std::string& e = s.root.entry;
    if (s.flags & game::kBuildingBit) {
        if (e.size() < 4) return 0;
        switch (lower(e[3])) {
        case 't': return 1;
        case 'l': return 2;
        case 'm': return 3;
        case 'b': case 'e': return 4;
        default: return 0;
        }
    }
    if (e.size() < 3) return 0;
    switch (lower(e[2])) {
    case 't': return 1;
    case 'l': case 'h': return 2;
    case 'm': return 3;
    case 'b': case 'e': return 4;
    default: return 0;
    }
}

std::uint32_t building_size_class(const mission::Scheme& s) { return game::building_size_class(s.root.entry); }

const mission::Scheme* SchemeCache::get(std::string_view path) {
    const std::string key = normalize_path(path);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        std::optional<mission::Scheme> s;
        if (auto p = resolve_path(game_, key)) {
            try {
                s = mission::decode_scheme(read_file(*p));
            } catch (const std::exception&) {
            }
        }
        it = cache_.emplace(key, std::move(s)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void SchemeCache::put(std::string_view path, mission::Scheme s) { cache_[normalize_path(path)] = std::move(s); }

float battery_capacity(const mission::Scheme& s, const ResourceLookup& lookup) {
    float sum = 0;
    const std::function<void(const mission::SchemeNode&)> walk = [&](const mission::SchemeNode& n) {
        if (n.one == 0) return; // "counts" == 0: the node and its subtree are skipped
        const auto rec = lookup(n.lib, n.entry);
        if (rec.size() >= 0x140) {
            const auto field = [&](std::size_t off) {
                std::string v(reinterpret_cast<const char*>(rec.data() + off), 32);
                v.resize(std::strlen(v.c_str()));
                return v;
            };
            const std::string lib = field(0x100), ctl = field(0x120);
            if (ctl.size() > 4 && iequal(std::string_view(ctl).substr(ctl.size() - 4), ".ctl")) {
                const auto data = lookup(lib, ctl);
                if (!data.empty()) {
                    try {
                        for (const auto& d : decode_ctld(data).devices)
                            if (d.type == 0x13) {
                                float cap = 0;
                                std::memcpy(&cap, d.header.data() + 0x2c, 4);
                                if (cap > 0) sum += cap;
                            }
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(s.root);
    return sum;
}

} // namespace parkan::economy

namespace parkan::economy {

std::vector<combat::PowerDevice> power_devices(const mission::Scheme& s, AssetLibrary& lib) {
    std::vector<combat::PowerDevice> out;
    auto field = [](const NResEntry& e, std::size_t off) {
        std::string v(reinterpret_cast<const char*>(e.data.data() + off), 32);
        v.resize(std::strlen(v.c_str()));
        return v;
    };
    // The CTLD of an objects.rlb BTLU / EXTO / INTO record (+0x100 library, +0x120 entry).
    auto ctld_of = [&](const NResEntry* e) -> std::optional<Ctld> {
        if (!e || e->data.size() < 0x140) return std::nullopt;
        const NResEntry* c = lib.find(field(*e, 0x100), field(*e, 0x120));
        if (!c || c->type != fourcc("CTLD")) return std::nullopt;
        try {
            return decode_ctld(c->data);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    auto load = [](const CtldDevice& d, combat::PowerDevice& pd) {
        std::int32_t st = -1;
        std::memcpy(&pd.type, d.header.data(), 4);
        std::memcpy(&st, d.header.data() + 0x18, 4);
        std::memcpy(&pd.power, d.header.data() + 0x20, 4);
        std::memcpy(pd.param.data(), d.header.data() + 0x2c, sizeof pd.param);
        pd.state = combat::PowerDevice::initial_state(pd.cls, st);
    };
    const auto walk = [&](auto&& self, const mission::SchemeNode& n, const NResEntry* rec) -> void {
        const std::size_t first = out.size();
        std::size_t count = 0;
        if (auto c = ctld_of(rec)) {
            for (const auto& d : c->devices) {
                combat::PowerDevice pd;
                pd.cls = d.type;
                load(d, pd);
                out.push_back(pd);
            }
            count = c->devices.size();
        }
        for (const auto& ch : n.children) {
            const NResEntry* e = lib.find("objects.rlb", ch.entry);
            if (!e) continue;
            if (e->type == fourcc("EXTO")) {
                if (ch.mount >= 0) self(self, ch, e);
                continue;
            }
            if (e->type != fourcc("INTO") || ch.one == 0 || ch.mount < 0 || std::size_t(ch.mount) >= count) continue;
            if (auto c = ctld_of(e); c && !c->devices.empty()) load(c->devices[0], out[first + std::size_t(ch.mount)]);
        }
    };
    const NResEntry* root = lib.find("objects.rlb", s.root.entry);
    if (root && root->type == fourcc("FORT") && root->data.size() >= 0x40) root = lib.find(field(*root, 0x00), field(*root, 0x20));
    walk(walk, s.root, root);
    return out;
}

} // namespace parkan::economy
