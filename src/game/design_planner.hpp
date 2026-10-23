// A clan strategist's design planner (ai.dll, CSuperAI+0x40c, "P"): the AI design table (UNITS\UNITS\AI\*.dat,
// h43 LoadDesignList 100104e0), the research choice (SelectCheapestResearch 10010f90, h41 / h56), the design
// choice for TARGET_BY_NAME orders (SelectBestDesign 100107c0, h15 / h28, spread h69) and the mineral-lode
// table (10010e40, h18 / h40 / h53). Spec: notes/base-building.md §3.3 / §6, notes/designs-research.md §6,
// notes/script-handlers-b.md "Design planner".
//
// PORT APPROXIMATIONS (EvaluateDesign 10010c30 reads the statistics off a CreateObjectFromScheme object:
// armour = control property 0x36, speed = 0x91, weapon = property 0x204[1], range; the port has no control
// model to read them from, so they are derived from the scheme):
//   armour = kPlaceholderArmour · size class; weapon = 35000 · Σ over the gun nodes (research category 0x0c)
//   of (gun size class + 1); range = the largest gun size class + 1; speed = 5 − size class.
// The 105 000 weapon threshold of h65 therefore means "three small guns or one large gun" here.
// The lode table's "in the lode's areal" is taken as within 250 m (2-D) of the lode [port]; the x test
// (|mine.x − lode.x| ≤ 150) is kept as read. preload.lda is not shipped and not written.
#pragma once

#include "economy/economy.hpp"

namespace parkan::game {

struct DesignEntry { // 0x124 bytes
    std::string file;       // char[0x104], game-relative
    bool available = false; // +0x104
    std::uint32_t type = 0, size = 0; // +0x108 / +0x10c
    float armour = 0, weapon = 0, speed = 0, range = 0; // +0x110 / +0x114 / +0x118 / +0x11c
};

struct LodeRecord { // 0x18 bytes
    bool has_mine = false;   // +0
    bool flag = false;       // +1: lode field 3 != 0
    std::uint32_t mine_clan = kNoClan; // +4
    Vec3 pos{};              // +8..+0x10
    std::uint32_t mine = kNoObject;    // +0x14
};

class DesignPlanner {
public:
    // h43 → 100104e0: every UNITS\UNITS\AI\*.dat (name order), evaluated; then SelectCheapestResearch.
    void load(economy::Economy& eco, const economy::Research* research);
    // 10010f90: re-evaluates the unavailable designs; the one with the least research ore becomes `selected`.
    void select_cheapest_research(economy::Economy& eco, const economy::Research& research);
    // 100107c0: an available design of exactly `type` ranked by `mode` (1 weapon, 2 armour, 3 range,
    // 4 speed, 5/6 hits); mode 6 → the first with size < 3 (else the last), others → index
    // (rnd % (spread + 1)) clamped to the list. nullopt = none fits.
    std::optional<std::size_t> select_best(std::uint32_t type, std::uint32_t mode, std::uint32_t rnd) const;
    // 10010e40.
    void refresh_lodes(const World& world, const economy::Economy& eco);

    bool loaded = false;         // +0x24
    bool all_available = false;  // +0x25
    bool large_latch = false;    // +0x14 (h65)
    std::int32_t selected = -1;  // +0xc
    std::uint32_t spread = 0;    // +0x10 (h69)
    std::vector<DesignEntry> entries;
    std::vector<LodeRecord> lodes;
};

} // namespace parkan::game
