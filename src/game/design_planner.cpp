#include "game/design_planner.hpp"

#include "core/paths.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace parkan::game {

namespace {

DesignEntry evaluate(const std::string& file, const mission::Scheme& s, const economy::Research* research) { // 10010c30
    DesignEntry d;
    d.file = file;
    d.type = s.flags;
    d.size = economy::robot_size(s);
    d.armour = kPlaceholderArmour * float(std::max<std::uint32_t>(d.size, 1));
    d.speed = 5.f - float(d.size);
    const std::function<void(const mission::SchemeNode&)> walk = [&](const mission::SchemeNode& n) {
        if (n.one == 0) return;
        const mission::ResearchItem* it = research ? research->item(research->find_item(n.entry)) : nullptr;
        const bool gun = it ? it->category() == 0x0c : n.entry.rfind("e_gun", 0) == 0;
        if (gun) {
            const float sz = it ? float((it->extra >> 16) & 0xff) : 0.f; // TRF0 +0x26 size class
            d.weapon += 35000.f * (sz + 1);
            d.range = std::max(d.range, sz + 1);
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(s.root);
    return d;
}

} // namespace

void DesignPlanner::load(economy::Economy& eco, const economy::Research* research) {
    entries.clear();
    selected = -1;
    std::vector<std::string> files;
    if (auto dir = resolve_path(eco.schemes().game_dir(), "UNITS/UNITS/AI")) {
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(*dir, ec)) {
            std::string name = f.path().filename().string();
            std::string up = economy::normalize_path(name);
            if (up.size() > 4 && up.ends_with(".DAT")) files.push_back("UNITS\\UNITS\\AI\\" + name);
        }
    }
    // FindFirstFile order on the original's file system: by name, case-insensitive [inferred].
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return economy::normalize_path(a) < economy::normalize_path(b);
    });
    for (const auto& f : files)
        if (const mission::Scheme* s = eco.schemes().get(f)) entries.push_back(evaluate(f, *s, research));
    loaded = true;
    if (research) select_cheapest_research(eco, *research);
}

void DesignPlanner::select_cheapest_research(economy::Economy& eco, const economy::Research& research) {
    all_available = true;
    selected = -1;
    float best = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        DesignEntry& d = entries[i];
        if (d.available) continue;
        const mission::Scheme* s = eco.schemes().get(d.file);
        if (!s) {
            all_available = false;
            continue;
        }
        d.available = research.build_cost(*s).ok; // IsBuildable (10011530)
        if (d.available) continue;
        all_available = false;
        if (!research.all_items_available(*s)) continue; // AreAllItemsAvailable (100115f0)
        const auto c = research.full_research_cost(*s);  // CalcFullResearchCost
        if (!c) continue;
        if (selected < 0 || c->ore < best) selected = std::int32_t(i), best = c->ore;
    }
}

std::optional<std::size_t> DesignPlanner::select_best(std::uint32_t type, std::uint32_t mode, std::uint32_t rnd) const {
    std::vector<std::pair<float, std::size_t>> cand;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const DesignEntry& d = entries[i];
        if (!d.available || d.type != type || (d.type & kBuildingBit)) continue;
        float v = 0;
        switch (mode) {
        case 1: v = d.weapon; break;
        case 2: v = d.armour; break;
        case 3: v = d.range; break;
        case 4: v = d.speed; break;
        default: v = combat_hits(d.armour, d.weapon); break; // 5 / 6
        }
        cand.push_back({v, i});
    }
    if (cand.empty()) return std::nullopt;
    std::stable_sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (mode == 6) {
        for (auto& [v, i] : cand)
            if (entries[i].size < 3) return i;
        return cand.back().second;
    }
    std::size_t k = rnd % (spread + 1);
    if (k >= cand.size()) k = cand.size() - 1;
    return cand[k].second;
}

void DesignPlanner::refresh_lodes(const World& world, const economy::Economy& eco) {
    lodes.clear();
    for (const auto& l : eco.lodes()) {
        LodeRecord r;
        r.flag = l.flag != 0;
        r.pos = l.pos;
        for (const auto& o : world.objects()) {
            if (!o.alive || o.type != kTypeMine) continue;
            const Vec3 p = o.position();
            if (std::hypot(p.x - l.pos.x, p.y - l.pos.y) > 250.f || std::fabs(p.x - l.pos.x) > 150.f) continue;
            r.has_mine = true, r.mine_clan = o.clan, r.mine = o.logic_id;
            break;
        }
        lodes.push_back(r);
    }
}

} // namespace parkan::game
