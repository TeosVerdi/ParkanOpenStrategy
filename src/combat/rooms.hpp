// Reload rooms: the hallway vertices of a building that refill the units standing on them. Behavior.dll
// MRoomManager_Init (100186b0) builds a 0x3c-byte room per hallway vertex of the building's IHallWay; the
// recharge step is MRoomManager_Takt (10018ac0). Spec: notes/energy.md §3.1–§3.3.
//
// Port choices (inferred where marked):
// - the hallway is the type-0x11 entry of the building's model .msh (nested NRes; found by a data scan,
//   CreateHallWay's arguments were not traced); its vertices are in the model frame, placed by the
//   building's pose like the mesh;
// - the step period is a fixed 100 ms (the manager timer is built with (0.1, 0.1): the exact base/jitter
//   scaling is not decoded), with dt = the time since the last step;
// - an occupant is a unit (not a building) of the building's own clan: the allied test (MBehaviour slot
//   0xd4) and the occupant's behaviour property 0x208 are taken as "same clan" and "0";
// - the in-cylinder test (FUN_10022c80) is the horizontal distance ≤ radius;
// - vertex flag 0x10000000 only widens the room (radius 10, height 12); its other meaning is open.
#pragma once

#include "core/math.hpp"
#include "res/library.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace parkan::combat {

// One hallway vertex (GetVertexInfo 1000a6c0): 20 bytes {f32 x, y, z; u32 flags; i32 value}.
struct HallwayVertex {
    Vec3 pos{};
    std::uint32_t flags = 0;
    std::int32_t value = 0;
};

// FUN_1000a400: the type-0x11 sub-entry of the model's nested NRes (attr1 = vertex count); empty when the
// mesh has none.
std::vector<HallwayVertex> decode_hallway(std::span<const std::uint8_t> msh);
// The same for `entry` of `archive` in the library (empty when missing).
std::vector<HallwayVertex> load_hallway(AssetLibrary& lib, std::string_view archive, std::string_view entry);

// A room record (MRoomManager_Init): kept for a vertex with flags & 0xf8 or flags & 0xc600.
struct Room {
    std::uint32_t vertex = 0; // +0
    Vec3 pos{};               // model frame
    std::uint32_t flags = 0;  // +8
    std::int32_t value = 0;   // +0xc
    float speed_limit = 2;    // +0x2c: 1000 if flags & 0x7c000, else 2.0
    float radius = 5;         // +0x30: 10 if flags & 0x10000000, else 5
    float height = 3;         // +0x34: 12 if flags & 0x10000000, else 3
    // The recharge step runs on flags & 0x20 or flags & 0x600 (MRoomManager_Takt).
    bool reloads() const { return (flags & 0x20) != 0 || (flags & 0x600) != 0; }
};
std::vector<Room> build_rooms(const std::vector<HallwayVertex>& vertices);

// Room_UpdateOccupants' box test (10018310) for a unit standing at `unit` and a room at the world point
// `room`: z from room.z − 0.7·h (room.z − h when flags & 0x4040) to room.z + h, horizontally within radius.
bool room_contains(const Room& r, Vec3 room_world, Vec3 unit);

// Room_RechargeGuns' ammo step (100181e0): ammo += max(1, round(N·a)), at most N (N = round(GetParam(0)) > 0).
std::int32_t recharged_ammo(std::int32_t ammo, std::int32_t max_ammo, float a);

constexpr float kRoomPeriodMs = 100; // [inferred] the manager timer's period
constexpr float kRoomRate = 0.1f;    // 10059604: a = 0.1 · dt

} // namespace parkan::combat
