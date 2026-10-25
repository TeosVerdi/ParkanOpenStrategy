// The world model the clan strategists (CSuperAI, src/game/super_ai.hpp) query: every placed object
// with its LogicID, clan, ObjectTypeCode and position, the clans with their relations and bases, and
// the tactical areals. It stands in for what ai.dll reads through the clan object map
// (CreateArealMap, CSuperAI+0x3d8), the system areal map (+0x3dc) and IBehaviour. Spec:
// notes/script-handlers-a.md / -b.md ("shared state"), notes/behaviour-data.md (type codes, orders).
//
// Units the app moves are attached through UnitControl, so the world is testable without the app.
//
// Derivations that are the port's (the original's code path is not traced):
// - ObjectTypeCode. The original passes it as World3D initData[1] (IBehaviour::GetCreationParam);
//   Behavior.dll picks the profession sheet from it (10008a80), not the other way round. Here it comes
//   from the design's folder (UNITS\UNITS\BATTLE = warbot, ...\BUILDS\PLANT = plant, the BuildDat.lst
//   layout); UNITS\UNITS\AI, PREBLD, AutoDEMO and UNITS\AUTO designs count as warbots.
// - Size class (property 0x201): from the chassis prototype's size letter (R_T_ 1, R_L_ 2, R_M_ 3,
//   R_B_ 4) when the scheme is readable; 0 otherwise. Buildings: 0 (their level is not modelled).
// - Tactical areals (system map slot 33, h32): the data.tma paths, path id = areal number, polygon in
//   xy; an object is "in" the areal when its position is inside the polygon (inferred from Tut_2,
//   whose paths 0..4 ring the places its script reports on).
// - Hit points: no damage model yet. Armour/weapon are placeholders (kPlaceholder*) so the scripts'
//   force arithmetic (hits = (weapon + 0.8) · armour · 1e-5, 1000fc70) has non-zero values.
#pragma once

#include "core/math.hpp"
#include "mission/tma.hpp"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace parkan::game {

inline constexpr std::uint32_t kNoObject = 0xFFFFFFFF; // LogicID -1 / script ERROR
inline constexpr std::uint32_t kNoClan = 0xFFFE;       // IBehaviour +0x44 "no clan"
inline constexpr std::uint32_t kBuildingBit = 0x80000000; // LogicID and type bit 31

// ObjectTypeCode (behaviour-data.md; varset.var ROBOT_* / BUILDING_* / CLASS_*).
enum TypeCode : std::uint32_t {
    kTypeExplorer = 0x01001000,
    kTypeTransport = 0x01002000,
    kTypeBuilder = 0x01004000,
    kTypeWarbot = 0x01008000,
    kTypeHq = 0x01010000,
    kTypeHero = 0x01020000,
    kTypeAnimal = 0x20000000,
    kTypeGenerator = 0x80000002,
    kTypeMine = 0x80000004,
    kTypeStorage = 0x80000008,
    kTypePlant = 0x80000010,
    kTypeHangar = 0x80000040,
    kTypeTeleport = 0x80000200,
    kTypeInstitute = 0x80000400,
    kTypeBridge = 0x80001000,
    kTypeRuins = 0x80002000,
    kTypeBunkerSmall = 0x80010000,
    kTypeBunkerMedium = 0x80020000,
    kTypeBunkerLarge = 0x80040000,
    kTypeTowerMedium = 0x80100000,
    kTypeTowerLarge = 0x80200000,
    kClassRobot = 0x01000000,
};

// Placeholders until damage exists: one robot ≈ 10 "hits", the scale of the scripts' thresholds
// (dBuildingProtectHits is set to 20 + 55·fDifficulty, 25, 150 or 250 by the shipped Init events).
inline constexpr float kPlaceholderArmour = 1000.f, kPlaceholderWeapon = 1000.f;

// The script-side Order (0x13c bytes; behaviour-data.md "Orders and tasks", script-handlers-a.md h15).
struct UnitOrder {
    std::uint32_t type = 0;
    std::uint32_t param = 0;       // +4 (patrol radius, robot type, ...), the script's raw DWORD
    std::uint32_t flags = 0;       // +8
    std::uint32_t target_kind = 0x204;
    std::uint32_t target_id = kNoObject; // 0x201 LogicID
    Vec3 place{};                        // 0x202
    std::uint32_t mask = 0;              // 0x203
    std::string design;                  // 0x205: a scheme file name
    float angle = 0;                     // 0x206 (a 4×4 placement, M_Task_Build T+0xe0): rotation about z; position = place
    std::array<float, 11> limits{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f}; // +0x110; script writes [7..10]
};

