// Clan research tree (MISSIONS/SCRIPTS/<name>.trf). Spec: notes/research-tree.md.
// Original loader: MisLoad.dll CResearch::LoadTree (10002fe0).
#pragma once

#include "core/bytes.hpp"

namespace parkan::mission {

struct ResearchItem {
    float research_time = 0, research_cost = 0;     // (inferred) 0,0 = needs no research
    float production_time = 0, production_cost = 0; // (inferred)
    std::string code, name, description, stats;     // CP1251 as stored
    std::uint32_t kind = 0;  // low16 = model index, byte3 = category (8 building, 9 tower, 0xa clip, 0xc weapon)
    std::uint32_t extra = 0; // meaning open
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
