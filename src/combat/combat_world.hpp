// Weapons → bullets → damage → death for the live units and the buildings of a mission (a building is
// a combatant over a static LiveUnit, LiveUnit::load_building; notes/buildings.md §2): each unit's gun devices and
// damage parts, the bullets in flight, the damage queue and removal. Spec: notes/weapons-damage.md
// (§7 is the recipe this follows); the pieces are combat/gun.hpp and combat/damage.hpp.
//
// Port choices and approximations (inferred where marked):
// - hit test: the bullet's swept segment against the landscape (TerrainGround::trace), the static
//   scene's triangles (TraceMesh) and the posed triangles of every live unit and building (the face hit names the
//   object and mesh part, as Terrain's TraceRay does); dead units have left collision (the player's robot stays);
// - seekers (device 0x11): CSeekerDevice::Track + CBulletControl::Update's homing turn (fly()); the gun's
//   lock-on gate (param 9/10) is kept, so guided guns need a target (set_target);
// - rockets with their own throttle (bullet property 0x20) start at the gun's speed like the rest;
// - force shields (classes 9 + 0x15, combat/energy.hpp): the bullet tests the shield sphere — the mesh's
//   IVisual GetBoundingSphere(2) (IsShieldActive 1002c500 → AniMesh 10014580), taken as the sphere around
//   the static pose's part spheres (radius-weighted centre, AniMesh 10009510; inferred: that function is
//   read only in part) — as a point (bullet radius 0, inferred) before the face hit
//   (CBulletControl::OnCollision 1000d0c0). Only a sphere the bullet enters during the step counts: the
//   original takes the object hits from the collision manager's spheres (SweptSphereTOI, whose overlap
//   case would also report a bullet already inside, e.g. a bunker's shot inside the sphere of the plant
//   beside it; the collision sphere of a building is not traced: inferred). The hit effect follows the
//   unit's frame (its part: inferred root);
// - armour (type 0x1b) is the last one in the unit's scheme (CreateDevice overwrites ctl+0x5b4; inferred);
// - the impact effect is EXPL fx[0] (the hit face's surface type is not decoded);
// - the slow tick (SlowTickDamage, then the energy budget, combat/energy.hpp) runs every 250 ms with the
//   time elapsed since the last one (the original's LFSR jitter is not reproduced);
// - the devices are every part's CTLD devices with the scheme's INTO modules applied (module@N reloads
//   device N of its owner's CTLD, FUN_1002d890; a clip on a gun replaces its params and bullet);
// - mult (property 0xb4) is 1 and the scale is the TMA z scale (GetContactPoint scales per axis).
#pragma once

