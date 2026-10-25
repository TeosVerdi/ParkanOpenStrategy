// Weapons → bullets → damage → death for the live units and the buildings of a mission (a building is
// a combatant over a static LiveUnit, LiveUnit::load_building; notes/buildings.md §2): each unit's gun devices and
// damage parts, the bullets in flight, the damage queue and removal. Spec: notes/weapons-damage.md
// (§7 is the recipe this follows); the pieces are combat/gun.hpp and combat/damage.hpp.
//
// Port choices and approximations (inferred where marked):
// - hit test: the bullet's swept segment against the landscape (TerrainGround::trace), the static
//   scene's triangles (TraceMesh) and the posed triangles of every live unit and building (the face hit names the
//   object and mesh part, as Terrain's TraceRay does); units that are dead have left collision;
// - seekers (device 0x11): CSeekerDevice::Track + CBulletControl::Update's homing turn (fly()); the gun's
//   lock-on gate (param 9/10) is kept, so guided guns need a target (set_target);
// - rockets with their own throttle (bullet property 0x20) start at the gun's speed like the rest;
// - shields (types 9/0x15) are absent; armour (type 0x1b) is the last one in the unit's scheme
//   (CreateDevice overwrites ctl+0x5b4; inferred);
// - the impact effect is EXPL fx[0] (the hit face's surface type is not decoded);
// - the slow tick (SlowTickDamage, then the energy budget, combat/energy.hpp) runs every 250 ms with the
//   time elapsed since the last one (the original's LFSR jitter is not reproduced);
// - the energy budget's devices are every part's CTLD devices with the scheme's INTO modules applied
//   (module@N replaces device N of its owner's CTLD, FUN_1002d890), except the guns, which keep their own
//   part's CTLD header (a clip module, e.g. i_c03_t under e_gun_tc_03, is not applied to the gun);
// - mult (property 0xb4) is 1 and the scale is the TMA z scale (GetContactPoint scales per axis).
#pragma once

#include "combat/damage.hpp"
#include "combat/energy.hpp"
#include "combat/gun.hpp"
#include "world/collision.hpp"
#include "world/ground.hpp"
#include "world/live_unit.hpp"

#include <map>
#include <memory>

namespace parkan::combat {

// Where a gun sits on its unit.
struct GunMount {
    std::size_t part = 0;                 // UnitModel part whose CTLD holds the device
    std::vector<ContactPoint> points;     // that part's CTPT
    // The turret sight the gun converges on (CGunDevice +0x114 / +0x110, pushed by the turret device):
    // CTPT points of the turret part `sight_part` (its own CTPT `sight_points`); -1 = the gun is not on a
    // turret (chassis guns).
    std::int32_t sight_origin = -1, sight_dir = -1;
    std::size_t sight_part = 0;
    std::vector<ContactPoint> sight_points;
    bool on_turret() const { return sight_origin >= 0 && sight_dir >= 0; }
};

// One live unit's combat state. The AI drives it through the public members below.
class Combatant {
public:
    LiveUnit* unit = nullptr;
    std::uint32_t logic_id = 0;
    DamageState damage;
    std::vector<GunDevice> guns;   // every weapon device of the unit's parts, in part then device order
    std::vector<GunMount> mounts;  // per gun
    std::vector<bool> selected;    // ManManager +0x5a per multigun component
    EnergyBudget energy;           // every device of the control, the guns by index (combat/energy.hpp)
    // Unit property 0x73 (GetPropertyImpl 1000e3d1 → 0x204 GetProperty(1)): the batteries' charge; 0 when the
    // unit has none (the HUD energy gauge).
    float battery_fraction() const { return energy.battery_fraction().value_or(0.f); }

