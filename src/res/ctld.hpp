// Motion graphs: NRes type CTLD (e.g. bases.rlb r_l_01.ctl). Loader: Control.dll
// CControlBase::LoadControlData (10008b10). Spec: notes/control-physics.md "The motion graph";
// reference parser: tools/ctl_dump.py.
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

struct CtldContact {
    std::uint32_t point = 0;      // CTPT point index
    std::uint32_t flags = 0;      // 0x100 must touch, 0x200 must not, 0x1000 down at the end, 0x10/0x20 derive at load
    std::int32_t event_list = -1; // fired on touch-down
    std::uint32_t raw_c = 0;
};

// One motion state; offsets are within the file record.
struct CtldState {
    std::uint32_t velocity_mask = 0;        // +0x00: 1/2/4 linear x/y/z, 0x10/0x20/0x40 angular x/y/z
    std::uint32_t flags = 0;                // +0x04: bit0 auto-selectable, 0x100000 no root motion, ...
    std::array<std::uint8_t, 4> lean{};     // +0x08: lean source per axis (3 used)
    std::array<float, 2> track_a{}, track_b{}; // +0x0c/+0x14: {from, to} frames (IAniMesh::SetAnimRange)
    float min_b_weight = 0;                 // +0x1c
    float duration_ms = 0;                  // +0x20: 0 = blended track distance / requested speed
    std::array<float, 3> lin_min{}, lin_max{}, ang_min{}, ang_max{}; // +0x24..+0x53
    float energy = 0;                       // +0x54
    std::array<float, 14> computed{};       // +0x58..+0x8f: root displacements, filled at load
    std::int32_t enter_event_list = -1;     // +0x90
    std::int32_t uses = -1;                 // +0x94: 0 never, -1 unlimited
    std::int32_t mode = -1;                 // +0x98: required mode, -1 any
    std::vector<CtldContact> contacts;
};

struct CtldDevice {
    std::uint32_t type = 0;
    std::int32_t part = 0;
    std::array<std::uint8_t, 0xb0> header{};
    std::vector<std::uint32_t> joints;
    std::string text; // the unit module the device stands for (cp1251 as stored)
};

struct CtldEvent {
    std::uint32_t flags = 0;
    std::uint32_t type = 0;
    std::array<std::uint8_t, 0x64> raw{};
    std::string lib, entry;
};

struct Ctld {
    std::array<float, 27> body{};
    std::vector<CtldState> states;
    std::vector<float> weights; // [to * states + from] (row = destination; motion-runtime.md §3)
    std::vector<std::array<std::uint8_t, 0x24>> joints;
    std::vector<CtldDevice> devices;
    std::array<std::int32_t, 21> situations{};
    std::vector<std::vector<CtldEvent>> event_lists;
};

Ctld decode_ctld(std::span<const std::uint8_t> data);

} // namespace parkan