#include "combat/damage.hpp"
#include "combat/energy.hpp"
#include "combat/gun.hpp"
#include "combat/rooms.hpp"
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
    // Unit property 0x79 (GetPropertyImpl 1000dcc0 → 0x204 slot 0x2c, 1002c430): the six shield sides
    // 0..1 for the HUD (front, back, left, right, top, bottom); nullopt without a shield and deflector.
    std::optional<std::array<float, 6>> shield_charges() const { return energy.shield_charges(energy_host()); }
    // What the devices read from this unit (part ratios, the damage state, the clock; no engine load).
    EnergyHost energy_host() const;
    // Unit properties 0x50 / 0xa9 (CUnitControl GetProperty 1000e6c0 → 0x204 GetProperty(8 / 9), 1002b410): the
    // radar's (class 8) GetParam(3) (range) / GetParam(4) (the HUD sweep's); nullopt without a radar.
    std::optional<float> radar_param(std::size_t i) const;
    // A reload room's step on this unit (Room_RechargeLifeAndShield 10018100, Room_RechargeGuns 100181e0, and the
    // battery, MRoomManager_Takt 10018ac0): battery, HP, shield mean and every type-2 gun's energy ratio rise by
    // `a` (clamped to 1), its ammo by max(1, round(N·a)); `now_ms` stamps the healing.
    void recharge(float a, float now_ms);
    // The unit's detection signatures (unit properties 0x43 / 0x44 / 0x45, unit-stats.md §3):
    // ΣA · ctl+0x5dc · Π0xc00, ΣV · ctl+0x5e0 · Π0xd00, M · Π0xb00.
    struct Signature {
        float p43 = 0, p44 = 0, p45 = 0;
    };
    // The assembled design's mass, area and volume sums (economy::UnitStats), set by the app; nullopt = unknown
    // (the unit is then detected by range alone, as before the radar depended on energy).
    struct Mass {
        float mass = 0, area = 0, volume = 0;
    };
    std::optional<Mass> mass;
    std::optional<Signature> signature() const;
    // CRadarDevice_Detects (10024620) of this unit's radar (class 8) for a target at distance `d` with
    // signature `s`: on (dev+0x50 ≠ 0), not broken, d < R = GetParam(3), f = 1 − d²/R²,
    // GetParam(0x300)·p45·f ≥ 1 or GetParam(0x301)·p43·f ≥ 1 or GetParam(0x302)·p44·f ≥ 1.
    bool radar_detects(float d, const Signature& s) const;
    // Device property 0x300 of gun `g`'s device (GetDevicePropertyById 1002bb40 → CDevice slot 2): broken, its
    // part's HP ratio is 0.
    bool gun_broken(std::size_t g) const;
    // The state (dev+0x50) of the first device of spec type `type` (0x204 slot 0x24(0, type), then slot 0x14);
    // nullopt without one.
    std::optional<std::uint32_t> device_state(std::uint32_t type) const;
    // CControlBase part value 1 (1000dc40) of damage part `transform` (a device's dev+4): its HP ratio; 1 for a
    // node without an NDPR record (port) or out of range.
    float part_ratio(std::int32_t transform) const;

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
    // The shield sphere (IVisual GetBoundingSphere, space 2): world centre and radius.
    Vec3 shield_centre() const { return unit->frame.matrix.transform_point(shield_centre_); }
    float shield_radius() const { return shield_radius_; }
    // A building (Terrain CBuilding over a BTLU agent; notes/buildings.md): a static body, no motion.
    bool building() const { return unit && unit->is_static(); }

private:
    friend class CombatWorld;
    Vec3 bound_centre_{}; // in the unit frame
    float bound_radius_ = 1;
    Vec3 shield_centre_{}; // in the unit frame
    float shield_radius_ = 0;
    float next_slow_tick_ = 0;
    std::uint32_t shield_fx_ = 0;         // shield +0xd8: the next of its three hit effects (10025ca0)
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
    float hp = 0;             // ctl+0x590: its HP (NDPR part 0 × mult), spent punching through shields
    bool alive = true;
    bool fresh = true; // spawned this step: flies from the next one (AddObjectToGame)
};

// What happened during update(), for the app (sound, logs, AI notifications, mission runtime).
struct CombatEvent {
    enum class Type {
        Shot,    // unit fired gun `other` (Bullet spawned at pos)
        Hit,     // a bullet exploded at pos; unit = the unit hit (−1 terrain/static/air), other = shooter
        Damaged, // OnDamageMsg: unit was in a damage record of attacker `other` (or a bullet of `other`
                 // punched through its shield, amount 0); amount = HP lost
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
    // Effect: the world matrix to start it at, instead of bullet_matrix(pos, axis); with `attached` it
    // follows unit `unit` from there (CreateEffect attached to the object: the shield's hit effects).
    std::optional<Mat4> placement{};
    bool attached = false;
    // Effect: an instance key; starting one restarts the instance with the same key (the shield's ring of
    // three, 10025ca0). 0 = a new instance.
    std::uint32_t handle = 0;
    std::int32_t start_mode = -1; // Effect: StartEffect's time mode (−1: the template's own start)
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
    // `player`: the hero (guns selected through the keys). `building`: control class 3 (no
    // removal by Control, no debris; the hero is class 4 like every robot).
    std::size_t add_unit(LiveUnit& unit, std::uint32_t logic_id, std::uint32_t clan, bool player, bool building = false);
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
    // CTurretDevice state 0x400 ("AI point") on unit `u`'s turret toward `aim`: the yaw / pitch channels slew
    // (TurretRig::slew, `dt_s` s) toward the angles seen from the first gun's muzzle, and the gun channels
    // add kGunElevation · the ballistic elevation of the turret's first ballistic gun (AimGunChannel; one
    // offset for every gun channel: port); each turret gun's fire-enabled byte (dev+0x118) is whether its
    // shot can reach `aim`. Nothing without a turret.
    void ai_aim(std::size_t u, Vec3 aim, float dt_s);
    // The weapon's aim error for the fire control (device slot 0x28 stand-in): for a free-flying bullet on
    // a turret, the sight's error (its shots converge on the sight); for a ballistic one, the barrel against
    // the direction AimGunChannel commands (from the turret centre to `aim`, raised by kGunElevation · its
    // elevation); else aim_error.
    float ai_aim_error(std::size_t u, std::size_t g, Vec3 aim) const;
    // Gun `g`'s muzzle in the world (current pose), if it has one.
    std::optional<Vec3> muzzle(std::size_t unit, std::size_t gun) const;

