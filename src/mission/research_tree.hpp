// Clan research tree (MISSIONS/SCRIPTS/<name>.trf). Spec: notes/research-tree.md.
// Original loader: MisLoad.dll CResearch::LoadTree (10002fe0).
#pragma once

#include "core/bytes.hpp"

namespace parkan::mission {

struct ResearchItem {
    // TRF0 costs [confirmed, designs-research.md §2]: research (power +0x00, ore +0x04) at an institute,
    // build (power +0x08, ore +0x0c) per scheme node at a plant or by a builder.
    float research_power = 0, research_ore = 0;
    float build_power = 0, build_ore = 0;
    std::string code, name, description, stats;     // CP1251 as stored
    std::uint32_t kind = 0;  // low16 = model index, byte3 = category (8 building, 9 tower, 0xa clip, 0xc weapon)
    // TRF0 +0x24 bytes: +0x24 sub-type, +0x25 (open), +0x26 size class, +0x27 mark level (designs-research.md §2).
    std::uint32_t extra = 0;
    std::uint8_t initial_state = 0; // 0 locked, 7 available at mission start
    std::vector<std::uint32_t> prerequisites;
    std::vector<std::uint32_t> unlocks;

    std::uint16_t model_index() const { return std::uint16_t(kind & 0xFFFF); }
    std::uint8_t category() const { return std::uint8_t(kind >> 24); }
};

struct ResearchTree {
    std::vector<ResearchItem> items;
    std::vector<std::string> models;     // TRF6
    std::vector<std::uint32_t> model_ids; // TRFB, one per model (meaning open)
    bool trf1_flag = false;              // TRF1 attr2 == 1
};

ResearchTree load_research_tree(const std::filesystem::path& path);

} // namespace parkan::mission
