// Unit and building designs ("schemes"): UNITS/**/*.dat. Reader: ArealMap CreateObjectFromScheme
// (10014fd0) with the recursive node reader 10016a10.
//
//   u32 magic 0xF0F1, u32 flags, then one node:
//   node = { char lib[32]; char entry[32]; u32 one; i32 mount; char name[32]; u32 category;
//            u32 child_count; node children[child_count]; }        (0x6c bytes + count + children)
//
// `entry` is an objects.rlb prototype. `mount` is -1 on the root; on an external component (EXTO)
// it is the parent mesh node it is attached to, on an internal one (INTO) an internal slot.
// `category` is not a component type: the same part has different values in different files.
#pragma once

#include "core/bytes.hpp"

namespace parkan::mission {

struct SchemeNode {
    std::string lib, entry, name; // name is UTF-8
    std::uint32_t one = 0;
    std::int32_t mount = -1;
    std::uint32_t category = 0;
    std::vector<SchemeNode> children;
};

struct Scheme {
    std::uint32_t flags = 0;
    SchemeNode root;
};

Scheme decode_scheme(std::span<const std::uint8_t> data);
// The writer (the HQ build screen's UNITS\tempbld.dat, the factory's bld_unit_<LogicID>.dat; designs-research.md
// §1): the same layout, strings NUL-padded to 32 bytes (names back to CP1251). Throws on a string that does
// not fit in 31 bytes.
Bytes encode_scheme(const Scheme& s);

} // namespace parkan::mission