    // Unit-level API (ManManager / CDeviceSet in the original):
    // MCMD_SELECT: index n ≥ 1 toggles the n-th gun (selecting resets it, deselecting ceases its fire);
    // −1 selects every gun.
    void select(std::int32_t index);
    // MCMD_STATE INDEX −1: SetState(state) on every selected gun (HERO.TBL: LMB 0x100, release 0).
    void set_state_selected(std::uint32_t state);
    void set_trigger(bool held) { set_state_selected(held ? kFireContinuous : kCeaseFire); }
    // SetState on one gun: kCeaseFire, kFireContinuous, kFireSingle, kGunReset.
    void set_gun_state(std::size_t gun, std::uint32_t state);
    // The AI's FireRequest::trigger: bit i → gun i fires one shot (state 0x200, its reload still applies).
    void fire_mask(std::uint32_t mask);
    // CDeviceSet::SetTarget: the unit index the guns aim at (guided guns lock on it; −1 none).
    std::int32_t target = -1;
    // Port aid: when set, every bullet leaves the muzzle towards this world point (instead of the barrel
    // direction / sight convergence). The turret itself is aimed through unit->aim_x / aim_y.
    std::optional<Vec3> aim_point;

    bool alive() const { return !damage.killed; }
    bool removed = false;
    Vec3 centre() const;      // world bounding-sphere centre
    Vec3 aim_centre() const;  // the root part's mesh sphere centre (the point to aim at), else centre()
    float radius() const { return bound_radius_; }
    // A building (Terrain CBuilding over a BTLU agent; notes/buildings.md): a static body, no motion.
    bool building() const { return unit && unit->is_static(); }

private:
    friend class CombatWorld;
    Vec3 bound_centre_{}; // in the unit frame
    float bound_radius_ = 1;
    float next_slow_tick_ = 0;
    std::optional<float> last_slow_tick_; // ctl+0xec
    // Buildings: the world-space triangles of each draw, rebuilt when its node matrix changes (doors),
    // with a bounding sphere per draw for the segment test (port: the meshes are large and static).
    struct DrawCache {
        Mat4 matrix{};
        std::vector<Vec3> verts; // 3 per triangle
        Vec3 centre{};
        float radius = -1;
    };
    mutable std::vector<DrawCache> draw_cache_;
};

struct Bullet {
    std::uint32_t id = 0; // unique, from 1 (effects follow it)
    const BulletProto* proto = nullptr;
    std::int32_t shooter = -1, gun = -1;
    Vec3 pos{}, vel{}, dir{0, 1, 0};
    float range_left = 0;
    float mult = 1;
    std::int32_t target = -1; // the seeker's target (the gun's at the shot; CSeekerDevice +0x98), −1 none
    bool alive = true;
    bool fresh = true; // spawned this step: flies from the next one (AddObjectToGame)
};

// What happened during update(), for the app (sound, logs, AI notifications, mission runtime).
struct CombatEvent {
    enum class Type {
        Shot,    // unit fired gun `other` (Bullet spawned at pos)
        Hit,     // a bullet exploded at pos; unit = the unit hit (−1 terrain/static/air), other = shooter
        Damaged, // OnDamageMsg: unit was in a damage record of attacker `other`; amount = HP lost
        Killed,  // unit's clan became 0xfffe (amount = time)
        Removed, // unit's removal time passed: MissionRuntime::object_killed, hide it
        Effect,  // start effect `effect` at pos (bullet tracer/muzzle, impact, part explosion)
        BulletEnded, // bullet `bullet` died in situation `amount` (2 hit, 3 left the world, 4 range exhausted)
    };
    Type type;
    std::int32_t unit = -1, other = -1;
    float amount = 0;
    Vec3 pos{};
    ResName effect;
    std::uint32_t bullet = 0; // Effect: the bullet it rides on (situation 0 lists), 0 = placed once
    Vec3 axis{0, 1, 0};       // Effect: the frame's forward axis (bullet direction)
    float scale = 1;          // Effect: SetEffectScale (part explosions: the part's radius; inferred)
    Mat4 frame = Mat4::identity(); // Effect: local spawn frame (fx::contact_frame of the event's CTPT points)
    // Effect on a bullet: BulletProto::end_actions for situations 2/3/4, and the bullet's removal delay.
    std::array<std::int32_t, 3> end_actions{BulletProto::kEffectKeep, BulletProto::kEffectKeep, BulletProto::kEffectKeep};
    float removal_ms = 0;
};

// A segment hit in the world.
struct WorldHit {
    enum class Kind { Terrain, Static, Unit } kind = Kind::Terrain;
    float t = 1;
    Vec3 point{}, normal{0, 0, 1};
    std::int32_t unit = -1, part = -1; // unit index and resolved damage part
    Vec3 face_normal{0, 0, 1};         // the face's own normal (winding order), not flipped toward a
};

class CombatWorld {
public:
    CombatWorld(AssetLibrary& lib, const TerrainGround& ground, const TraceMesh* statics);