// What the app implements for a unit it simulates (IBehaviour GiveOrder / GetCurrentOrderInfo and
// the placement read through QI(6)->GetPlacement).
class UnitControl {
public:
    virtual ~UnitControl() = default;
    // GiveOrder(order, insert mode 1 end / 2 start / 3 replace); false = refused.
    virtual bool give_order(const UnitOrder& order, int mode) = 0;
    virtual std::uint32_t current_order() const = 0; // order type, 0 = idle
    virtual Vec3 position() const = 0;
    // Control property 0x91 (speed); h14's nearest-capturer search ignores units at 1.0 or below.
    virtual float speed() const { return 0; }
};

struct WorldObject {
    std::uint32_t logic_id = kNoObject;
    std::uint32_t clan = kNoClan;
    std::uint32_t type = 0;     // ObjectTypeCode
    std::uint32_t size_class = 0; // property 0x201
    std::string resource;       // data.tma resource (design path or prototype key)
    std::int32_t tma_index = -1;
    Vec3 placed{};              // position when no control is attached
    float armour = 0, weapon = 0; // placeholders (see the file comment)
    bool alive = true;
    UnitControl* control = nullptr;

    bool building() const { return (logic_id & kBuildingBit) != 0; }
    Vec3 position() const { return control ? control->position() : placed; }
    std::uint32_t current_order() const { return control ? control->current_order() : 0; }
};

struct WorldClan {
    std::string name;
    std::uint32_t mode = 0;      // data.tma clan mode (0 = nature: spatial areals)
    std::string script, research; // bundle name (no folder), .trf path
    float base_x = 0, base_y = 0;
    std::vector<std::uint32_t> relations; // per clan index: 0 hostile, 1, 2 (data.tma relations)
    std::vector<std::array<float, 5>> migration; // mode-0 spatial records
    // SuperAI brains (the robot limit): iron3d 100392e0 calls AddBrain player+0x20 times; player+0x20 is the
    // MisLoad clan record's dword [5] (10039010). [inferred] That dword is the data.tma i32 after the .trf
    // path (TmaClan::research_tag: 2..17 in the shipped missions, enemies more than the player).
    std::uint32_t brains = 0;
};

struct TacticalAreal {
    std::int32_t id = 0;
    std::vector<std::array<float, 2>> poly;
    bool contains(float x, float y) const;
};

// 1000fc70: the scripts' "hits" unit.
inline float combat_hits(float armour, float weapon) { return (weapon + 0.8f) * armour * 1e-5f; }

// The type code for a placed object (see the file comment); 0 when the resource says nothing.
std::uint32_t type_code_for_resource(std::string_view resource);

// A unit's size class, property 0x201 (MB+0x960, unit+0x30): MBehaviour slot 0xd8 (Behavior 1000cee0), for a
// unit (control class 4) the 3rd letter of its root part's name, case-insensitive: t 1, h/l 2, m 3, b 4, else 0
// (the hero's R_H_02 is 2). Not M_Task_Construct's table (economy::robot_size, where e is 4 as well).
std::uint32_t unit_size_class(std::string_view root_entry);

class World {
public:
    // Every non-static data.tma object becomes a world object (LogicID = identity, clan = ai_index).
    // With `game_dir`, unit schemes are read for their size class.
    static World from_tma(const mission::Tma& tma, const std::filesystem::path* game_dir = nullptr);

    WorldObject* find(std::uint32_t logic_id);
    const WorldObject* find(std::uint32_t logic_id) const;
    // An object that exists (IArealMap GetBehaviour != NULL).
    const WorldObject* live(std::uint32_t logic_id) const;
    WorldObject& add(WorldObject o); // scripted spawns; replaces a dead object with the same LogicID
    std::deque<WorldObject>& objects() { return objects_; }
    const std::deque<WorldObject>& objects() const { return objects_; }

    void attach(std::uint32_t logic_id, UnitControl* control);
    void kill(std::uint32_t logic_id);
    void set_clan(std::uint32_t logic_id, std::uint32_t clan);

    std::vector<WorldClan> clans;
    std::vector<TacticalAreal> tactical;
    // Map extents for h55 (DAT_1005537c / DAT_10055544): the landscape size, set by the app.
    float map_max_x = 0, map_max_y = 0;

    bool in_tactical_areal(std::uint32_t areal, std::uint32_t logic_id) const;

    // h18 FindBuildPlace's terrain: the ground height at (x, y), nullopt where no valid areal is (system
    // map slots 7/6 + areal flag +0x20). Set by the app; unset = flat and valid everywhere.
    std::function<std::optional<float>(float x, float y)> terrain_height;
    // The global "recently assigned spot" table of ai.dll (1000fb60: 20 slots, 60 s), shared by every clan.
    struct ReservedSpot {
        float x = 0, y = 0;
        std::uint32_t time = 0;
    };
    std::vector<ReservedSpot> reserved_spots;

private:
    std::deque<WorldObject> objects_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
};

} // namespace parkan::game