    // AimGunChannel's ballistic solution (10028200), for a shot at `d` (target − muzzle) at speed `v` under
    // gravity `g` (the gun's +0x98 · IWorld gravity: 0 for a bullet without ballistics): the flight time
    // T² = 2(B ∓ √(B² − g²|d|²))/g², B = v² − g·d.z (the shorter when positive), the launch direction
    // (d.xy/T, (g·T²/2 + d.z)/T) and the angle between it and `d`. nullopt: out of reach (the gun's
    // fire-enabled byte dev+0x118 is cleared). g = 0 or v = 0: angle 0.
    static std::optional<float> ballistic_elevation(Vec3 d, float v, float g);
    static constexpr float kGravity = 10.f;     // IWorld::GetGravity default (control-physics.md)
    static constexpr float kGunElevation = 0.83f; // 1003cb38: the share of that angle AimGunChannel adds

    // MatrixFromDir (1002b0f0): y = dir, x = dir × up(0,0,1), at `pos`.
    static Mat4 bullet_matrix(Vec3 pos, Vec3 dir);
    // Reload rooms (combat/rooms.hpp, notes/energy.md §3): the hallway of building `unit`. Every kRoomPeriodMs the
    // living, standing (< the room's speed limit) units of the building's clan on a reload vertex are recharged.
    void set_rooms(std::size_t unit, std::vector<Room> rooms);
    // Statistics.
    std::size_t shots = 0, hits = 0;

private:
    class Host;
    const BulletProto* bullet(const ResName& name);
    void spawn(std::size_t unit, std::size_t gun, Vec3 pos, Vec3 dir);
    std::optional<Vec3> seek(const Bullet& b) const;
    void fly(Bullet& b, float dt_ms, float now);
    void explode(const Bullet& b, const Expl& e, Vec3 pos, std::int32_t target, std::int32_t part, std::int32_t side = -1);
    // GetShieldSide (1002c590) for a hit at world point `p`: the side (−1: no active shield) and, when its
    // strength is > 0, the hit effect (10025ca0).
    std::int32_t shield_side(std::size_t unit, Vec3 p);
    // AbsorbByShield (1000ff00) on a copy of `rec` for victim `v`: the shield takes min(strength, damage).
    void absorb_by_shield(std::size_t v, DamageRecord& rec);
    void process_records(float now);
    void apply_record(std::size_t victim, const DamageRecord& rec, float now);
    void drain_explosions(std::size_t unit);
    void note_death(std::size_t unit, bool was_killed, float now);
    void update_energy(Combatant& c, float dt_s, float now);
    void update_rooms(float now);
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
    struct RoomSet {
        std::size_t building = 0;
        std::vector<Room> rooms;
        std::optional<float> last_ms; // MB+0x400
    };
    std::vector<RoomSet> room_sets_;
    std::uint32_t next_bullet_ = 1;
};

} // namespace parkan::combat
