#include "economy/research.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

namespace parkan::economy {

namespace {

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

const std::vector<std::uint32_t> kNone;

} // namespace

Research::Research(mission::ResearchTree tree) : tree_(std::move(tree)) {
    // LoadTree copies TRF1 and does not recompute availability (every shipped tree is consistent).
    for (const auto& it : tree_.items) state_.push_back(it.initial_state);
    progress_.assign(tree_.items.size(), 0.f);
}

std::int32_t Research::find_item(std::string_view model) const {
    for (std::size_t m = 0; m < tree_.models.size() && m < tree_.model_ids.size(); ++m)
        if (iequal(tree_.models[m], model)) {
            const std::uint32_t item = tree_.model_ids[m] >> 16; // TRFB: u16 name offset | u16 item << 16
            return item < tree_.items.size() ? std::int32_t(item) : -1;
        }
    return -1;
}

std::optional<ResearchInfo> Research::info(std::int32_t item) const {
    if (!valid(item)) return std::nullopt;
    const auto& it = tree_.items[std::size_t(item)];
    const std::uint8_t s = state_[std::size_t(item)];
    return ResearchInfo{(s & kResResearched) != 0, (s & kResAvailable) != 0, (s & kResPresent) != 0,
                        it.research_power,         it.research_ore,            it.build_power,
                        it.build_ore};
}

bool Research::set_progress(std::int32_t item, float p) {
    if (!valid(item)) return false;
    progress_[std::size_t(item)] = p;
    return true;
}

bool Research::mark_researched(std::int32_t item) {
    if (!valid(item) || !(state_[std::size_t(item)] & kResPresent)) return false;
    state_[std::size_t(item)] |= kResAvailable | kResResearched;
    for (std::size_t i = 0; i < state_.size(); ++i) {
        if (!(state_[i] & kResPresent) || (state_[i] & kResAvailable)) continue;
        bool all = true;
        for (std::uint32_t p : tree_.items[i].prerequisites)
            all = all && (state_[p] & (kResPresent | kResResearched)) == (kResPresent | kResResearched);
        if (all) state_[i] |= kResAvailable;
    }
    return true;
}

const std::vector<std::uint32_t>& Research::prerequisites(std::int32_t item) const {
    return valid(item) ? tree_.items[std::size_t(item)].prerequisites : kNone;
}

const std::vector<std::uint32_t>& Research::unlocks(std::int32_t item) const {
    return valid(item) ? tree_.items[std::size_t(item)].unlocks : kNone;
}

SchemeCost Research::build_cost(const mission::Scheme& s) const {
    SchemeCost c;
    const std::function<void(const mission::SchemeNode&)> walk = [&](const mission::SchemeNode& n) {
        if (n.one == 0) return;
        const std::int32_t i = find_item(n.entry);
        const auto inf = info(i);
        if (!inf || !inf->researched || !inf->present) c.ok = false;
        if (inf) c.power += inf->build_power, c.ore += inf->build_ore;
        for (const auto& ch : n.children) walk(ch);
    };
    walk(s.root);
    return c;
}

// CSchemeNode_CollectResearchItems (10016ab0): each unresearched item once.
void Research::collect(const mission::SchemeNode& n, std::vector<std::int32_t>& out, bool& missing) const {
    if (n.one == 0) return;
    const std::int32_t i = find_item(n.entry);
    if (i < 0) missing = true;
    else if (!(state(i) & kResResearched) && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    for (const auto& c : n.children) collect(c, out, missing);
}

// CollectUnresearchedPrerequisites (10015fe0): recursion into the prerequisites that are not available.
void Research::add_prerequisites(std::int32_t item, std::vector<std::int32_t>& out) const {
    for (std::uint32_t p : prerequisites(item)) {
        const auto pi = std::int32_t(p);
        if ((state(pi) & kResResearched) || std::find(out.begin(), out.end(), pi) != out.end()) continue;
        out.push_back(pi);
        if (!(state(pi) & kResAvailable)) add_prerequisites(pi, out);
    }
}

std::vector<std::int32_t> Research::tech_array(const mission::Scheme& s) const {
    std::vector<std::int32_t> items;
    bool missing = false;
    collect(s.root, items, missing);
    const std::vector<std::int32_t> direct = items;
    for (std::int32_t i : direct)
        if (!(state(i) & kResAvailable)) add_prerequisites(i, items);
    return items;
}

std::optional<SchemeCost> Research::full_research_cost(const mission::Scheme& s) const {
    std::vector<std::int32_t> items;
    bool missing = false;
    collect(s.root, items, missing);
    if (missing) return std::nullopt;
    const std::vector<std::int32_t> direct = items;
    for (std::int32_t i : direct)
        if (!(state(i) & kResAvailable)) add_prerequisites(i, items);
    SchemeCost c;
    for (std::int32_t i : items) c.power += tree_.items[std::size_t(i)].research_power, c.ore += tree_.items[std::size_t(i)].research_ore;
    return c;
}

bool Research::fully_researched(const mission::Scheme& s) const {
    std::vector<std::int32_t> items;
    bool missing = false;
    collect(s.root, items, missing);
    return items.empty();
}

bool Research::all_items_available(const mission::Scheme& s) const {
    bool ok = true;
    const std::function<void(const mission::SchemeNode&)> walk = [&](const mission::SchemeNode& n) {
        if (n.one == 0) return;
        const std::uint8_t st = state(find_item(n.entry));
        if (!(st & kResPresent) || !(st & (kResResearched | kResAvailable))) ok = false;
        for (const auto& c : n.children) walk(c);
    };
    walk(s.root);
    return ok;
}

} // namespace parkan::economy
