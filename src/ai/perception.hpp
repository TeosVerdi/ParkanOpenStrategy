// The unit brain's view of the other objects: what Behavior.dll asks the system areal map (IArealMap
// slot 0x54 GetObjectInterface(LogicID) → IBehaviour / IGameObject), the clan object map (MB+0x54,
// slot 9: the 0x28-byte record of a known unit), the unit's own clan relations (MBehaviour slots
// 0xd0 / 0xd4) and the world ray the fire control casts (MB+0x3c slot 12). The app implements it over
// its live objects.
// Spec: notes/unit-ai-runtime.md §4.
//
// Deliberately reduced (see §4.9): the clan object map's knowledge gating (an object is known only while
// an areal it stands in was observed by a unit of the clan, ArealMap 10001dd0 / 10006e40) is left to the
// implementation: whatever `object()` returns counts as known.
#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace parkan::ai {

inline constexpr std::uint32_t kNoObject = 0xFFFFFFFF; // LogicID −1
inline constexpr std::uint32_t kNoClan = 0xFFFE;       // IBehaviour slot 0x44 "no clan" (counts as dead)
inline constexpr std::uint32_t kBuildingBit = 0x80000000; // LogicID / type bit 31
inline constexpr std::uint32_t kTypeAnimal = 0x20000000;

struct SeenObject {
    std::uint32_t logic_id = kNoObject;
    std::uint32_t type = 0;       // ObjectTypeCode (IBehaviour GetCreationParam, slot 0x38)
    std::uint32_t clan = kNoClan; // IBehaviour slot 0x44
    Vec3 pos{};                   // IGameObject GetPlacement(2) translation
    // Bounding radius: IGameObject slot 0x24(2) sphere radius (building attack ring, MakeGoCommand) and
    // the target-size property 0x206 (fire control hit tolerance). 0 = unknown (treated as 1 m there).
    float radius = 0;
    // The clan object map record (+0x18, +0x1c, +0x20 armour, +0x24 weapon; ArealMap 10006e40 fills them
    // from control properties 0x36 / 0x26 and behaviour property 0x204). Only the target scores read
    // them (10001010, 1002d390, 100255a0, 100256e0); zeros give the plain distance scores.
    float rec18 = 0, rec1c = 0, armour = 0, weapon = 0;

    bool building() const { return (logic_id & kBuildingBit) != 0; }
};

class Perception {
public:
    virtual ~Perception() = default;
    // GetObjectInterface(lid) + its IBehaviour: nullopt when the object does not exist (or has no
    // clan: FUN_10014b70 treats clan 0xfffe as gone).
    virtual std::optional<SeenObject> object(std::uint32_t logic_id) const = 0;
    // LogicIDs of the objects (units and buildings) within `radius` metres (2-D) of `centre`. Used for
    // the unit's sensor (observer MB+0x4a8, radius = AiUnitConfig::sensor_radius), the nearest-enemy
    // search of an Attack without a target (10001120, 3000 m) and the friendly-in-line check.
    virtual void objects_near(Vec3 centre, float radius, std::vector<std::uint32_t>& out) const = 0;
    // MBehaviour slot 0xd0 IsEnemy(clan): the own clan's relation to `other` is hostile (0).
    virtual bool hostile(std::uint32_t own_clan, std::uint32_t other_clan) const = 0;
    // The world ray of the fire control (MB+0x3c slot 12, flags 0x40a) from the muzzle `from` to the aim
    // point `to`, ignoring `self` and `target`: true when no static object or landscape blocks it, or
    // the first hit is within 20 m of `to` (10024788..100247eb).
    virtual bool line_clear(Vec3 from, Vec3 to, std::uint32_t self, std::uint32_t target) const = 0;
};

} // namespace parkan::ai
