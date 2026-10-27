#include "game/world.hpp"

#include "core/paths.hpp"
#include "mission/scheme.hpp"

#include <algorithm>
#include <cctype>

namespace parkan::game {

namespace {

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper((unsigned char)c));
    std::replace(r.begin(), r.end(), '/', '\\');
    return r;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

} // namespace

std::uint32_t unit_size_class(std::string_view entry) {
    if (entry.size() < 3) return 0;
    switch (std::tolower((unsigned char)entry[2])) {
    case 't': return 1;
    case 'h': case 'l': return 2;
    case 'm': return 3;
    case 'b': return 4;
    default: return 0;
    }
}

std::uint32_t building_size_class(std::string_view entry) {
    if (entry.size() < 4) return 0;
    switch (std::tolower((unsigned char)entry[3])) {
    case 'b': return 4;
    case 'e': return 5;
    case 'l': return 2;
    case 'm': return 3;
    default: return 0;
    }
}

std::uint32_t level_from_size_class(std::uint32_t size_class) {
    switch (size_class) {
    case 3: return 1;
    case 4: return 2;
    case 5: return 3;
    default: return 0;
    }
}

bool TacticalAreal::contains(float x, float y) const {
    bool in = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto &a = poly[i], &b = poly[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}

std::uint32_t type_code_for_resource(std::string_view resource) {
    const std::string r = upper(resource);
    const auto folder_of = [&](std::string_view root) -> std::string {
        const auto at = r.find(root);
        if (at == std::string::npos) return {};
        const auto start = at + root.size();
        const auto end = r.find('\\', start);
        return end == std::string::npos ? std::string() : r.substr(start, end - start);
    };
    const std::string file = r.substr(r.find_last_of('\\') == std::string::npos ? 0 : r.find_last_of('\\') + 1);
    if (const std::string b = folder_of("UNITS\\BUILDS\\"); !b.empty()) {
        // BuildDat.lst: sbunk/mbunk/lbunk, mtow/ltow; the other folders hold one building type each.
        if (b == "BUNKER") return file[0] == 'L' ? kTypeBunkerLarge : file[0] == 'M' ? kTypeBunkerMedium : kTypeBunkerSmall;
        if (b == "TOWER") return file[0] == 'L' ? kTypeTowerLarge : kTypeTowerMedium;
        if (b == "GENER") return kTypeGenerator;
        if (b == "MINE") return kTypeMine;
        if (b == "STORAGE") return kTypeStorage;
        if (b == "PLANT") return kTypePlant;
        if (b == "HANGAR") return kTypeHangar;
        if (b == "TELEMAIN" || b == "TELEPORT") return kTypeTeleport; // both prof_mtp (inferred for TELEPORT)
        if (b == "INSTITUT") return kTypeInstitute;
        if (b == "BRIDGE") return kTypeBridge;
        if (b == "RUIN") return kTypeRuins;
        return kBuildingBit; // MAST: no type code in Behavior.dll's profession switch
    }
    if (const std::string u = folder_of("UNITS\\UNITS\\"); !u.empty()) {
        if (u == "ANIMAL") return kTypeAnimal;
        if (u == "HERO") return kTypeHero;
        if (u == "BUILDER") return kTypeBuilder;
        if (u == "TRANSPRT") return kTypeTransport;
        if (u == "HQ") return kTypeHq;
        return kTypeWarbot; // BATTLE, AI, PREBLD, AutoDEMO
    }
    if (r.find("UNITS\\AUTO\\") != std::string::npos) return kTypeWarbot;
    // objects.rlb fortification keys (fr_<size>_<kind>).
    if (starts_with(r, "FR_")) {
        const std::string k = r.size() > 5 ? r.substr(5) : std::string();
        if (starts_with(k, "PLANT")) return kTypePlant;
        if (starts_with(k, "GENER")) return kTypeGenerator;
        if (starts_with(k, "MINE")) return kTypeMine;
        if (starts_with(k, "STORE")) return kTypeStorage;
        if (starts_with(k, "ANGAR")) return kTypeHangar;
        if (starts_with(k, "INST")) return kTypeInstitute;
        if (starts_with(k, "MTP")) return kTypeTeleport;
        if (starts_with(k, "BRIGE")) return kTypeBridge;
        if (starts_with(k, "RUIN")) return kTypeRuins;
        if (starts_with(k, "TOWH")) return kTypeTowerLarge;
        if (starts_with(k, "TOWL")) return kTypeTowerMedium;
        if (starts_with(k, "BUNKER")) return r[3] == 'B' ? kTypeBunkerLarge : r[3] == 'M' ? kTypeBunkerMedium : kTypeBunkerSmall;
        return kBuildingBit;
    }
    return 0;
}

World World::from_tma(const mission::Tma& tma, const std::filesystem::path* game_dir) {
    World w;
    for (const auto& c : tma.clans) {
        WorldClan wc;
        wc.name = c.name;
        wc.mode = c.mode;
        wc.script = c.script.substr(c.script.find_last_of("\\/") == std::string::npos ? 0 : c.script.find_last_of("\\/") + 1);
        wc.research = c.research;
        wc.base_x = c.anchor_x;
        wc.base_y = c.anchor_y;
        wc.migration = c.spatial;
        wc.brains = c.research_tag > 0 ? std::uint32_t(c.research_tag) : 0;
        w.clans.push_back(std::move(wc));
    }
    // Relations by clan name. Unlisted pairs stay 1 (neither hostile nor allied; inferred: every shipped
    // clan lists the others).
    for (std::size_t i = 0; i < tma.clans.size(); ++i) {
        w.clans[i].relations.assign(tma.clans.size(), 1);
        for (const auto& rel : tma.clans[i].relations)
            for (std::size_t j = 0; j < tma.clans.size(); ++j)
                if (tma.clans[j].name == rel.clan) w.clans[i].relations[j] = std::uint32_t(rel.value);
    }
    for (const auto& p : tma.paths) {
        TacticalAreal a;
        a.id = p.id;
        for (const auto& pt : p.points) a.poly.push_back({pt[0], pt[1]});
        w.tactical.push_back(std::move(a));
    }
    for (std::size_t i = 0; i < tma.objects.size(); ++i) {
        const auto& o = tma.objects[i];
        if (o.kind == 2 || o.identity == kNoObject) continue; // statics (trees, rocks) have no LogicID
        WorldObject wo;
        wo.logic_id = o.identity;
        wo.clan = o.ai_index < tma.clans.size() ? o.ai_index : kNoClan;
        wo.resource = o.resource;
        wo.tma_index = std::int32_t(i);
        wo.placed = {o.position[0], o.position[1], o.position[2]};
        wo.type = type_code_for_resource(o.resource);
        if (wo.building()) wo.type |= kBuildingBit;
        wo.armour = kPlaceholderArmour;
        wo.weapon = (wo.type & kClassRobot) ? kPlaceholderWeapon : 0.f;
        if (game_dir) {
            if (auto p = resolve_path(*game_dir, o.resource)) {
                try {
                    const std::string entry = mission::decode_scheme(read_file(*p)).root.entry;
                    wo.size_class = wo.building() ? building_size_class(entry) : unit_size_class(entry);
                } catch (const std::exception&) {
                }
            }
        }
        w.add(std::move(wo));
    }
    return w;
}

WorldObject* World::find(std::uint32_t logic_id) {
    auto it = index_.find(logic_id);
    return it == index_.end() ? nullptr : &objects_[it->second];
}

const WorldObject* World::find(std::uint32_t logic_id) const {
    auto it = index_.find(logic_id);
    return it == index_.end() ? nullptr : &objects_[it->second];
}

const WorldObject* World::live(std::uint32_t logic_id) const {
    const WorldObject* o = find(logic_id);
    return o && o->alive ? o : nullptr;
}

WorldObject& World::add(WorldObject o) {
    if (auto it = index_.find(o.logic_id); it != index_.end()) {
        objects_[it->second] = std::move(o);
        return objects_[it->second];
    }
    index_[o.logic_id] = objects_.size();
    objects_.push_back(std::move(o));
    return objects_.back();
}

void World::attach(std::uint32_t logic_id, UnitControl* control) {
    if (WorldObject* o = find(logic_id)) o->control = control;
}

void World::kill(std::uint32_t logic_id) {
    if (WorldObject* o = find(logic_id)) {
        o->placed = o->position(); // the control may go away with the unit
        o->alive = false;
        o->control = nullptr;
    }
}

void World::set_clan(std::uint32_t logic_id, std::uint32_t clan) {
    if (WorldObject* o = find(logic_id)) o->clan = clan;
}

bool World::in_tactical_areal(std::uint32_t areal, std::uint32_t logic_id) const {
    const WorldObject* o = live(logic_id);
    if (!o || o->building()) return false; // the system map tracks robots per tactical areal
    const Vec3 p = o->position();
    for (const auto& a : tactical)
        if (a.id == std::int32_t(areal)) return a.contains(p.x, p.y);
    return false;
}

} // namespace parkan::game
