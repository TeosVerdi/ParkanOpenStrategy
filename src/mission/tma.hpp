// Mission placement: MISSIONS/.../data.tma. Spec: FParkan reference/tma and tomes/04-world
// ("Clans", "PlacedObject"). Native reader: MisLoad.dll (objects at FUN_10003900).
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan::mission {

struct TmaPath {
    std::int32_t id = 0; // referenced by id, not by position
    std::vector<std::array<float, 3>> points;
};

struct TmaRelation {
    std::string clan;
    std::int32_t value = 0;
};

struct TmaClan {
    std::string name;
    std::int32_t raw_id = 0;
    float anchor_x = 0, anchor_y = 0; // base place
    std::uint32_t mode = 0;
    std::string script;  // mode 1..3: MISSIONS\SCRIPTS\<bundle> (no extension)
    std::int32_t script_tag = 0;
    std::string research; // mode 1..3: MISSIONS\SCRIPTS\<name>.trf, or empty
    std::int32_t research_tag = 0;
    std::vector<std::array<float, 5>> spatial; // mode 0 only (raw)
    std::vector<TmaRelation> relations;
};

struct TmaProperty {
    std::array<std::uint32_t, 4> raw{};
    std::string name;
};

struct TmaObject {
    std::uint32_t kind = 0;
    std::uint32_t class_flags = 0; // 0x80000002 in every shipped object
    std::string resource;          // objects.rlb prototype key, or a unit DAT path
    std::uint32_t ai_index = 0;    // the original's "raw_after_resource": selects GetSuperAI[n]
    std::uint32_t identity = 0;
    std::array<float, 3> position{}, orientation{}, scale{};
    std::string instance_name;
    std::uint32_t raw_after_name = 0;
    std::int32_t link0 = -1, link1 = -1;
    std::vector<TmaProperty> properties;
};

struct TmaExtra {
    std::array<float, 3> position{};
    std::array<std::uint32_t, 4> raw{};
};

struct Tma {
    std::vector<TmaPath> paths;
    std::vector<TmaClan> clans;
    std::vector<TmaObject> objects;
    std::string land_path; // e.g. DATA\MAPS\Tut_2\land
    std::uint32_t mission_flag = 0;
    std::string description; // raw bytes (may contain non-text)
    std::vector<TmaExtra> extras;
};

Tma decode_tma(std::span<const std::uint8_t> data);

} // namespace parkan::mission