    // Adds a live unit: its gun devices (with their bullets), damage parts, armour and removal delay.
    // `player`: the hero (class 3: guns selected through the keys, never removed).
    std::size_t add_unit(LiveUnit& unit, std::uint32_t logic_id, std::uint32_t clan, bool player);
    std::size_t size() const { return units_.size(); }
    Combatant& unit(std::size_t i) { return *units_[i]; }
    const Combatant& unit(std::size_t i) const { return *units_[i]; }

    // One simulation step to `now_ms`. Call after the units moved and were posed (pose_parts) for this
    // step. Order: guns, bullets, the damage queue, the slow damage tick, removals.
    void update(float now_ms);
    std::vector<CombatEvent> take_events() { return std::move(events_); }

    const std::vector<Bullet>& bullets() const { return bullets_; }
    // Every bullet prototype any gun fires (for the renderer's pools), by name.
    const std::map<std::string, std::unique_ptr<BulletProto>>& bullet_protos() const { return protos_; }

    // The unit under unit `u`'s turret sight (the first gun mount with one), within `range` m; −1 none.
    // Port stand-in for the target CTurretDevice pushes to its guns in manual mode (PushTargetToGuns).
    std::int32_t sight_target(std::size_t u, float range = 1000.f) const;

    // The nearest hit along a → b against the landscape, statics and live units except `skip`.
    std::optional<WorldHit> trace(Vec3 a, Vec3 b, std::int32_t skip) const;
    // The first static or building surface along a → b (no landscape, no mobile units): the floors
    // units stand on (LiveUnit::FloorProbe).
    std::optional<WorldHit> trace_floor(Vec3 a, Vec3 b) const;

    // The angle (rad) between gun `g`'s current barrel and the line from its muzzle to `aim`; π when the
    // muzzle is unknown. The AI's weapon / turret aim error (device slot 0x28) stand-in.
    float aim_error(std::size_t unit, std::size_t gun, Vec3 aim) const;
    // Gun `g`'s muzzle in the world (current pose), if it has one.
    std::optional<Vec3> muzzle(std::size_t unit, std::size_t gun) const;

    // MatrixFromDir (1002b0f0): y = dir, x = dir × up(0,0,1), at `pos`.
    static Mat4 bullet_matrix(Vec3 pos, Vec3 dir);
    // Statistics.
    std::size_t shots = 0, hits = 0;

private:
    class Host;
    const BulletProto* bullet(const ResName& name);
    void spawn(std::size_t unit, std::size_t gun, Vec3 pos, Vec3 dir);
    std::optional<Vec3> seek(const Bullet& b) const;
    void fly(Bullet& b, float dt_ms, float now);
    void explode(const Bullet& b, const Expl& e, Vec3 pos, std::int32_t target, std::int32_t part);
    void process_records(float now);
    void apply_record(std::size_t victim, const DamageRecord& rec, float now);
    void drain_explosions(std::size_t unit);
    void note_death(std::size_t unit, bool was_killed, float now);
    void update_energy(Combatant& c, float dt_s, float now);
    Vec3 part_centre(const Combatant& c, std::size_t part, float& radius) const;

    AssetLibrary& lib_;
    const TerrainGround& ground_;
    // everything = false: statics and buildings only.
    std::optional<WorldHit> trace(Vec3 a, Vec3 b, std::int32_t skip, bool everything) const;
    const TraceMesh* statics_;
    std::vector<std::unique_ptr<Combatant>> units_;
    std::vector<Bullet> bullets_;
    std::vector<DamageRecord> records_;
    std::vector<CombatEvent> events_;
    std::map<std::string, std::unique_ptr<BulletProto>> protos_;
    std::map<std::string, std::optional<Expl>> expl_cache_;
    float last_ms_ = 0;
    std::uint32_t next_bullet_ = 1;
};

} // namespace parkan::combat
