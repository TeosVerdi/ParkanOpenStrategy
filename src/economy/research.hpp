// A clan's research tree at run time: MisLoad CResearchTree (the IResearch, iid 0x502; ctor 100028d0,
// vtable 1000e18c) over the loaded .trf, plus the scheme walks that read it: SchemeNode_SumBuildCost
// (Behavior 10029810), CalcFullResearchCost (ArealMap 10016280), MResearchCenter::CreateTechArray
// (Behavior 10023790) and the ai.dll AiScheme checks (IsSchemeFullyResearched 1000f7a0,
// AreAllItemsAvailable 100115f0, IsBuildable 10011530). Spec: notes/designs-research.md §1–§6.
//
// Network sync (SetProgress queueing, GetSyncData/ApplySyncData) is not modelled.
#pragma once

#include "mission/research_tree.hpp"
#include "mission/scheme.hpp"

#include <optional>

namespace parkan::economy {

inline constexpr std::uint8_t kResAvailable = 1, kResResearched = 2, kResPresent = 4; // TRF1 state bits

// IResearch slot 3 GetItemInfo's out record {researched, available, present, resPower, resOre,
// buildPower, buildOre, model}.
struct ResearchInfo {
    bool researched = false, available = false, present = false;
    float research_power = 0, research_ore = 0; // TRF0 +0x00 / +0x04
    float build_power = 0, build_ore = 0;       // TRF0 +0x08 / +0x0c
};

struct SchemeCost {
    float power = 0, ore = 0;
    bool ok = true; // every counted node's item found, researched and present
};

class Research {
public:
    Research() = default;
    explicit Research(mission::ResearchTree tree);

    std::size_t item_count() const { return tree_.items.size(); } // slot 8
    bool valid(std::int32_t item) const { return item >= 0 && std::size_t(item) < tree_.items.size(); } // slot 20
    // Slot 2 FindItemByModel (10002a40): TRFB in order, case-insensitive, first match; -1 if none.
    std::int32_t find_item(std::string_view model) const;
    std::optional<ResearchInfo> info(std::int32_t item) const; // slot 3
    std::uint8_t state(std::int32_t item) const { return valid(item) ? state_[std::size_t(item)] : 0; }
    float progress(std::int32_t item) const { return valid(item) ? progress_[std::size_t(item)] : 0.f; } // slot 4
    bool set_progress(std::int32_t item, float p); // slot 5
    // Slot 7 MarkResearched (10002c10): only a present item: state |= 3; then every present, not-available
    // item whose prerequisites are all present + researched becomes available.
    bool mark_researched(std::int32_t item);
    const std::vector<std::uint32_t>& prerequisites(std::int32_t item) const; // slot 9
    const std::vector<std::uint32_t>& unlocks(std::int32_t item) const;      // slot 10
    const mission::ResearchItem* item(std::int32_t i) const { return valid(i) ? &tree_.items[std::size_t(i)] : nullptr; }
    const mission::ResearchTree& tree() const { return tree_; }

    // SchemeNode_SumBuildCost (10029810): Σ build (power, ore) over the counted nodes (root included,
    // duplicates included); ok = every node's item found, researched and present.
    SchemeCost build_cost(const mission::Scheme& s) const;
    // CalcFullResearchCost (10016280): Σ research (power, ore) of the unresearched items of the scheme and,
    // for those not available, their unresearched prerequisites (transitively). nullopt = a model is missing.
    std::optional<SchemeCost> full_research_cost(const mission::Scheme& s) const;
    // MResearchCenter::CreateTechArray (10023790): the same item set, in walk order.
    std::vector<std::int32_t> tech_array(const mission::Scheme& s) const;
    // IsSchemeFullyResearched (ai 1000f7a0): no counted node's item is unresearched (missing models ignored).
    bool fully_researched(const mission::Scheme& s) const;
    // AiScheme::AreAllItemsAvailable (100115f0): every node item present and (researched or available).
    bool all_items_available(const mission::Scheme& s) const;

private:
    void collect(const mission::SchemeNode& n, std::vector<std::int32_t>& out, bool& missing) const;
    void add_prerequisites(std::int32_t item, std::vector<std::int32_t>& out) const;

    mission::ResearchTree tree_;
    std::vector<std::uint8_t> state_;
    std::vector<float> progress_;
};

} // namespace parkan::economy
