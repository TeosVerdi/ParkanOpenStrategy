#include "app/play.hpp"

#include "app/scene_viewer.hpp"
#include "audio/audio_engine.hpp"
#include "combat/combat_world.hpp"
#include "res/fx.hpp"
#include "control/turret.hpp"
#include "control/unit_motion.hpp"
#include "core/paths.hpp"
#include "input/man_manager.hpp"
#include "input/scancodes.hpp"
#include "mission/ske.hpp"
#include "mission/tma.hpp"
#include "res/ctld.hpp"
#include "res/terrain.hpp"
#include "scene/mission_scene.hpp"
#include "scene/sky.hpp"
#include "world/collision.hpp"
#include "world/building.hpp"
#include "world/live_unit.hpp"
#include "ai/ai_unit.hpp"
#include "res/landmap.hpp"
#include "world/ground.hpp"
#include "game/mission_runtime.hpp"
#include "res/textres.hpp"
#include "res/tft.hpp"
#include "ui/hud.hpp"
#include "ui/hud_resources.hpp"
#include "ui/hud_text.hpp"
#include "ui/mission_end.hpp"
#include "ui/campaign.hpp"
#include "ui/hq.hpp"
#include "shell/briefing.hpp"

#include <fstream>
#include <set>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <optional>

namespace parkan {

namespace {

bool contains_ci(std::string_view s, std::string_view part) {
    return std::search(s.begin(), s.end(), part.begin(), part.end(), [](char a, char b) {
               return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
           }) != s.end();
}

// The hero: the player's unit whose design lives under UNITS\UNITS\HERO (iron3d finds it by type
// 0x1020000 in the unit registry; the TMA has no such flag).
std::optional<std::size_t> find_hero(const mission::Tma& t) {
    for (std::size_t i = 0; i < t.objects.size(); ++i)
        if (contains_ci(t.objects[i].resource, "\\HERO\\") && contains_ci(t.objects[i].resource, ".dat")) return i;
    return std::nullopt;
}

// The script world's handle on a live unit: orders go to its brain; the player's hero takes none.
class UnitAdapter final : public game::UnitControl {
public:
    UnitAdapter(const LiveUnit& unit, ai::AiUnit* brain) : unit_(unit), brain_(brain) {}
    bool give_order(const game::UnitOrder& o, int mode) override {
        if (!brain_) return false;
        // The script's limits (UnitOrder +0x110, [7..10] only) stay at the task defaults.
        ai::Order order;
        order.type = o.type, order.param = float(o.param), order.target_kind = o.target_kind, order.place = o.place;
        order.target_id = o.target_id, order.mask = o.mask;
        return brain_->give_order(order, mode);
    }
    std::uint32_t current_order() const override { return brain_ ? brain_->current_order() : 0; }
    void stop() { brain_ = nullptr; } // the unit died: no more orders
    Vec3 position() const override { return unit_.position(); }
    float speed() const override { return brain_ ? brain_->walker().max_speed : 0; }

private:
    const LiveUnit& unit_;
    ai::AiUnit* brain_;
};

// The brains' view of the world (ai::Perception): live units through the combat world (dead or removed
// units are gone), the other objects (buildings) through the mission's world model.
class WorldPerception final : public ai::Perception {
public:
    WorldPerception(const combat::CombatWorld& combat, const game::World* world, const TerrainGround& ground, const TraceMesh& statics,
                    std::map<std::uint32_t, std::size_t> units, std::map<std::uint32_t, std::uint32_t> clans,
                    std::map<std::uint32_t, std::uint32_t> types)
        : combat_(combat), world_(world), ground_(ground), statics_(statics), units_(std::move(units)), clans_(std::move(clans)),
          types_(std::move(types)) {}

    std::optional<ai::SeenObject> object(std::uint32_t lid) const override {
        if (auto it = units_.find(lid); it != units_.end()) {
            const combat::Combatant& c = combat_.unit(it->second);
            if (c.removed || !c.alive()) return std::nullopt;
            ai::SeenObject o;
            o.logic_id = lid, o.clan = clan(lid), o.type = type(lid), o.pos = c.unit->position();
            // Buildings: their bounding sphere (IGameObject slot 0x24(2)), centred on the body.
            o.radius = c.building() ? c.unit->sphere_radius : c.radius();
            if (auto e = entries_.find(lid); e != entries_.end()) o.entry = e->second;
            return o;
        }
        if (!world_) return std::nullopt;
        const game::WorldObject* w = world_->live(lid);
        if (!w || w->clan == ai::kNoClan) return std::nullopt;
        ai::SeenObject o;
        o.logic_id = lid, o.clan = w->clan, o.type = w->type, o.pos = w->position();
        o.radius = w->building() ? kBuildingRadius : 1.f;
        o.armour = w->armour, o.weapon = w->weapon;
        return o;
    }
    void objects_near(Vec3 centre, float radius, std::vector<std::uint32_t>& out) const override {
        const float r2 = radius * radius;
        auto near2d = [&](Vec3 p) { return (p.x - centre.x) * (p.x - centre.x) + (p.y - centre.y) * (p.y - centre.y) <= r2; };
        for (const auto& [lid, i] : units_) {
            const combat::Combatant& c = combat_.unit(i);
            if (!c.removed && c.alive() && near2d(c.unit->position())) out.push_back(lid);
        }
        if (world_)
            for (const auto& w : world_->objects())
                if (w.alive && w.building() && !units_.count(w.logic_id) && near2d(w.position())) out.push_back(w.logic_id);
    }
    bool hostile(std::uint32_t own, std::uint32_t other) const override {
        if (!world_ || own >= world_->clans.size() || other >= world_->clans[own].relations.size()) return own != other;
        return world_->clans[own].relations[other] == 0;
    }
    bool line_clear(Vec3 from, Vec3 to, std::uint32_t, std::uint32_t) const override {
        // The world ray (flags 0x40a): landscape and static objects; a hit within 20 m of the aim counts as clear.
        std::optional<TraceHit> h = ground_.trace(from, to);
        if (auto s = statics_.trace(from, to); s && (!h || s->t < h->t)) h = s;
        if (!h) return true;
        const Vec3 d = h->point - to;
        return dot(d, d) <= 20.f * 20.f;
    }

    // Port: where a capturer walks to in a building (the capture volume's centre; see ai::SeenObject::entry).
    void set_entry(std::uint32_t lid, Vec3 p) { entries_[lid] = p; }
    // An object created while the mission runs (a produced robot, a new or upgraded building's body).
    void add_unit(std::uint32_t lid, std::size_t combatant, std::uint32_t clan, std::uint32_t type) {
        units_[lid] = combatant, clans_[lid] = clan, types_[lid] = type;
    }

private:
    static constexpr float kBuildingRadius = 15.f; // port: a building that failed to load (not a combatant)
    std::map<std::uint32_t, Vec3> entries_;
    std::uint32_t clan(std::uint32_t lid) const {
        if (world_)
            if (const game::WorldObject* w = world_->find(lid)) return w->clan;
        auto it = clans_.find(lid);
        return it != clans_.end() ? it->second : ai::kNoClan;
    }
    std::uint32_t type(std::uint32_t lid) const {
        auto it = types_.find(lid);
        return it != types_.end() ? it->second : 0;
    }
    const combat::CombatWorld& combat_;
    const game::World* world_;
    const TerrainGround& ground_;
    const TraceMesh& statics_;
    std::map<std::uint32_t, std::size_t> units_;
    std::map<std::uint32_t, std::uint32_t> clans_, types_;
};

// The legs of a builder or transport for the economy's walking tasks (economy::Mover): an ORDER_GO to the
// brain (insert mode 3). Idle once the GO task has ended and the walker stopped.
class BrainMover final : public economy::Mover {
public:
    explicit BrainMover(ai::AiUnit& brain) : brain_(brain) {}
    bool go_to(Vec3 target) override {
        ai::Order o;
        o.type = ai::kOrderGo, o.target_kind = ai::kTargetPlace, o.place = target;
        return brain_.give_order(o, 3);
    }
    bool idle() const override { return brain_.current_order() != ai::kOrderGo && !brain_.moving(); }

private:
    ai::AiUnit& brain_;
};

// The key bound to a UI command, as the in-game text names it (the SCAN_ name without its prefix).
std::string key_label(std::string_view command, input::KeyLayout layout) {
    const auto k = input::ui_key(command, layout);
    if (!k) return "?";
    std::string_view n = input::scan_name(k->key);
    if (n.starts_with("SCAN_")) n.remove_prefix(5);
    return std::string(n);
}

std::string with_key(std::string text, const std::string& key) {
    if (auto p = text.find("%s"); p != std::string::npos) text.replace(p, 2, key);
    return text;
}

// The shell's bookkeeping after a won mission (notes/mission-end.md §5): [COMPLETE] <key>=1 in
// missions/dispatcher.ini. Port: the game folder stays read-only, so the edited copy lives in the
// user's preferences folder, seeded from the shipped file.
void record_mission_complete(AssetLibrary& lib, std::string_view mission) {
    std::string path(mission);
    for (char& c : path) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
    if (path.empty() || path.back() != '/') path += '/';
    char* pref = SDL_GetPrefPath("", "Parkan");
    if (!pref) return;
    const std::filesystem::path ini_path = std::filesystem::path(pref) / "dispatcher.ini";
    SDL_free(pref);
    std::string ini;
    std::error_code ec;
    if (std::filesystem::exists(ini_path, ec)) {
        const Bytes b = read_file(ini_path);
        ini.assign(b.begin(), b.end());
    } else if (auto shipped = resolve_path(lib.game_dir(), "MISSIONS/dispatcher.ini")) {
        const Bytes b = read_file(*shipped);
        ini.assign(b.begin(), b.end());
    }
    ini = ui::set_mission_complete(ini, path);
    std::ofstream(ini_path, std::ios::binary) << ini;
    auto campaigns = ui::scan_campaigns(lib.game_dir());
    ui::apply_progress(campaigns, ui::parse_dispatcher_ini(ini));
    const auto next = ui::next_mission(campaigns, path);
    SDL_Log("play: %s complete (%s); next: %s", path.c_str(), ini_path.c_str(), next ? next->c_str() : "-");
}

} // namespace

int run_play(SDL_Window* window, AssetLibrary& lib, const PlayOptions& opt) {
    auto dir = resolve_path(lib.game_dir(), opt.mission);
    if (!dir) throw std::runtime_error("mission folder not found: " + opt.mission);
    auto tma_path = resolve_path(*dir, "data.tma");
    if (!tma_path) throw std::runtime_error("no data.tma in " + dir->string());
    const mission::Tma tma = mission::decode_tma(read_file(*tma_path));
    const auto hero_index = find_hero(tma);
    if (!hero_index) throw std::runtime_error("the mission has no hero unit");

    std::string map_dir = tma.land_path;
    if (auto cut = map_dir.find_last_of("\\/"); cut != std::string::npos) map_dir.resize(cut);
    auto land = resolve_path(lib.game_dir(), map_dir + "/Land.msh");
    if (!land) throw std::runtime_error("no Land.msh for " + map_dir);
    const Terrain terrain = decode_terrain(read_file(*land));
    const TerrainGround ground(terrain);

    // Units that move (designs under UNITS\UNITS: the hero, robots, animals) become live units; the rest
    // of the mission is the static scene, whose objects are also the static colliders.
    std::vector<std::unique_ptr<LiveUnit>> live;
    std::vector<bool> is_live(tma.objects.size(), false);
    std::size_t hero_slot = 0;
    for (std::size_t i = 0; i < tma.objects.size(); ++i) {
        if (!contains_ci(tma.objects[i].resource, "\\UNITS\\") || !contains_ci(tma.objects[i].resource, ".dat")) continue;
        auto u = LiveUnit::load(lib, tma.objects[i], ground);
        if (!u) continue;
        if (i == *hero_index) hero_slot = live.size();
        is_live[i] = true;
        live.push_back(std::move(u));
    }
    if (!is_live[*hero_index]) throw std::runtime_error("hero design cannot move: " + tma.objects[*hero_index].resource);
    // Buildings (data.tma kind 0; notes/buildings.md): CBuilding bodies with doors, a capture door and
    // damage parts. They are drawn and posed separately, but stay walls for the units.
    std::vector<std::unique_ptr<Building>> buildings;
    std::vector<bool> is_building(tma.objects.size(), false);
    for (std::size_t i = 0; i < tma.objects.size(); ++i) {
        const mission::TmaObject& o = tma.objects[i];
        if (o.kind != 0 || !contains_ci(o.resource, ".dat")) continue;
        try {
            if (auto body = LiveUnit::load_building(lib, o)) {
                buildings.push_back(std::make_unique<Building>(std::move(body), game::type_code_for_resource(o.resource)));
                is_building[i] = true;
            }
        } catch (const std::exception& ex) {
            SDL_Log("play: building %s: %s", o.resource.c_str(), ex.what());
        }
    }
    RenderMesh scene = load_mission_render_mesh(lib, opt.mission, nullptr, [&](std::size_t i) { return is_live[i] || is_building[i]; });
    const std::uint32_t statics_end = std::uint32_t(scene.transforms.size());
    for (auto& b : buildings) {
        LiveUnit& body = b->body();
        body.first_transform = append_render_mesh(scene, body.model.mesh, Mat4::identity());
        body.pose(scene.transforms, false);
    }
    // Walls: the static scene and the buildings in their static pose (tag k + 1: a removed building's go).
    StaticColliders statics;
    statics.add(scene, 1, statics_end); // the terrain's draws use transform 0
    for (std::size_t k = 0; k < buildings.size(); ++k) {
        const LiveUnit& body = buildings[k]->body();
        for (std::uint32_t t = 0; t < body.model.mesh.transforms.size(); ++t)
            if (!buildings[k]->moving_transform(t)) statics.add(scene, body.first_transform + t, body.first_transform + t + 1, std::uint32_t(k + 1));
    }
    statics.finish();
    TraceMesh trace_statics; // every static triangle, for bullets and sight rays (buildings: combat)
    trace_statics.add(scene, 1, statics_end);
    trace_statics.finish();
    for (auto& u : live) u->first_transform = append_render_mesh(scene, u->model.mesh, Mat4::identity());

    // Weapons, damage and death (notes/weapons-damage.md): every live unit is a combatant; the hero's
    // guns follow HERO.TBL (1–8 / 0 select, LMB fires the selection). The buildings follow the units
    // (combatant index live.size() + building index).
    combat::CombatWorld combat(lib, ground, &trace_statics);
    for (std::size_t i = 0; i < live.size(); ++i)
        combat.add_unit(*live[i], live[i]->placement->identity, live[i]->placement->ai_index, i == hero_slot);
    for (auto& b : buildings) {
        const std::size_t ci = combat.add_unit(b->body(), b->body().placement->identity, b->body().placement->ai_index, false);
        SDL_Log("play: building %08x %s: %.0f HP, %zu doors, capture door %s", b->body().placement->identity, b->body().placement->resource.c_str(),
                combat.unit(ci).damage.initial, b->items().size() - (b->can_be_captured() ? 1 : 0), b->can_be_captured() ? "yes" : "no");
    }
    // Floors: object surfaces and building bodies (bridges, hangar floors), beside the landscape
    // (LiveUnit::settle / drop_to_ground).
    const LiveUnit::FloorProbe floor_probe = [&](Vec3 from, Vec3 to) -> std::optional<GroundHit> {
        const auto h = combat.trace_floor(from, to);
        return h ? std::optional<GroundHit>(GroundHit{h->point.z, h->face_normal}) : std::nullopt;
    };
    // DropToGround again with the object faces, now that the buildings stand: units placed on a bridge
    // or a building's floor (heroes starting in their base) stay there.
    for (auto& u : live) u->place(0, ground, floor_probe);
    // Index maps: live unit → combatant, building → combatant, and back. The placed units come first, then
    // the placed buildings; objects created while the mission runs (produced robots, constructed and
    // upgraded buildings) are appended to all three lists.
    std::vector<std::size_t> live_ci(live.size()), bld_ci(buildings.size());
    std::vector<std::int32_t> ci_live, ci_bld;
    for (std::size_t i = 0; i < live.size(); ++i) live_ci[i] = i, ci_live.push_back(std::int32_t(i)), ci_bld.push_back(-1);
    for (std::size_t k = 0; k < buildings.size(); ++k) bld_ci[k] = live.size() + k, ci_live.push_back(-1), ci_bld.push_back(std::int32_t(k));
    auto building_of = [&](std::size_t ci) -> Building* { return ci < ci_bld.size() && ci_bld[ci] >= 0 ? buildings[std::size_t(ci_bld[ci])].get() : nullptr; };
    // The TMA object of combatant `ci` (a live unit or a building).
    auto placement_of = [&](std::size_t ci) -> const mission::TmaObject& { return *combat.unit(ci).unit->placement; };
    combat::Combatant& hero_combat = combat.unit(hero_slot);
    if (opt.demo_fire) hero_combat.select(-1);
    {
        std::string guns;
        for (const auto& g : hero_combat.guns)
            guns += " " + g.spec().bullet.second + (g.ammo() < 0 ? std::string("(inf)") : "(" + std::to_string(g.ammo()) + ")");
        SDL_Log("play: hero guns:%s; %.0f HP", guns.c_str(), hero_combat.damage.initial);
    }
    // Bullets are drawn with their BULL prototype mesh until effects exist: a fixed pool of copies per
    // prototype appended to the scene, unused copies collapsed (zero matrices).
    struct BulletPool {
        const BulletProto* proto = nullptr;
        std::vector<Mat4> nodes;            // the mesh's static node matrices
        std::vector<std::uint32_t> firsts;  // first transform of each copy
    };
    std::vector<BulletPool> bullet_pools;
    constexpr std::size_t kBulletSlots = 48;
    for (const auto& [name, proto] : combat.bullet_protos()) {
        RenderMesh bm;
        try {
            bm = to_render_mesh(load_model(lib, proto->record.mesh().first, proto->record.mesh().second));
        } catch (const std::exception& ex) {
            SDL_Log("play: bullet %s mesh: %s", name.c_str(), ex.what());
        }
        if (bm.draws.empty()) continue;
        BulletPool pool{proto.get(), bm.transforms, {}};
        for (std::size_t k = 0; k < kBulletSlots; ++k) pool.firsts.push_back(append_render_mesh(scene, bm, Mat4::identity()));
        bullet_pools.push_back(std::move(pool));
    }
    LiveUnit& hero = *live[hero_slot];
    UnitMotion& motion = *hero.motion;
    std::vector<LiveUnit*> live_ptrs;
    for (auto& u : live) live_ptrs.push_back(u.get());
    // Computer-controlled units (notes/unit-ai-runtime.md): every live unit but the hero gets a brain.
    // Animals migrate in their clan's areals (data.tma mode-0 clans); other units hold until ordered.
    LandMap land_map;
    if (auto lm = resolve_path(lib.game_dir(), map_dir + "/Land.map")) land_map = decode_landmap(read_file(*lm));
    land_map.set_bounds(ground.bounds_min().x, ground.bounds_min().y, ground.bounds_max().x, ground.bounds_max().y);
    const ai::ArealGraph graph = ai::ArealGraph::build(land_map);
    ai::MsvcRand rng{1}; // the CRT's rand(), shared by every brain
    std::vector<std::unique_ptr<ai::ClanMigration>> migrations(tma.clans.size());
    std::vector<std::unique_ptr<ai::AiUnit>> brains(live.size());
    auto make_brain = [&](std::size_t i) {
        const mission::TmaObject& o = *live[i]->placement;
        const bool animal = contains_ci(o.resource, "\\ANIMAL\\");
        // Animals are flyers here (chas_fly: CanFly 1, WalkChassis 0; their chassis sheets are not
        // traced — tools/ai_tests.cpp). Robots: their chassis prototype's sheet (BTLU +0x160) — chas_fly.var
        // flies (CanFly 1, WalkChassis 0); the others keep the walking default.
        ai::AiUnitConfig cfg;
        if (animal) cfg = {true, false, true};
        else if (contains_ci(live[i]->model.chassis_sheet, "chas_fly")) cfg = {true, false, false};
        brains[i] = std::make_unique<ai::AiUnit>(*live[i]->motion, land_map, graph, [&rng] { return rng(); }, cfg);
        brains[i]->set_ground_height([&ground](float x, float y) {
            auto g = ground.probe({x, y, 1000}, 2000.f);
            return g ? g->z : 0.f;
        });
        if (animal && o.ai_index < tma.clans.size() && !tma.clans[o.ai_index].spatial.empty()) {
            auto& mig = migrations[o.ai_index];
            if (!mig) mig = std::make_unique<ai::ClanMigration>(ai::ClanMigration{tma.clans[o.ai_index].spatial, {}, -1});
            brains[i]->set_migration(mig.get());
        }
    };
    for (std::size_t i = 0; i < live.size(); ++i)
        if (i != hero_slot) make_brain(i);

    // Sound (notes/audio.md): in a mission a random track plays once, then a different random one; the
    // listener is the rendered camera. Not started for --shot.
    // The briefing (notes/shell.md "Briefing"; CMission::LoadNewMission 100a1f30, CBriefing::Load 10031330):
    // a new mission entered from the shell plays mission.cfg's `object briefing` fly-through first (view
    // mode 5). A missing object, file or a parse error leaves the mission to start at once.
    shell::BriefingSetup briefing_setup;
    std::optional<shell::BriefingPath> briefing;
    std::string briefing_title; // <mission>/descr (CGame+0xd4), cp1251
    bool briefing_started = false, briefing_skip = false;
    if (auto p = resolve_path(*dir, "mission.cfg")) {
        const Bytes b = read_file(*p);
        briefing_setup = shell::parse_briefing_setup({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    if (opt.briefing && !briefing_setup.filename.empty()) {
        try {
            if (auto bp = resolve_path(*dir, briefing_setup.filename)) {
                const Bytes b = read_file(*bp);
                briefing.emplace(shell::parse_briefing_cfg({reinterpret_cast<const char*>(b.data()), b.size()}));
                if (briefing->waypoints().empty()) briefing.reset();
            }
        } catch (const std::exception& ex) {
            SDL_Log("play: briefing: %s", ex.what());
            briefing.reset();
        }
        if (auto d = resolve_path(*dir, "descr")) {
            const Bytes b = read_file(*d);
            briefing_title.assign(b.begin(), std::find(b.begin(), b.end(), '\n'));
            while (!briefing_title.empty() && (briefing_title.back() == '\r' || briefing_title.back() == ' ')) briefing_title.pop_back();
        }
        if (briefing) SDL_Log("play: briefing %s, %zu waypoints", briefing_setup.filename.c_str(), briefing->waypoints().size());
    }
    std::unique_ptr<audio::AudioEngine> sound;
    if (opt.shot_path.empty()) {
        sound = std::make_unique<audio::AudioEngine>(lib);
        if (!sound->ok()) sound.reset();
        if (sound && opt.sfx_volume >= 0) sound->set_gain(audio::Bus::Sfx, opt.sfx_volume);
        if (sound && opt.music_volume >= 0) sound->set_gain(audio::Bus::Music, opt.music_volume);
        if (sound && !opt.music) sound->set_gain(audio::Bus::Music, 0); // PLAY_CD_MUSIC = 0 (port: the tracks stay silent)
        // initGame: MusicPlayRandomTrack unless the view mode is 5; CBriefing::Load: MusicStop, then the
        // briefing's cd_track looped (C01M01 only).
        if (sound && !briefing) sound->play_random_music(false);
        else if (sound && briefing_setup.cd_track) sound->play_music(*briefing_setup.cd_track, true);
    }
    std::size_t footsteps = 0;
    std::map<std::string, std::vector<FxSound>> fx_sounds; // effect → its sound commands
    auto play_effect_sounds = [&](const ResName& fx, Vec3 pos) {
        if (!sound || fx.second.empty()) return;
        auto [it, fresh] = fx_sounds.try_emplace(fx.second);
        if (fresh) it->second = load_fx_sounds(lib, fx.second);
        for (const auto& s : it->second) sound->play(audio::fx_sound(s), pos);
    };
    std::size_t kills = 0;
    // Effects (src/fx): bullet tracers ride their bullet, impacts and part explosions play once; their
    // sound commands drive positional voices.
    fx::FxContext fx_ctx(lib);
    std::map<std::string, std::shared_ptr<const FxEffect>> fx_templates;
    struct LiveEffect {
        fx::EffectInstance inst;
        std::uint32_t bullet = 0;
        bool stopped = false;
        std::array<std::int32_t, 3> end_actions{}; // see CombatEvent::end_actions
        float removal_ms = 0;
        double remove_at = 0; // the bullet object's removal (s), once it has died
        std::map<std::size_t, audio::Handle> voices; // sound command → voice
        std::int32_t owner = -1;  // attached: follows this combatant's frame (CombatEvent::attached)
        std::uint32_t handle = 0; // CombatEvent::handle
    };
    std::vector<LiveEffect> effects;
    auto start_effect = [&](const combat::CombatEvent& e, double now) {
        auto [it, fresh] = fx_templates.try_emplace(e.effect.second);
        if (fresh) {
            try {
                it->second = fx::load_effect(lib, e.effect.second);
            } catch (const std::exception& ex) {
                SDL_Log("play: effect %s: %s", e.effect.second.c_str(), ex.what());
            }
        }
        if (!it->second) { // no template: its sounds at least
            play_effect_sounds(e.effect, e.pos);
            return;
        }
        // A keyed instance (the shield's ring of three) is restarted: the old one goes.
        if (e.handle) std::erase_if(effects, [&](const LiveEffect& x) { return x.handle == e.handle; });
        Mat4 m = e.placement ? *e.placement : combat::CombatWorld::bullet_matrix(e.pos, e.axis);
        Mat4 owner = Mat4::identity();
        if (e.attached && e.unit >= 0) { // CreateEffect attached to the object: its matrix is kept relative to it
            owner = combat.unit(std::size_t(e.unit)).unit->frame.matrix;
            m = fx::inverse_affine(owner) * m;
        }
        effects.push_back({fx::EffectInstance(it->second, fx_ctx, m, now), e.bullet, false, e.end_actions, e.removal_ms, 0, {},
                           e.attached ? e.unit : -1, e.handle});
        LiveEffect& le = effects.back();
        if (le.owner >= 0) le.inst.set_owner(owner);
        le.inst.set_spawn_frame(e.frame); // bullet tracers: the CTPT frame (×0.03)
        if (e.scale != 1) le.inst.set_scale({e.scale, e.scale, e.scale});
        if (e.start_mode >= 0) le.inst.start(now, std::uint32_t(e.start_mode)); // StartEffect(handle, mode)
    };
    std::map<std::uint32_t, int> ended_bullets; // bullet → its end situation (CombatEvent::BulletEnded)
    auto update_effects = [&](double now) {
        std::map<std::uint32_t, const combat::Bullet*> flying;
        for (const auto& b : combat.bullets()) flying[b.id] = &b;
        std::vector<fx::FxSoundEvent> sounds;
        for (auto& e : effects) {
            if (e.bullet && !e.stopped) {
                if (auto f = flying.find(e.bullet); f != flying.end()) {
                    e.inst.set_transform(combat::CombatWorld::bullet_matrix(f->second->pos, f->second->dir));
                } else { // the bullet died: its end situation's list acts on the effect (BulletProto::end_actions)
                    const auto en = ended_bullets.find(e.bullet);
                    const int situation = en != ended_bullets.end() ? en->second : 2;
                    const std::int32_t act = e.end_actions[std::size_t(situation - 2)];
                    if (act >= 0) e.inst.start(now, std::uint32_t(act));
                    else if (act == BulletProto::kEffectStop) e.inst.stop(now);
                    else if (act == BulletProto::kEffectDisable) e.inst.set_enabled(false, now);
                    e.stopped = true;
                    e.remove_at = act == BulletProto::kEffectDestroy ? now : now + e.removal_ms / 1000;
                }
            }
            if (e.owner >= 0) {
                const combat::Combatant& o = combat.unit(std::size_t(e.owner));
                if (o.removed) { // its effect manager goes with the object
                    e.bullet = ~0u;
                    continue;
                }
                e.inst.set_owner(o.unit->frame.matrix);
            }
            sounds.clear();
            const bool alive = e.inst.update(now, &sounds);
            if (sound)
                for (const auto& s : sounds) {
                    if (s.kind == fx::FxSoundEvent::Play && s.sound) {
                        audio::Sound snd = audio::fx_sound(*s.sound);
                        snd.pitch *= s.pitch;
                        e.voices[s.command] = sound->play(snd, s.pos);
                    } else if (auto v = e.voices.find(s.command); v != e.voices.end()) {
                        if (s.kind == fx::FxSoundEvent::Stop) sound->stop(v->second), e.voices.erase(v);
                        else sound->set_position(v->second, s.pos, {});
                    }
                }
            // A bullet's effects go with the bullet object, body[18] after its death (its effect manager is
            // released with it).
            if (!alive || (e.stopped && now >= e.remove_at)) e.bullet = ~0u; // marked for removal
        }
        std::erase_if(effects, [](const LiveEffect& e) { return e.bullet == ~0u; });
        ended_bullets.clear();
    };

    // The mission's strategy layer (notes/script-vm.md): one SuperAI per clan running the clan's script;
    // its orders reach the brains through UnitAdapter, its messages and objectives the HUD below.
    std::unique_ptr<game::MissionRuntime> runtime;
    std::vector<std::unique_ptr<UnitAdapter>> adapters;
    try {
        runtime = game::MissionRuntime::load(lib.game_dir(), opt.mission);
    } catch (const std::exception& ex) {
        SDL_Log("play: no mission scripts (%s)", ex.what());
    }
    if (runtime) {
        runtime->world().map_max_x = ground.bounds_max().x, runtime->world().map_max_y = ground.bounds_max().y;
        for (std::size_t i = 0; i < live.size(); ++i) {
            adapters.push_back(std::make_unique<UnitAdapter>(*live[i], brains[i].get()));
            runtime->world().attach(live[i]->placement->identity, adapters.back().get());
        }
    }

    // The brains' combat layer (notes/unit-ai-runtime.md §4): perception over the live units and the world
    // model, one fire-control weapon per gun that can fire.
    std::map<std::uint32_t, std::size_t> unit_of; // LogicID → live unit
    std::map<std::uint32_t, std::uint32_t> tma_clans, tma_types;
    for (std::size_t i = 0; i < combat.size(); ++i) { // live units, then buildings
        const auto& o = placement_of(i);
        unit_of[o.identity] = i;
        tma_clans[o.identity] = o.ai_index;
        tma_types[o.identity] = game::type_code_for_resource(o.resource);
    }
    WorldPerception perception(combat, runtime ? &runtime->world() : nullptr, ground, trace_statics, unit_of, tma_clans, tma_types);
    for (auto& b : buildings) {
        const auto spheres = b->rest_capture_spheres();
        if (spheres.empty()) continue;
        Vec3 c{};
        for (const auto& s : spheres) c = c + s.centre * (1.f / float(spheres.size()));
        if (auto g = ground.probe(c + Vec3{0, 0, 100}, 200.f)) c.z = g->z;
        perception.set_entry(b->body().placement->identity, c);
        for (const auto& s : spheres)
            SDL_Log("play: building %08x capture sphere (%.1f, %.1f, %.1f) r %.1f", b->body().placement->identity, s.centre.x, s.centre.y,
                    s.centre.z, s.radius);
    }
    std::vector<std::vector<std::size_t>> weapon_guns(live.size()); // fire-control weapon → gun
    // The identity and one fire-control weapon per gun that can fire, for combatant `ci`.
    auto arm_fire = [&](std::size_t ci, ai::FireControl& fc, std::vector<std::size_t>& guns) {
        const auto& o = placement_of(ci);
        combat::Combatant& c = combat.unit(ci);
        ai::SelfInfo self;
        self.logic_id = o.identity, self.clan = o.ai_index, self.type = tma_types[o.identity];
        if (runtime)
            if (const game::WorldObject* w = runtime->world().find(o.identity)) self.size_class = w->size_class; // property 0x201
        self.has_weapons = false;
        fc.weapons.clear();
        guns.clear();
        for (std::size_t g = 0; g < c.guns.size(); ++g) {
            const auto& gun = c.guns[g];
            if (gun.barrel_count() == 0 || !gun.bullet()) continue;
            // MWeaponPart init (Behavior 1001b4b0, read 2026-10-23): rate = device property 0x800, envelope
            // near 5 (animals 0) / opt (h + 1)/2 / far 2(h + 1) from +0x14 = a device property not resolved;
            // port: h = the bullet's range, rate = rounds per second from the barrel steps and param 3.
            ai::Weapon w;
            w.group = c.mounts[g].on_turret() ? 0 : -1;
            const auto& joints = gun.joints();
            const std::uint32_t j = gun.barrel_joint(0);
            const float step_ms = j < joints.size() && joints[j].rate > 0 ? 1000.f / joints[j].rate : 0.f;
            w.rate = 1000.f / std::max(1.f, step_ms + gun.param(3));
            w.height = gun.bullet()->range;
            w.near = self.animal() ? 0.f : 5.f;
            w.opt = (w.height + 1) * 0.5f;
            w.far = (w.height + 1) * 2;
            w.aim_error = w.group < 0 ? 0.f : 3.1415927f; // device slot 0x28 before any aiming: not aimed (π)
            fc.weapons.push_back(w);
            guns.push_back(g);
            self.has_weapons = true;
        }
        fc.turret_error.assign(1, 3.1415927f);
        return self;
    };
    auto arm_brain = [&](std::size_t i) {
        if (!brains[i]) return;
        const ai::SelfInfo self = arm_fire(live_ci[i], brains[i]->fire_control(), weapon_guns[i]);
        brains[i]->set_perception(&perception, self);
    };
    for (std::size_t i = 0; i < live.size(); ++i) arm_brain(i);
    // Buildings with weapons (bunkers, towers): MBehaviour's building tick and fire control (OnTakt runs the
    // fire control for a building too, slot 57 in place of MovementTakt; the fire channel is taken as AI,
    // MB+0xa04 & 0x40: the building's wizard is not traced).
    std::vector<std::unique_ptr<ai::BuildingAi>> bld_brains(buildings.size());
    std::vector<std::vector<std::size_t>> bld_weapon_guns(buildings.size());
    auto arm_building = [&](std::size_t k) {
        const std::size_t ci = bld_ci[k];
        if (combat.unit(ci).guns.empty()) return;
        LiveUnit& body = buildings[k]->body();
        auto b = std::make_unique<ai::BuildingAi>(land_map, graph, [&rng] { return rng(); }, [&body] { return body.frame.matrix; });
        const ai::SelfInfo self = arm_fire(ci, b->fire_control(), bld_weapon_guns[k]);
        if (!self.has_weapons) return;
        // The line of fire starts at the weapons (1001cd00, not traced; port: the first gun's muzzle height),
        // high on the turret of a bunker or tower.
        if (auto m = combat.muzzle(ci, bld_weapon_guns[k][0])) b->fire_control().muzzle_height = m->z - body.position().z;
        b->set_perception(&perception, self);
        bld_brains[k] = std::move(b);
    };
    for (std::size_t k = 0; k < buildings.size(); ++k) arm_building(k);

    // In-mission text (notes/rsli-font.md §3): mission texts from TextRes, the HUD's own strings from
    // iron3d.dll's string table; both are drawn in the game's fonts by cp1251 byte.
    TextRes texts, ui_strings;
    if (auto p = resolve_path(lib.game_dir(), "DATA/TextRes.dll")) texts = TextRes::from_pe(read_file(*p));
    if (auto p = resolve_path(lib.game_dir(), "DATA/TextRes.cfg")) {
        const Bytes b = read_file(*p);
        texts.load_names({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    if (auto p = resolve_path(lib.game_dir(), "iron3d.dll")) ui_strings = TextRes::from_pe(read_file(*p));
    auto ui_text = [&](std::uint32_t id) { return utf8_to_cp1251(ui_strings.get(id)); };
    // Fonts: GAME_FONT (gf_*) for the pager, MENU_FONT (mf_*) for the objectives; one set per resolution.
    struct FontSet {
        int size;
        TftFont game, menu;
    };
    std::vector<FontSet> font_sets;
    std::vector<ModelTexture> font_textures;
    for (int size : {640, 800, 1024}) {
        try {
            const auto& a = lib.archive("UI/font.lib");
            const NResEntry *g = a.find_name("gf_" + std::to_string(size) + ".tft"), *m = a.find_name("mf_" + std::to_string(size) + ".tft");
            if (!g || !m) continue;
            font_sets.push_back({size, decode_tft(g->data), decode_tft(m->data)});
            for (const TftFont* f : {&font_sets.back().game, &font_sets.back().menu})
                font_textures.push_back({"font", f->atlas_width, f->atlas_height, f->rgba});
        } catch (const std::exception& ex) {
            SDL_Log("play: fonts: %s", ex.what());
            break;
        }
    }
    // The cockpit (notes/hud.md): the ui.lib pages and the mission's satellite map, uploaded after the fonts.
    ui::HudTextures hud_tex;
    ui::HqSprites hq_sprites; // hq.cfg / compaund.cfg by name (the HQ interface, notes/hq-ui.md §3.4)
    try {
        const ui::HudResources hr = ui::load_hud_resources(lib, &*dir);
        auto add = [&](const Texm* t, const char* name) -> std::int32_t {
            if (!t || t->mips_rgba.empty()) return -1;
            font_textures.push_back({name, t->width, t->height, t->mips_rgba.front()});
            return std::int32_t(font_textures.size() - 1);
        };
        auto page = [&](const char* name) { auto it = hr.pages.find(name); return add(it != hr.pages.end() ? &it->second : nullptr, name); };
        hud_tex.page6 = page("page6"), hud_tex.page7 = page("page7"), hud_tex.page9 = page("page9");
        hud_tex.ui_menu = page("ui_menu"), hud_tex.ui_menu3 = page("ui_menu3");
        hud_tex.minimap = add(hr.minimap ? &*hr.minimap : nullptr, "minimap");
        hq_sprites = ui::make_hq_sprites(hr.sprites);
    } catch (const std::exception& ex) {
        SDL_Log("play: HUD textures: %s", ex.what());
    }
    // Message sources: messages.cfg (`info_system` picks the header) and the voice table, which maps a
    // voice key to a voices.lib entry in mission.cfg's resource objects.
    std::set<std::uint32_t> info_messages;
    std::map<std::string, std::string> voice_entries;
    if (auto p = resolve_path(*dir, "messages.cfg")) {
        const Bytes b = read_file(*p);
        for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()})) {
            const std::string *idx = o.get("message_index"), *info = o.get("info_system");
            if (idx && info && *info == "true") info_messages.insert(std::uint32_t(std::atoi(idx->c_str())));
        }
    }
    if (auto p = resolve_path(*dir, "mission.cfg")) {
        const Bytes b = read_file(*p);
        for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()}))
            if (const std::string* l = o.get("library"); l && contains_ci(*l, "voices.lib"))
                for (const auto& [k, v] : o.entries) voice_entries[k] = v;
    }
    // The end of the mission (notes/mission-end.md): CGame+0x08 as a controller, the centred message
    // over the frozen scene, and the computer voices from UI/game_resources.cfg.
    ui::MissionEndController end_ctl;
    const ui::EndStrings end_str = ui::load_end_strings(ui_strings);
    std::map<std::string, std::string> voice_table;
    if (auto p = resolve_path(lib.game_dir(), "UI/game_resources.cfg")) {
        const Bytes b = read_file(*p);
        voice_table = ui::parse_voice_table({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    std::optional<ViewerOptions::Camera> spectator; // CMission_EnterSpectatorAtUnit after the hero's death
    // QueueUnitVoice with no unit: the 2D computer voice (VOICE_MISSION_COMPLETE / _FAIL).
    auto play_end_voice = [&](const char* key) {
        if (!key) return;
        SDL_Log("play: mission %s", end_ctl.state() == ui::GameState::Complete ? "complete" : "failed");
        if (auto v = voice_table.find(key); sound && v != voice_table.end()) sound->play({"voices.lib", v->second});
    };
    std::size_t leaving_frames = 0;
    bool quit = false;
    std::optional<ui::PagerMessage> pager;
    std::set<std::uint32_t> messages_seen;
    bool objectives_visible = false;
    double objectives_shown_ms = 0;
    bool objectives_auto_hide = false;
    const std::string help_key = key_label("CMD_HELP", opt.layout), objectives_key = key_label("CMD_JAMES_MISSION_OBJ", opt.layout);
    double sim_ms = 0;
    if (runtime) {
        runtime->set_event_sink([&](const game::MissionEvent& e) {
            SDL_Log("play: %.1f s clan %u: %s %u %s", sim_ms / 1000, e.clan, game::to_string(e.type), e.id, e.text.c_str());
            using T = game::MissionEvent::Type;
            if (e.type == T::Message) {
                // ShowScriptMessageByIndex: the first showing has the text and its voice; a repeat says
                // "you already received this message" (6170) with the pager-log footer (6223).
                const auto& msgs = runtime->config().messages;
                const auto it = msgs.find(e.id);
                ui::PagerMessage m;
                m.header = ui_text(info_messages.count(e.id) ? 6214 : 3057);
                m.more_footer = with_key(ui_text(6208), help_key);
                if (messages_seen.insert(e.id).second) {
                    m.body = it != msgs.end() ? utf8_to_cp1251(texts.get(std::string_view(it->second.text_resource))) : std::string();
                    if (sound && it != msgs.end())
                        if (auto v = voice_entries.find(it->second.voice_resource); v != voice_entries.end())
                            sound->play({"voices.lib", v->second});
                } else {
                    m.body = ui_text(6170) + "\n" + with_key(ui_text(6223), help_key);
                }
                m.shown_ms = sim_ms;
                pager = std::move(m);
            } else if (e.type == T::ObjectiveComplete || e.type == T::ObjectiveFailed) {
                // PostComputerPagerText: "from: the central computer" with 5040 / 5041.
                pager = ui::PagerMessage{ui_text(1541), ui_text(e.type == T::ObjectiveComplete ? 5040 : 5041), {}, sim_ms};
            } else if (e.type == T::MissionComplete || e.type == T::MissionFailed) {
                play_end_voice(e.type == T::MissionComplete ? end_ctl.mission_complete() : end_ctl.mission_failed());
            }
        });
        // Assumed: initGame shows the objectives at the start and lets them hide after 7 s.
        objectives_visible = !runtime->config().primary_objectives.empty(), objectives_auto_hide = true;
    }

    // Steady turret (port choice, --camera-sway turns it off): the walk clips twist the chassis root
    // (r_h_02: up to ~10° of yaw per stride) and everything mounted under it follows, in the original
    // too (notes/turret-camera.md "Walk sway"). The hero's turret, guns, sight and cockpit camera are
    // attached as if the root kept its rest rotation, so view and barrels stay together and still.
    hero.steady_mounts = opt.stabilize_camera;
    // Port debug aids: --goto puts the hero at (x, y[, heading]) on the ground; --damage-mult sets the
    // hero guns' damage multiplier (dev+0x180 → bullet property 0xb4, which the container property 0x11
    // sets in the original).
    if (opt.goto_xy) {
        Mat4 m = hero.frame.matrix;
        if (opt.goto_heading) {
            const float h = *opt.goto_heading * 3.1415927f / 180.f, c = std::cos(h), s = std::sin(h);
            m = Mat4::identity();
            m.at(0, 0) = c, m.at(0, 1) = -s, m.at(1, 0) = s, m.at(1, 1) = c; // Rz(h): forward (+y) turns by h
        }
        m.at(0, 3) = opt.goto_xy->first, m.at(1, 3) = opt.goto_xy->second;
        if (auto g = ground.probe({m.at(0, 3), m.at(1, 3), 2000.f}, 4000.f)) m.at(2, 3) = g->z;
        motion.start(0, m);
        hero.frame = motion.interpolate(0);
        hero.pose_parts();
    }
    if (opt.damage_mult != 1)
        for (auto& g : hero_combat.guns) g.damage_mult = opt.damage_mult;


    // Input: the hero is driven through HERO.TBL (iron3d loads it for the hero; warbots use M1/M2),
    // rebound to the chosen layout (docs/CONTROLS.md).
    std::vector<input::Binding> table;
    if (auto tbl = resolve_path(lib.game_dir(), "hero.tbl")) {
        const Bytes b = read_file(*tbl);
        table = input::parse_translate_table({reinterpret_cast<const char*>(b.data()), b.size()});
        input::apply_layout(table, opt.layout);
    }
    input::ManManager man(std::move(table));
    input::CommandState commands;
    std::vector<input::InputEvent> events;
    float mouse_dx = 0, mouse_dy = 0;

    float last_angle_z = 0.5f;
    // Cameras: 0 = TURRET view (first person, the default), 1..4 = OUTER presets (JAMES_OUTER_CAMERA
    // cycles them: V modern, C classic), -1 = the port's debug orbit (F10).
    int camera_mode = 0, outer_preset = 0;
    struct OuterParams {
        float yaw, height, dist;
    };
    static constexpr OuterParams kOuter[5] = {{0, 0, 0}, {0.4f, 0.15f, 2.5f}, {0.2f, 0.15f, 4.5f}, {-0.2f, 0.15f, 4.5f}, {-0.4f, 0.15f, 2.5f}};
    if (opt.camera_preset > 0 && opt.camera_preset < 5) camera_mode = 1, outer_preset = opt.camera_preset;
    OuterParams outer_cur = kOuter[outer_preset];
    double outer_blend_start = -1;
    Vec3 last_cam_forward{};
    std::size_t pushed_steps = 0;

    // HUD state (notes/hud.md §10): cockpit pages on/off, the satellite map and its alpha, the sight zoom,
    // the selected target (CTargeting+4; −1 none) and the last rendered camera (projections, the radar).
    bool cockpit_on = true, map_visible = false, zoomed = false;
    int map_alpha = 0x80;          // CSatelliteMap +0x90 (iron_3d.ini MAP_ALPHA is not read)
    double map_alpha_changed_ms = -1e9;
    float zoom_fov = -1;           // the turret camera's current FOV (CUnit +0x14); < 0 = not started
    std::int32_t hud_target = -1;
    double hud_target_since_ms = -1e9;
    std::optional<ViewerOptions::Camera> hud_cam;
    float max_speed_seen = 10.f;   // port: the speed arc's full scale (property 0x90 is not read)
    // Port stand-in for the radar range (unit property 0x50): the detected objects are the living units
    // within it.
    constexpr float kRadarRange = 300.f;
    const std::uint32_t hero_clan = hero.placement->ai_index;
    // A combatant's clan now (GetObjectClan: captures and deaths change it).
    auto clan_of = [&](std::size_t ci) -> std::uint32_t {
        if (runtime)
            if (const game::WorldObject* w = runtime->world().find(placement_of(ci).identity)) return w->clan;
        return combat.unit(ci).damage.clan;
    };
    auto detected = [&]() {
        std::vector<std::int32_t> out;
        for (std::size_t i = 0; i < combat.size(); ++i) {
            const combat::Combatant& c = combat.unit(i);
            if (i == hero_slot || c.removed || !c.alive()) continue;
            const Vec3 d = c.unit->position() - hero.position();
            if (d.x * d.x + d.y * d.y <= kRadarRange * kRadarRange) out.push_back(std::int32_t(i));
        }
        return out;
    };
    auto set_hud_target = [&](std::int32_t t) {
        if (t != hud_target) hud_target_since_ms = sim_ms;
        hud_target = t;
    };
    // CTargeting::SelectNearestEnemy / SelectNearestFriend over the detected objects.
    auto select_nearest = [&](bool enemy) {
        std::int32_t best = -1;
        float best_d = FLT_MAX;
        for (std::int32_t i : detected()) {
            const bool hostile = perception.hostile(hero_clan, clan_of(std::size_t(i)));
            if (hostile != enemy) continue;
            const Vec3 d = combat.unit(std::size_t(i)).unit->position() - hero.position();
            if (dot(d, d) < best_d) best_d = dot(d, d), best = i;
        }
        if (best >= 0) set_hud_target(best);
    };

    // The interface voices (ui/game_resources.cfg `object voices`: VOICE_* → voices.lib entry).
    std::map<std::string, std::string> game_voices;
    if (auto p = resolve_path(lib.game_dir(), "UI/game_resources.cfg")) {
        const Bytes b = read_file(*p);
        for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()}))
            if (o.name == "voices")
                for (const auto& [k, v] : o.entries) game_voices[k] = v;
    }
    auto play_voice = [&](const std::string& key) {
        SDL_Log("play: voice %s", key.c_str());
        if (auto v = game_voices.find(key); sound && v != game_voices.end()) sound->play({"voices.lib", v->second});
    };
    // The HQ interface and the view-state stack (notes/hq-ui.md; created once the HUD strings exist, below).
    std::unique_ptr<ui::HqController> hq;
    std::function<void()> hq_frame; // its per-frame part (HqUi_Update and the rest), set below
    std::function<ui::HqPick(float, float)> hq_pick; // what lies under a virtual-screen point, set below
    float hq_aspect = 4.f / 3.f; // drawable width / height of the last frame
    double hq_cam_ms = -1;       // the HQ camera's last update (game clock)
    ui::HqPick hq_last_pick;     // under the pointer, refreshed every frame
    bool relative_mouse_on = true;
    // The capture callback (iron3d OnBuildingCaptureCallback 10061540): unit `i` was inside building `k`
    // when its capture door finished opening. Same clan: the unit enters the building (for the player's
    // hero, CMission::OnUnitEntersOwnBuilding 10062ad0 opens the building's view; ui::HqController). Otherwise the
    // owner changes (CUnit::SetClan 10033190 → MBehaviour::Capture 10008e40) and CMission::OnObjectCaptured
    // (100a4840) tells the player: their capture → pager 5039 from the central computer and a voice by
    // the old owner (neutral clan or relation 1 → VOICE_NBUILD_CAPTURE, hostile → VOICE_EBUILD_CAPTURE,
    // else VOICE_BUILD_CAPTURE); their loss → VOICE_BUILD_CAPTURE.
    auto on_building_entered = [&](std::size_t k, std::size_t i) { // i: the unit's combatant
        const std::size_t ci = bld_ci[k];
        const std::uint32_t lid = placement_of(ci).identity, new_clan = clan_of(i), old_clan = clan_of(ci);
        if (new_clan == combat::kKilledClan) return;
        if (new_clan == old_clan) {
            SDL_Log("play: %.1f s unit %08x enters its clan's building %08x", sim_ms / 1000, placement_of(i).identity, lid);
            // 10062ad0: the player's hero (or a manually driven unit of the clan: remote control is not ported).
            if (hq && i == hero_slot && new_clan == hero_clan) hq->hero_enters_building(placement_of(i).identity, lid);
            return;
        }
        SDL_Log("play: %.1f s building %08x (%s) captured by unit %08x: clan %u -> %u", sim_ms / 1000, lid, placement_of(ci).resource.c_str(),
                placement_of(i).identity, old_clan, new_clan);
        combat.unit(ci).damage.clan = new_clan;
        if (runtime) {
            // MBehaviour::Capture (10008e40): NotifyGamePlay({7}) (slot 51, 1000d580) while the building still
            // belongs to the old owner → its SuperAI runs Fort_Captured (NotifyObjectEvent 7); then the maps.
            runtime->object_event(lid, 7);
            runtime->object_captured(lid, new_clan); // world clan, SuperAI maps (old map out, new in)
        }
        if (new_clan == hero_clan) {
            std::uint32_t relation = 2;
            if (runtime)
                if (game::SuperAI* ai = runtime->super_ai(old_clan)) relation = ai->relation(hero_clan);
            const bool neutral = old_clan < tma.clans.size() && tma.clans[old_clan].mode == 3;
            pager = ui::PagerMessage{ui_text(1541), ui_text(5039), {}, sim_ms};
            play_voice(neutral || relation == 1 ? "VOICE_NBUILD_CAPTURE" : relation == 0 ? "VOICE_EBUILD_CAPTURE" : "VOICE_BUILD_CAPTURE");
        } else if (old_clan == hero_clan) {
            play_voice("VOICE_BUILD_CAPTURE");
        }
        // CMission::OnObjectCaptured ends with OnUnitEntersOwnBuilding: the capturer now owns it.
        if (hq && i == hero_slot && new_clan == hero_clan) hq->hero_enters_building(placement_of(i).identity, lid);
    };

    // Objects created while the mission runs (the economy's events; notes/base-building.md §2.2, §3.2). Each
    // gets a TMA-like record (LiveUnit::placement), a body, a combatant, and for a robot a brain with its
    // perception and fire control and a UnitAdapter in the world; the viewer's grow hook appends their
    // meshes (and a building's walls) before they are drawn.
    std::deque<mission::TmaObject> spawned_tma;
    std::vector<std::size_t> pending_units, pending_buildings;
    std::vector<bool> unit_drawn(live.size(), true), building_drawn(buildings.size(), true);
    std::vector<Vec3> unit_start;
    economy::Economy* eco = runtime ? runtime->economy() : nullptr;
    // A building's batteries and power devices are its combatant's (combat/energy.hpp): the economy reads the
    // charge and pf from them and refills them through the distributor.
    auto bind_power = [&](std::size_t ci) {
        if (!eco) return;
        combat::Combatant& c = combat.unit(ci);
        eco->bind_power(c.logic_id, &c.energy, [&c](std::int32_t t) { return c.part_ratio(t); });
        if (const economy::EcoState* st = eco->state(c.logic_id))
            SDL_Log("play: building %08x power: %zu devices, battery %.0f (charge %.2f), pf %.2f", c.logic_id, c.energy.devices.size(),
                    st->battery_cap, st->battery_frac, st->power_factor());
    };
    for (const std::size_t ci : bld_ci) bind_power(ci);
    // The economy's walking tasks move builders and transports of every clan through their brains.
    std::map<std::uint32_t, std::unique_ptr<BrainMover>> movers;
    auto give_mover = [&](std::size_t i) {
        if (!eco || !brains[i]) return;
        const std::uint32_t lid = live[i]->placement->identity;
        const game::WorldObject* w = runtime->world().find(lid);
        if (!w || (w->type != game::kTypeBuilder && w->type != game::kTypeTransport)) return;
        auto& m = movers[lid];
        m = std::make_unique<BrainMover>(*brains[i]);
        eco->set_mover(lid, m.get());
    };
    // Bounding spheres for the placement test (economy radius_of): the bodies' spheres; a building type
    // without a body yet (the ghost, a builder's site) from its level-0 design's model bounds.
    std::map<std::uint32_t, float> type_radius;
    if (eco) {
        for (std::size_t i = 0; i < live.size(); ++i) give_mover(i);
        eco->ground_height = [&ground](float x, float y) {
            auto g = ground.probe({x, y, 2000.f}, 4000.f);
            return g ? g->z : 0.f;
        };
        runtime->world().terrain_height = [&ground](float x, float y) -> std::optional<float> {
            if (x < ground.bounds_min().x || y < ground.bounds_min().y || x > ground.bounds_max().x || y > ground.bounds_max().y) return std::nullopt;
            auto g = ground.probe({x, y, 2000.f}, 4000.f);
            return g ? std::optional<float>(g->z) : std::nullopt;
        };
        eco->radius_of = [&](const game::WorldObject& o) -> float {
            if (auto it = unit_of.find(o.logic_id); o.logic_id != game::kNoObject && it != unit_of.end()) {
                const combat::Combatant& c = combat.unit(it->second);
                return c.building() ? c.unit->sphere_radius : c.radius();
            }
            auto [it, fresh] = type_radius.try_emplace(o.type, 0.f);
            if (fresh && (o.type & game::kBuildingBit))
                if (const std::string* path = eco->building_scheme(o.type, 0))
                    if (const mission::Scheme* sc = eco->schemes().get(*path)) try {
                            if (auto m = load_unit_model(lib, *sc)) {
                                const Vec3 ext = m->mesh.bounds_max - m->mesh.bounds_min;
                                it->second = 0.5f * std::sqrt(dot(ext, ext));
                            }
                        } catch (const std::exception&) {
                        }
            if (it->second <= 0) it->second = (o.type & game::kBuildingBit) ? 30.f : o.size_class >= 3 ? 8.f : 4.f; // economy's defaults
            return it->second;
        };
    }
    auto spawn_unit = [&](std::uint32_t lid, const std::string& scheme_path) {
        const game::WorldObject* w = runtime->world().find(lid);
        if (!w) return;
        mission::TmaObject& o = spawned_tma.emplace_back();
        o.kind = 1, o.resource = scheme_path, o.ai_index = w->clan, o.identity = lid;
        o.position = {w->placed.x, w->placed.y, w->placed.z}, o.scale = {1, 1, 1};
        std::unique_ptr<LiveUnit> u;
        try {
            u = LiveUnit::load(lib, o, ground, eco ? eco->schemes().get(scheme_path) : nullptr, float(sim_ms));
        } catch (const std::exception& ex) {
            SDL_Log("play: produced unit %08x %s: %s", lid, scheme_path.c_str(), ex.what());
        }
        if (!u) {
            SDL_Log("play: produced unit %08x %s has no body (no motion graph)", lid, scheme_path.c_str());
            return;
        }
        const std::size_t i = live.size();
        live.push_back(std::move(u));
        const std::size_t ci = combat.add_unit(*live[i], lid, w->clan, false);
        live_ci.push_back(ci), ci_live.push_back(std::int32_t(i)), ci_bld.push_back(-1);
        unit_of[lid] = ci, tma_clans[lid] = w->clan, tma_types[lid] = w->type;
        perception.add_unit(lid, ci, w->clan, w->type);
        brains.emplace_back(), weapon_guns.emplace_back();
        make_brain(i);
        arm_brain(i);
        adapters.push_back(std::make_unique<UnitAdapter>(*live[i], brains[i].get()));
        runtime->world().attach(lid, adapters.back().get()); // the runtime re-wraps it in the economy's control
        give_mover(i);
        unit_drawn.push_back(false), pending_units.push_back(i), unit_start.push_back(live[i]->position());
        const Vec3 p = live[i]->position();
        SDL_Log("play: %.1f s unit %08x (%s, clan %u) produced at (%.0f, %.0f, %.0f)", sim_ms / 1000, lid, scheme_path.c_str(), w->clan, p.x, p.y, p.z);
    };
    // A new building's body at its world object (BuildingPlaced; BuildingUpgraded replaces the old body).
    auto spawn_building = [&](std::uint32_t lid) {
        const game::WorldObject* w = runtime->world().find(lid);
        if (!w) return;
        mission::TmaObject& o = spawned_tma.emplace_back();
        o.kind = 0, o.resource = w->resource, o.ai_index = w->clan, o.identity = lid, o.scale = {1, 1, 1};
        Vec3 p = w->placed;
        if (auto g = ground.probe({p.x, p.y, 2000.f}, 4000.f)) p.z = g->z;
        o.position = {p.x, p.y, p.z}, o.orientation = {0, 0, eco ? eco->angle(lid) : 0.f};
        std::unique_ptr<LiveUnit> body;
        try {
            body = LiveUnit::load_building(lib, o, eco ? eco->schemes().get(w->resource) : nullptr);
        } catch (const std::exception& ex) {
            SDL_Log("play: building %08x %s: %s", lid, w->resource.c_str(), ex.what());
        }
        if (!body) {
            SDL_Log("play: building %08x %s has no body", lid, w->resource.c_str());
            return;
        }
        if (auto it = unit_of.find(lid); it != unit_of.end()) // an upgrade: the old body goes silently
            if (Building* old = building_of(it->second)) {
                old->removed = true;
                combat.unit(it->second).removed = true;
                statics.remove(std::uint32_t(ci_bld[it->second] + 1));
            }
        const std::size_t k = buildings.size();
        buildings.push_back(std::make_unique<Building>(std::move(body), w->type));
        const std::size_t ci = combat.add_unit(buildings[k]->body(), lid, w->clan, false);
        bld_ci.push_back(ci), ci_live.push_back(-1), ci_bld.push_back(std::int32_t(k));
        bind_power(ci);
        unit_of[lid] = ci, tma_clans[lid] = w->clan, tma_types[lid] = w->type;
        perception.add_unit(lid, ci, w->clan, w->type);
        bld_brains.emplace_back(), bld_weapon_guns.emplace_back();
        arm_building(k);
        if (const auto spheres = buildings[k]->rest_capture_spheres(); !spheres.empty()) {
            Vec3 c{};
            for (const auto& sp : spheres) c = c + sp.centre * (1.f / float(spheres.size()));
            if (auto g = ground.probe(c + Vec3{0, 0, 100}, 200.f)) c.z = g->z;
            perception.set_entry(lid, c);
        }
        building_drawn.push_back(false), pending_buildings.push_back(k);
        SDL_Log("play: %.1f s building %08x %s (clan %u) at (%.0f, %.0f), %.0f HP", sim_ms / 1000, lid, w->resource.c_str(), w->clan, p.x, p.y,
                combat.unit(ci).damage.initial);
    };

    // The economy's events (MissionRuntime::set_economy_sink): bodies for the new objects, and the player's
    // notices: VOICE_UNIT_READY for an own robot (OnWarbotBuiltVoiceCallback 10033650: own clan, not the
    // autodemo), the research completion (ResearchUI_PollCompleted 10089e60: pager "%s (%s)" with 2010 and
    // the item's name, VOICE_RSRCH_COMPLETE).
    if (runtime)
        runtime->set_economy_sink([&](const economy::EconomyEvent& e) {
            using T = economy::EconomyEvent::Type;
            SDL_Log("play: %.1f s economy: %s %08x (source %08x, clan %u, item %d, code %x) %s", sim_ms / 1000, economy::to_string(e.type),
                    e.object, e.source, e.clan, e.item, e.code, e.scheme.c_str());
            switch (e.type) {
            case T::UnitProduced:
                spawn_unit(e.object, e.scheme);
                if (e.clan == hero_clan) play_voice("VOICE_UNIT_READY");
                break;
            case T::BuildingPlaced:
            case T::BuildingUpgraded: spawn_building(e.object); break;
            case T::ResearchDone:
                if (e.clan == hero_clan && eco) {
                    const economy::Research* r = eco->research(e.clan);
                    const mission::ResearchItem* it = r ? r->item(e.item) : nullptr;
                    pager = ui::PagerMessage{ui_text(1541), ui_text(2010) + " (" + (it ? it->name : std::string()) + ")", {}, sim_ms};
                    play_voice("VOICE_RSRCH_COMPLETE");
                }
                break;
            default: break;
            }
        });

    ViewerOptions vo;
    vo.title = "Parkan — " + opt.mission;
    vo.shot_path = opt.shot_path;
    vo.start_time = opt.start_time;
    vo.pitch = 0.35f;
    vo.orbit_distance = 9.f;

    // The HUD's UI commands (OnUICommandDown 10072120), by the scan code pressed (keys and mouse buttons).
    auto hud_command = [&](std::uint16_t scan) {
        auto is = [&](std::string_view command) {
            const auto k = input::ui_key(command, opt.layout);
            return k && k->key == scan;
        };
        if (is("CMD_JAMES_COCKPIT_OFF")) {
            cockpit_on = !cockpit_on; // CCockpit_AnyPageVisible ? hide all : show all
        } else if (is("CMD_JAMES_SATELLITE_MAP")) {
            map_visible = !map_visible;
        } else if ((is("CMD_INC_MAP_ALPHA") || is("CMD_DEC_MAP_ALPHA")) && map_visible) {
            map_alpha = std::clamp(map_alpha + (is("CMD_INC_MAP_ALPHA") ? 1 : -1) * ui::kMapAlphaStep, ui::kMapAlphaMin, ui::kMapAlphaMax);
            map_alpha_changed_ms = sim_ms;
        } else if (is("CMD_JAMES_ZOOM_MODE")) {
            // Ignored while a zoom transition runs (0.2 < fov < the camera's own).
            const float base = hero.rig ? hero.rig->camera_fov : 1.3f;
            if (zoom_fov < 0 || zoom_fov <= ui::kZoomFov + 1e-4f || zoom_fov >= base - 1e-4f) zoomed = !zoomed;
        } else if (is("CMD_JAMES_SELECT_TARGET")) {
            // CycleRadarTarget: the entry after the current target in the detected list.
            const auto list = detected();
            if (!list.empty()) {
                auto it = std::find(list.begin(), list.end(), hud_target);
                set_hud_target(it == list.end() || it + 1 == list.end() ? list.front() : *(it + 1));
            }
        } else if (is("CMD_JAMES_SELECT_ENEMY")) {
            select_nearest(true);
        } else if (is("CMD_JAMES_SELECT_FRIEND")) {
            select_nearest(false);
        } else if (is("CMD_JAMES_AIM_TARGET")) {
            // LockTarget: the object under the screen centre within the radar range.
            const std::int32_t t = combat.sight_target(hero_slot, kRadarRange);
            if (t >= 0) set_hud_target(t);
        }
    };

    vo.escape_quits = false;
    vo.quit_requested = [&] { return quit || (end_ctl.state() == ui::GameState::Leaving && leaving_frames > 0); };
    bool window_closed = false;
    vo.on_event = [&](const SDL_Event& ev) {
        if (ev.type == SDL_EVENT_QUIT) window_closed = true;
        if (briefing) {
            // DispatchUIKeyMessage ignores everything in view mode 5; OnGameKeyDown: Esc skips the briefing.
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_ESCAPE) briefing_skip = true;
            return;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            if (end_ctl.message_visible()) {
                // OnGameKeyDown / OnGameCharKey: Esc leaves; after a failure R restarts and L loads. Read
                // by physical key, so a Russian layout's К / Д work as in the original.
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) end_ctl.on_escape();
                else if (ev.key.scancode == SDL_SCANCODE_R) end_ctl.on_char('r');
                else if (ev.key.scancode == SDL_SCANCODE_L) end_ctl.on_char('l');
                return;
            }
            // Esc: the HQ's own handling (OnGameKeyDown / ROLLBACK_STATE) in the HQ states; on foot the in-game
            // menu is not ported, so Esc leaves.
            if (ev.key.scancode == SDL_SCANCODE_ESCAPE && !(hq && hq->escape())) quit = true;
        }
        // The HQ states (3–5, notes/hq-ui.md §2.2, §3.2, §10): the pointer is a cursor on the 640×480 virtual
        // screen, the keys drive the HQ camera and the hardcoded letters; the hero takes no input
        // (ViewTrans_*: FUN_10075440(hero, 0), stdClearKeyboard). Releases still reach the hero's tables.
        if (hq && !hq->hero_controlled()) {
            int ww = 640, wh = 480;
            SDL_GetWindowSize(window, &ww, &wh);
            const auto virt = [&](float x, float y) {
                return std::pair{std::round(x * 640.f / float(std::max(ww, 1))), std::round(y * 480.f / float(std::max(wh, 1)))};
            };
            if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat) {
                const bool down = ev.type == SDL_EVENT_KEY_DOWN;
                const auto scan = input::scan_from_hid(int(ev.key.scancode));
                if (scan && !down) events.push_back({false, scan});
                auto is = [&](std::string_view command) {
                    const auto k = input::ui_key(command, opt.layout);
                    return scan && k && k->key == scan;
                };
                static constexpr std::pair<const char*, ui::HqCamera::Key> kMoves[6] = {
                    {"CMD_JAMES_HQ_MOVE_LEFT", ui::HqCamera::Left},   {"CMD_JAMES_HQ_MOVE_RIGHT", ui::HqCamera::Right},
                    {"CMD_JAMES_HQ_MOVE_FORWARD", ui::HqCamera::Forward}, {"CMD_JAMES_HQ_MOVE_BACKWARD", ui::HqCamera::Back},
                    {"CMD_JAMES_HQ_MOVE_UP", ui::HqCamera::Up},       {"CMD_JAMES_HQ_MOVE_DOWN", ui::HqCamera::Down}};
                bool camera_key = false;
                for (const auto& [command, k] : kMoves)
                    if (is(command)) hq->camera.key(k, down, sim_ms / 1000), camera_key = true;
                if (!down) return;
                if (is("CMD_JAMES_BASE_ROTLEFT")) hq->rotate_ghost(1); // [inferred] left = counterclockwise
                else if (is("CMD_JAMES_BASE_ROTRIGHT")) hq->rotate_ghost(-1);
                else if (is("CMD_JAMES_ZOOM_MODE") && hq->view_mode() == 2) hq->camera.toggle_zoom();
                else if (is("CMD_JAMES_SATELLITE_MAP")) hq->map_open = !hq->map_open;
                else if (is("CMD_JAMES_MISSION_OBJ")) objectives_visible = !objectives_visible, objectives_auto_hide = false;
                // The letters W C B R P (listener slot 8). Port: skipped when the layout binds the key to an HQ
                // camera move (the modern layout's W).
                else if (!camera_key) {
                    const SDL_Scancode sc = ev.key.scancode;
                    const char letter = sc == SDL_SCANCODE_W ? 'W' : sc == SDL_SCANCODE_C ? 'C' : sc == SDL_SCANCODE_B ? 'B'
                                        : sc == SDL_SCANCODE_R ? 'R' : sc == SDL_SCANCODE_P ? 'P' : 0;
                    if (letter) hq->hotkey(letter);
                }
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const auto [vx, vy] = virt(ev.button.x, ev.button.y);
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    events.push_back({false, ev.button.button == SDL_BUTTON_LEFT ? input::kScanMouseLeft
                                             : ev.button.button == SDL_BUTTON_RIGHT ? input::kScanMouseRight : input::kScanMouseMiddle});
                const ui::HqPick pk = hq_pick(vx, vy);
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) hq->lbutton_down(vx, vy, pk);
                    else hq->lbutton_up(vx, vy, pk);
                } else if (ev.button.button == SDL_BUTTON_RIGHT && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    hq->rbutton_down();
                }
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                const auto [vx, vy] = virt(ev.motion.x, ev.motion.y);
                hq->mouse_move(vx, vy);
            }
            return;
        }
        if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat) {
            if (const auto scan = input::scan_from_hid(int(ev.key.scancode)))
                events.push_back({ev.type == SDL_EVENT_KEY_DOWN, scan});
            const auto outer_key = input::ui_key("CMD_JAMES_OUTER_CAMERA", opt.layout);
            if (ev.type == SDL_EVENT_KEY_DOWN && outer_key && input::scan_from_hid(int(ev.key.scancode)) == outer_key->key) {
                // JAMES_OUTER_CAMERA (C): AdvanceOuterMode; preset 0 blends back into the TURRET view.
                outer_preset = (outer_preset + 1) % 5;
                if (outer_preset != 0) camera_mode = 1;
                outer_blend_start = sim_ms;
            }
            const auto obj_key = input::ui_key("CMD_JAMES_MISSION_OBJ", opt.layout);
            if (ev.type == SDL_EVENT_KEY_DOWN && obj_key && input::scan_from_hid(int(ev.key.scancode)) == obj_key->key) {
                objectives_visible = !objectives_visible; // JAMES_MISSION_OBJ toggles; no auto-hide then
                objectives_auto_hide = false;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F10) // port debug orbit
                camera_mode = camera_mode < 0 ? 0 : -1;
            if (ev.type == SDL_EVENT_KEY_DOWN)
                if (const auto scan = input::scan_from_hid(int(ev.key.scancode))) hud_command(scan);
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            const std::uint16_t b = ev.button.button == SDL_BUTTON_LEFT    ? input::kScanMouseLeft
                                    : ev.button.button == SDL_BUTTON_RIGHT ? input::kScanMouseRight
                                                                           : input::kScanMouseMiddle;
            events.push_back({ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, b});
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) hud_command(b);
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            mouse_dx += ev.motion.xrel, mouse_dy += ev.motion.yrel;
        }
    };

    // The briefing's per-frame part of MainLoop, run with the simulation clock (game time 0 = the first
    // CBriefing_Draw).
    auto briefing_tick = [&] {
        if (briefing && (briefing_skip || (briefing_started && briefing->done()))) {
            // MainLoop, the frame after the path ended or Esc: ~CBriefing stops every 2D wave (the voice),
            // the AI unfreezes, the view returns to the hero; an only_briefing mission is over (won), any
            // other plays a random track. The objectives screen's 7 s start now.
            briefing.reset();
            if (sound) sound->stop_all();
            objectives_shown_ms = sim_ms;
            if (briefing_setup.only_briefing) {
                end_ctl.briefing_only_finished();
            } else if (sound) {
                sound->stop_music();
                sound->play_random_music(false);
            }
        }
        if (briefing) {
            // CMission_UpdateViewModeCamera 100a5560 (view mode 5): the path by the game clock; playback
            // starts with the first CBriefing_Draw (here: the first frame or step).
            const std::uint32_t now = std::uint32_t(sim_ms);
            if (!briefing_started) briefing->start(now), briefing_started = true;
            else briefing->update(now);
            for (const std::string& key : briefing->take_sounds())
                if (auto v = voice_entries.find(key); sound && v != voice_entries.end()) sound->play({"voices.lib", v->second});
        }
    };
    // One simulation frame at most 33 ms long: input → IControl requests → motion → ground.
    auto step = [&](double to_ms) {
        const float dt = float(to_ms - sim_ms);
        man.process(events, mouse_dx, mouse_dy, dt, commands);
        events.clear();
        mouse_dx = mouse_dy = 0;
        using namespace input;
        // In the HQ states the hero is not driven (ViewTrans_*: FUN_10075440(hero, 0)).
        const bool hero_input = !hq || hq->hero_controlled();
        // ExecuteBinding (vehicle class 0): throttle.y from FORWARD/BACK/WALK_F/WALK_B, the angular
        // command from ROTATE_*; strafing (LEFT/RIGHT heading) is not modelled yet.
        float forward = commands.get(0, 0, kMcmdForward) + commands.get(0, 0, kMcmdBack) +
                        commands.get(0, 0, kMcmdWalkF) + commands.get(0, 0, kMcmdWalkB);
        if (opt.demo_walk) forward = 1;
        forward = hero_input ? std::clamp(forward, -1.f, 1.f) : 0.f;
        // Strafing (ExecuteBinding LEFT/RIGHT): walk (throttle.y = ±1) with an attitude offset of ±90°,
        // ±45° while also walking forward or back; the motion graph sees a plain walk request.
        // The LEFT/RIGHT value is the strafe throttle (1 in the original tables; the modern layout walks
        // at 0.5 and sprints at 1 with Shift).
        const float left_v = !hero_input ? 0.f : opt.demo_strafe ? 1.f : commands.get(0, 0, kMcmdLeft),
                    right_v = hero_input ? commands.get(0, 0, kMcmdRight) : 0.f;
        const bool left = left_v > 0, right = right_v > 0;
        float heading = 0;
        if (left != right) {
            const float k = forward != 0 ? 0.5f * (forward < 0 ? -1.f : 1.f) : 1.f;
            heading = (left ? 1.f : -1.f) * k * 1.5707963f;
            if (forward == 0) forward = std::max(left_v, right_v);
        }
        motion.set_attitude_offset({0, 0, heading});
        motion.set_throttle({0, forward, hero_input ? commands.get(0, 0, kMcmdUp) + commands.get(0, 0, kMcmdDown) : 0.f});
        // HERO.TBL mouse X → ANGLE_Z: the chassis attitude target (IControl +0x48) gets the mouse delta
        // on top of the remaining turn the mover has not consumed yet (GetProperty 0x30).
        const float angle_z = commands.get(0, 0, kMcmdAngleZ);
        float dz_turn = angle_z - last_angle_z;
        dz_turn -= std::round(dz_turn);
        last_angle_z = angle_z;
        if (!hero_input) {
        } else if (dz_turn != 0) {
            Vec3 t = motion.attitude_target();
            t.z = t.z + dz_turn - std::floor(t.z + dz_turn);
            motion.set_attitude_target(t);
        } else if (commands.get(0, 0, kMcmdRotateZ) != 0) {
            motion.set_angular_command({0, 0, commands.get(0, 0, kMcmdRotateZ)});
        }
        // The multigun bindings (ExecuteBinding SELECT / STATE): 1–8 toggle a gun, 0 selects all, LMB
        // sets CIS_CONTINUEFIGHT on the selected guns and its release CIS_SWITCHOFF.
        for (const auto& a : commands.take_actions()) {
            if (a.cls != 2 || !hero_input) continue;
            if (a.cmd == kMcmdSelect) hero_combat.select(a.index);
            else if (a.cmd == kMcmdState && a.index == -1) hero_combat.set_state_selected(std::uint32_t(a.state));
            else if (a.cmd == kMcmdState && a.index >= 1) hero_combat.set_gun_state(std::size_t(a.index - 1), std::uint32_t(a.state));
        }
        if (opt.demo_fire && hero_combat.alive()) hero_combat.set_trigger(true);
        sim_ms = to_ms;
        briefing_tick();
        for (std::size_t i = 0; i < live.size(); ++i) {
            const combat::Combatant& c = combat.unit(live_ci[i]);
            if (c.removed) continue;
            // SetAllUnitsAIFrozen(1) during the briefing: every unit but the hero (property 0x20a).
            if (brains[i] && c.alive() && !(briefing && i != hero_slot)) brains[i]->update(float(sim_ms)); // brain tick + motion update (pilot inside)
            else live[i]->motion->update(float(sim_ms));
        }
        // The fire control's request: the target goes to the guns (SetTarget), the turret slews toward its
        // aim (state 0x400 AI point; port: the target's root part), each requested weapon fires a shot
        // (state 0x200). The aim errors feed the next pass.
        auto apply_fire = [&](std::size_t ci, LiveUnit& u, ai::FireControl& fc, const ai::FireRequest& fr, const std::vector<std::size_t>& guns) {
            combat::Combatant& c = combat.unit(ci);
            const auto t = unit_of.find(fr.target);
            c.target = t != unit_of.end() ? std::int32_t(t->second) : -1;
            if (c.target < 0) return;
            const Vec3 aim = combat.unit(std::size_t(c.target)).aim_centre();
            float group_err = 3.1415927f;
            for (std::size_t k = 0; k < guns.size() && k < fc.weapons.size(); ++k) {
                const std::size_t g = guns[k];
                fc.weapons[k].aim_error = fc.weapons[k].group < 0 ? 0.f : combat.ai_aim_error(ci, g, aim);
                if (fc.weapons[k].group >= 0) group_err = std::min(group_err, fc.weapons[k].aim_error);
                if (fr.trigger & (1u << k)) c.set_gun_state(g, combat::kFireSingle);
            }
            if (!fc.turret_error.empty()) fc.turret_error[0] = group_err;
            if (fr.track && u.rig) combat.ai_aim(ci, aim, dt * 0.001f);
        };
        for (std::size_t i = 0; i < live.size(); ++i) {
            const combat::Combatant& c = combat.unit(live_ci[i]);
            if (!brains[i] || !c.alive() || c.removed) continue;
            apply_fire(live_ci[i], *live[i], brains[i]->fire_control(), brains[i]->fire(), weapon_guns[i]);
        }
        // The buildings' brains (frozen with the units during the briefing); a capture changes their clan.
        for (std::size_t k = 0; k < buildings.size(); ++k) {
            const std::size_t ci = bld_ci[k];
            const combat::Combatant& c = combat.unit(ci);
            if (!bld_brains[k] || !c.alive() || c.removed || briefing) continue;
            bld_brains[k]->brain().self.clan = clan_of(ci);
            bld_brains[k]->update(float(sim_ms));
            apply_fire(ci, buildings[k]->body(), bld_brains[k]->fire_control(), bld_brains[k]->fire(), bld_weapon_guns[k]);
        }
        live_ptrs.clear();
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (combat.unit(live_ci[i]).removed) continue;
            const Vec3 push = live[i]->settle(float(sim_ms), ground, statics, floor_probe);
            if (i == hero_slot && dot(push, push) > 0) ++pushed_steps;
            if (combat.unit(live_ci[i]).alive()) live_ptrs.push_back(live[i].get()); // the dead have left collision
        }
        collide_units(live_ptrs, float(sim_ms));
        // Turrets: the hero's from HERO.TBL (turret channels), the others hold their aim_x/aim_y.
        hero.aim_x = commands.get(1, 1, input::kMcmdAngleX), hero.aim_y = commands.get(1, 1, input::kMcmdAngleY);
        for (std::size_t i = 0; i < live.size(); ++i)
            if (!combat.unit(live_ci[i]).removed) live[i]->pose_parts();
        // Buildings (CBuilding::SendMsg message 1, 100335d0): doors and the capture door, then the pose.
        // Contacts: the live units whose sphere overlaps the building's (port reduction, building.hpp).
        for (std::size_t k = 0; k < buildings.size(); ++k) {
            Building& b = *buildings[k];
            if (b.removed) continue;
            const std::size_t ci = bld_ci[k];
            if (combat.unit(ci).alive()) {
                std::vector<Building::Contact> contacts;
                const Vec3 bc = b.body().sphere_world();
                for (std::size_t i = 0; i < live.size(); ++i) {
                    if (combat.unit(live_ci[i]).removed) continue;
                    const Vec3 c = live[i]->sphere_world(), d = c - bc;
                    const float r = live[i]->sphere_radius, reach = r + b.body().sphere_radius;
                    if (dot(d, d) <= reach * reach) contacts.push_back({std::int32_t(i), c, r, combat.unit(live_ci[i]).alive()});
                }
                const int before = b.capture_door() ? b.capture_door()->state : -1;
                if (auto cap = b.update(float(sim_ms), contacts)) on_building_entered(k, live_ci[std::size_t(*cap)]);
                if (b.capture_door() && b.capture_door()->state != before)
                    SDL_Log("play: %.2f s building %08x capture door state %d -> %d (capturer %d, %zu contacts)", sim_ms / 1000,
                            placement_of(ci).identity, before, b.capture_door()->state, b.capturer(), contacts.size());
            }
            b.apply_frames();
            b.body().pose_parts();
        }
        if (opt.demo_aim) {
            // Port debug aid: the nearest living hostile unit becomes the target; the body turns to it and every
            // bullet heads for its centre.
            std::int32_t best = -1;
            float best_d = FLT_MAX;
            for (std::size_t i = 0; i < combat.size(); ++i) {
                if (i == hero_slot || combat.unit(i).removed || !combat.unit(i).alive()) continue;
                if (opt.aim_target ? placement_of(i).identity != *opt.aim_target
                                   : combat.unit(i).building() || !perception.hostile(hero_clan, clan_of(i)))
                    continue;
                const Vec3 d = combat.unit(i).centre() - hero.position();
                if (dot(d, d) < best_d) best_d = dot(d, d), best = std::int32_t(i);
            }
            hero_combat.target = best;
            hero_combat.aim_point.reset();
            if (best >= 0) {
                const Vec3 to = combat.unit(std::size_t(best)).aim_centre() - hero.position();
                hero_combat.aim_point = combat.unit(std::size_t(best)).aim_centre();
                const Mat4& m = hero.frame.matrix;
                const float err = std::atan2(to.y, to.x) - std::atan2(m.at(1, 1), m.at(0, 1));
                const float turns = err / 6.2831853f - std::round(err / 6.2831853f);
                motion.set_attitude_target({0.5f, 0.5f, 0.5f - turns});
                if (hero.rig) { // and the turret (with the camera) looks at it
                    const ai::AimAngles a = ai::aim_angles(hero.frame.matrix, hero.position() + Vec3{0, 0, 3}, *hero_combat.aim_point);
                    std::tie(hero.aim_x, hero.aim_y) = hero.rig->inputs_for_angles(a.yaw, a.pitch);
                    hero.pose_parts();
                }
            }
        } else {
            // The guns' target is the selected HUD target only (CTargeting 10091cd0 pushes it to the selected
            // weapons on LockTarget, the select commands and the target's death); nothing selected → no
            // target, so guided guns (plasma, missiles) hold fire (CGunDevice::Update 10029f4b).
            if (hud_target >= 0 && (combat.unit(std::size_t(hud_target)).removed || !combat.unit(std::size_t(hud_target)).alive()))
                hud_target = -1;
            hero_combat.target = hud_target;
        }
        combat.update(float(sim_ms));
        for (const auto& e : combat.take_events()) {
            using T = combat::CombatEvent::Type;
            const auto name = [&](std::int32_t u) { return u >= 0 ? placement_of(std::size_t(u)).resource.c_str() : "-"; };
            switch (e.type) {
            case T::Effect:
                start_effect(e, sim_ms / 1000);
                break;
            case T::BulletEnded: ended_bullets[e.bullet] = int(e.amount); break;
            case T::Hit:
                if (e.unit >= 0)
                    SDL_Log("play: %.2f s bullet of %d hit %s at (%.1f, %.1f, %.1f)", sim_ms / 1000, e.other, e.unit >= 0 ? name(e.unit) : "the world",
                            e.pos.x, e.pos.y, e.pos.z);
                break;
            case T::Damaged: {
                const auto& v = combat.unit(std::size_t(e.unit));
                SDL_Log("play: %.2f s hit %zu %s by %d: -%.0f HP (%.0f / %.0f left)", sim_ms / 1000, std::size_t(e.unit), name(e.unit),
                        e.other, e.amount, v.damage.total, v.damage.initial);
                // MBehaviour::OnDamageMsg skips buildings (not decoded further; no brain here).
                if (const std::int32_t li = ci_live[std::size_t(e.unit)]; li >= 0 && brains[std::size_t(li)] && e.other >= 0 && v.alive())
                    brains[std::size_t(li)]->on_damage(combat.unit(std::size_t(e.other)).logic_id);
                break;
            }
            case T::Killed: {
                ++kills;
                const auto& v = combat.unit(std::size_t(e.unit));
                SDL_Log("play: %.2f s %s %08x (%s) killed", sim_ms / 1000,
                        std::size_t(e.unit) == hero_slot ? "HERO" : v.building() ? "building" : "unit", v.logic_id, name(e.unit));
                // GetObjectClan returns 0xfffe from now on; the AI stops (MBehaviour OnTakt "No clan").
                if (runtime) runtime->world().set_clan(v.logic_id, combat::kKilledClan);
                if (std::size_t(e.unit) == hero_slot) {
                    // The hero (class 3) stays in the world as it is: no removal, no debris (AddPartHP
                    // 10010f30, DetachPartDebris 100102a0). iron3d's unit registry (1007daa0, after
                    // stdCalculateGame in the same MainLoop frame) sees its clan 0xfffe and drops it (1007d8a0 →
                    // OnUnitRemoved 100755f0): the own unit's VOICE_UNIT_LOST, then CMission_EnterSpectatorAtUnit
                    // (100a4df0) and CGame_SetMissionFailed (VOICE_MISSION_FAIL): the world stops from this frame.
                    play_voice("VOICE_UNIT_LOST"); // QueueUnitVoice: queued before the failure's (port: not serialised)
                    play_end_voice(end_ctl.hero_killed());
                    // The spectator camera gets the hero's world matrix at z = ground height + 16; in single
                    // player its free flight is off (HqCam_Update skips HqCam_Integrate in view mode 4 unless
                    // mission+0xaf5), so it stays there. A World3D camera looks along its local +X (left +Y, up
                    // +Z; shell.md): the hero's right. Projection as HqCam_Init's (inferred for this camera).
                    const Vec3 p = hero.position();
                    const auto g = ground.probe(p + Vec3{0, 0, 50}, 200.f);
                    const Mat4& hm = hero.frame.matrix;
                    ViewerOptions::Camera c;
                    c.eye = {p.x, p.y, (g ? g->z : p.z) + ui::kDeathCameraHeight};
                    c.forward = normalize(Vec3{hm.at(0, 0), hm.at(1, 0), hm.at(2, 0)});
                    c.up = normalize(Vec3{hm.at(0, 2), hm.at(1, 2), hm.at(2, 2)});
                    c.fov_x = 1.04f, c.znear = 3.f, c.zfar = 700.f;
                    spectator = c;
                }
                if (const std::int32_t li = ci_live[std::size_t(e.unit)]; li >= 0) {
                    LiveUnit& dead = *live[std::size_t(li)];
                    dead.motion->pilot = nullptr;
                    dead.motion->set_throttle({});
                    dead.motion->set_angular_command({});
                    if (runtime && std::size_t(li) < adapters.size()) adapters[std::size_t(li)]->stop();
                }
                break;
            }
            case T::Removed:
                // KillGameObject: the object leaves the game; scripts see ObjectExists 0 / GetObjectClan −1.
                SDL_Log("play: %.2f s %s %08x removed", sim_ms / 1000, combat.unit(std::size_t(e.unit)).building() ? "building" : "unit",
                        combat.unit(std::size_t(e.unit)).logic_id);
                if (runtime) runtime->object_killed(combat.unit(std::size_t(e.unit)).logic_id);
                if (Building* b = building_of(std::size_t(e.unit))) {
                    b->removed = true;
                    statics.remove(std::uint32_t(ci_bld[std::size_t(e.unit)] + 1)); // its walls go with it
                }
                break;
            default:
                break;
            }
        }
        update_effects(sim_ms / 1000);
        if (runtime && !briefing) runtime->update(std::uint32_t(sim_ms)); // MainLoop skips the scripts in view mode 5
        if (hq_frame && !briefing) hq_frame(); // CHud_UpdateHq and the HQ's MainLoop parts
        // Footsteps: a touch-down starts its step effect (step_h* by surface; the default surface's
        // entry until surfaces are decoded); the effect's sound commands play at the foot.
        for (auto& u : live)
            for (const auto& td : u->touchdowns)
                if (auto fx = u->touchdown_effect(td.event_list)) {
                    ++footsteps;
                    if (sound)
                        for (const auto& s : load_fx_sounds(lib, fx->second)) sound->play(audio::fx_sound(s), td.position);
                }
        if (sound && !briefing && sound->music_finished()) sound->play_random_music(false);
    };

    vo.animate = [&](double seconds, std::vector<Mat4>& transforms) {
        const double now = seconds * 1000.0;
        if (end_ctl.simulating()) {
            // One stdCalculateGame a frame: a mission end inside it stops the rest of the frame's steps.
            while (end_ctl.simulating() && sim_ms + 1e-6 < now) step(std::min(now, sim_ms + 33.0));
        } else {
            // MainLoop: the world stops (stdCalculateGame) while the player's script, sound and music go on;
            // a later opposite result flips the message.
            if (runtime) runtime->update(std::uint32_t(now));
            if (sound && sound->music_finished()) sound->play_random_music(false);
        }
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (!unit_drawn[i]) continue; // created this frame: the grow hook appends it first
            // From its own TURRET camera the unit is not drawn (inferred from CCameraDevice::CreateCamera
            // handing the camera to IAniMesh slot 0x84). The camera reads the posed model, not these.
            // A removed unit is no longer drawn.
            live[i]->pose(transforms, (i == hero_slot && camera_mode == 0 && !spectator) || combat.unit(live_ci[i]).removed);
        }
        for (std::size_t k = 0; k < buildings.size(); ++k) // doors; destroyed parts collapsed
            if (building_drawn[k]) buildings[k]->body().pose(transforms, buildings[k]->removed);
        for (auto& pool : bullet_pools) {
            std::size_t k = 0;
            for (const auto& b : combat.bullets()) {
                if (b.proto != pool.proto || k >= pool.firsts.size()) continue;
                const Mat4 m = combat::CombatWorld::bullet_matrix(b.pos, b.dir);
                for (std::size_t n = 0; n < pool.nodes.size(); ++n) transforms[pool.firsts[k] + n] = m * pool.nodes[n];
                ++k;
            }
            for (; k < pool.firsts.size(); ++k)
                for (std::size_t n = 0; n < pool.nodes.size(); ++n) transforms[pool.firsts[k] + n] = Mat4{};
        }
    };
    // The new objects' meshes join the scene; a new building's static parts become walls (tag = index + 1).
    vo.grow = [&](RenderMesh& mesh) {
        if (pending_units.empty() && pending_buildings.empty()) return false;
        for (std::size_t i : pending_units) {
            live[i]->first_transform = append_render_mesh(mesh, live[i]->model.mesh, Mat4::identity());
            unit_drawn[i] = true;
        }
        for (std::size_t k : pending_buildings) {
            LiveUnit& body = buildings[k]->body();
            body.first_transform = append_render_mesh(mesh, body.model.mesh, Mat4::identity());
            body.pose(mesh.transforms, false);
            for (std::uint32_t t = 0; t < body.model.mesh.transforms.size(); ++t)
                if (!buildings[k]->moving_transform(t)) statics.add(mesh, body.first_transform + t, body.first_transform + t + 1, std::uint32_t(k + 1));
            building_drawn[k] = true;
        }
        if (!pending_buildings.empty()) statics.finish();
        pending_units.clear(), pending_buildings.clear();
        return true;
    };
    vo.effect_textures = &fx_ctx.materials.textures;
    vo.effects = [&](Vec3 eye, fx::FxFrame& f) {
        for (auto& e : effects) e.inst.render(f, eye);
    };
    vo.follow = [&](Vec3& centre) { centre = hero.position() + Vec3{0, 0, 2.f}; };
    vo.relative_mouse = true;
    // CCameraDevice::Update: eye = CameraCenter, X = TargetDirect (the barrel), up = CameraCenter's
    // vector; free look (LShift + mouse) rotates locally by Rz((0.5 − ax)·2π) · Ry((0.5 − ay)·π).
    vo.camera = [&](ViewerOptions::Camera& cam) {
        hud_cam.reset();
        if (briefing && !briefing_started) briefing_tick();
        if (briefing) {
            // CreateOuterCamera(…, near 3.0, far 700.0, fov 1.04); CameraCtl_ApplyZoom: fov = lerp(1.04, 0.2, zoom).
            // The original renders into the viewport (0, 75, W, H − 75); with a horizontal fov the full-screen
            // image under the black bars is the same picture. Night vision (flag 0x20) is unused by the data
            // and not drawn.
            const auto& bc = briefing->camera();
            const float z = std::clamp(briefing->zoom(), 0.f, 1.f);
            cam.eye = bc.eye, cam.forward = bc.forward, cam.up = bc.up;
            cam.fov_x = (1 - z) * 1.04f + z * 0.2f;
            cam.znear = 3.f, cam.zfar = 700.f;
            if (sound) sound->set_listener(cam.eye, cam.forward, cam.up);
            return true;
        }
        if (spectator) {
            cam = *spectator;
            if (sound) sound->set_listener(cam.eye, cam.forward, cam.up);
            return true;
        }
        // The HQ states show the system pointer (cursors .ani not decoded) instead of the captured mouse.
        if (const bool rel = !hq || hq->hero_controlled(); opt.shot_path.empty() && rel != relative_mouse_on)
            SDL_SetWindowRelativeMouseMode(window, rel), relative_mouse_on = rel;
        if (hq && hq->view_mode() == 2) {
            // The HQ camera (view mode 2, CFreeCamera mission+0x100, updated in the simulation step); the
            // World3D camera far 700, near 3 (HqCam_Init 10036ed0).
            cam.eye = hq->camera.pos, cam.forward = hq->camera.forward(), cam.up = hq->camera.up();
            cam.fov_x = hq->camera.fov(), cam.znear = 3.f, cam.zfar = 700.f;
            hud_cam = cam;
            if (sound) sound->set_listener(cam.eye, cam.forward, cam.up);
            return true;
        }
        const auto& rig = hero.rig;
        const auto& turret_points = hero.turret_points;
        if (camera_mode < 0 || !rig || rig->camera_eye_point < 0 || std::size_t(rig->camera_eye_point) >= turret_points.size() ||
            std::size_t(rig->camera_axis_point) >= turret_points.size())
            return false;
        const auto& nodes = hero.model.parts[hero.turret_part].model.node_world;
        const MotionFrame& last_frame = hero.frame;
        const ContactPoint &pe = turret_points[std::size_t(rig->camera_eye_point)],
                           &pa = turret_points[std::size_t(rig->camera_axis_point)];
        if (pe.part >= nodes.size() || pa.part >= nodes.size()) return false;
        // The turret part's nodes already carry the steady mount (hero.steady_mounts).
        const Mat4& base = last_frame.matrix;
        const Mat4 me = base * nodes[pe.part], ma = base * nodes[pa.part];
        const Vec3 eye = me.transform_point(pe.pos);
        Vec3 fwd = normalize(ma.transform_point(pa.vec) - ma.transform_point({}));
        Vec3 up = normalize(me.transform_point(pe.vec) - me.transform_point({}));
        const float ax = commands.get(4, 1, input::kMcmdAngleX), ay = commands.get(4, 1, input::kMcmdAngleY);
        const Vec3 leftv = normalize(cross(up, fwd));
        const auto rot = [](Vec3 v, Vec3 axis, float a) {
            return v * std::cos(a) + cross(axis, v) * std::sin(a) + axis * (dot(axis, v) * (1 - std::cos(a)));
        };
        fwd = rot(fwd, up, (0.5f - ax) * 6.2831853f);
        fwd = rot(fwd, rot(leftv, up, (0.5f - ax) * 6.2831853f), (0.5f - ay) * 3.1415927f);
        cam.eye = eye, cam.forward = fwd, cam.up = up;
        if (sound) sound->set_listener(eye, fwd, up);
        last_cam_forward = fwd;
        // JAMES_ZOOM_MODE (CUnit_Update 10075ad0): the FOV steps 0.1 per frame toward 0.2 or back.
        if (zoom_fov < 0) zoom_fov = rig->camera_fov;
        zoom_fov = zoomed ? std::max(ui::kZoomFov, zoom_fov - ui::kZoomStep) : std::min(rig->camera_fov, zoom_fov + ui::kZoomStep);
        cam.fov_x = zoom_fov, cam.znear = rig->camera_near, cam.zfar = rig->camera_far;
        hud_cam = cam;
        if (camera_mode == 1) {
            // CMissionCameras::UpdateOuter: parallel to the turret view, behind and below it by the unit's
            // radius × the preset factors; the presets blend over 0.3 s (0.5 s leaving).
            const OuterParams& target = kOuter[outer_preset];
            if (outer_blend_start >= 0) {
                const double dur = outer_preset != 0 ? 0.5 : 0.3, s = (sim_ms - outer_blend_start) / 1000.0;
                if (s < 0.8 * dur) {
                    const float f = float(std::min(1.0, 1.25 * s));
                    outer_cur = {outer_cur.yaw + (target.yaw - outer_cur.yaw) * f, outer_cur.height + (target.height - outer_cur.height) * f,
                                 outer_cur.dist + (target.dist - outer_cur.dist) * f};
                } else {
                    outer_cur = target;
                    outer_blend_start = -1;
                    if (outer_preset == 0) camera_mode = 0;
                }
            }
            const float h = std::atan2(fwd.y, fwd.x) + outer_cur.yaw, R = hero.sphere_radius;
            cam.eye = cam.eye - Vec3{std::cos(h) * R * outer_cur.dist, std::sin(h) * R * outer_cur.dist, R * outer_cur.height};
            // ClipOuterToWorld, reduced to the ground: keep the camera 0.75 above the landscape.
            if (auto g = ground.probe(cam.eye + Vec3{0, 0, 50}, 100.f); g && cam.eye.z < g->z + 0.75f) cam.eye.z = g->z + 0.75f;
            cam.znear = 0.5f, cam.zfar = 175.f, cam.fov_x = 1.3f;
            if (sound) sound->set_listener(cam.eye, fwd, up);
            hud_cam = cam;
        }
        return true;
    };

    // The cockpit's state for this frame (notes/hud.md), from the hero's combatant, the live units and the
    // rendered camera. `aspect` = drawable width / height (projections).
    ui::HudStrings hud_strings;
    hud_strings.inf = ui_text(5094), hud_strings.out_of_zone = ui_text(6250), hud_strings.metres = ui_text(6178);
    auto unit_name = [&](std::size_t i) {
        const mission::TmaObject& o = placement_of(i);
        if (!o.instance_name.empty()) return utf8_to_cp1251(o.instance_name);
        std::string n = o.resource;
        if (auto cut = n.find_last_of("\\/"); cut != std::string::npos) n = n.substr(cut + 1);
        if (auto dot_at = n.find('.'); dot_at != std::string::npos) n.resize(dot_at);
        return n;
    };
    // GetClanRelationColour: own clan, nature clans (TMA mode 0; inferred = record kind 0), else the
    // other clan's relation towards the hero's.
    auto unit_colour = [&](std::size_t i) {
        const std::uint32_t clan = clan_of(i); // the owner now (captures recolour buildings)
        const int kind = clan < tma.clans.size() && tma.clans[clan].mode == 0 ? 0 : 1;
        int relation = perception.hostile(hero_clan, clan) ? 0 : 1;
        if (runtime && clan < runtime->world().clans.size() && hero_clan < runtime->world().clans[clan].relations.size())
            relation = int(runtime->world().clans[clan].relations[hero_clan]);
        return ui::relation_colour(clan == hero_clan, kind, relation);
    };
    auto build_hud = [&](float aspect) {
        ui::HudState s;
        s.cockpit = cockpit_on;
        const Vec3 hero_pos = hero.position();
        auto project = [&](Vec3 p) -> std::optional<ui::Vec2f> {
            if (!hud_cam) return std::nullopt;
            const float eye[3] = {hud_cam->eye.x, hud_cam->eye.y, hud_cam->eye.z};
            const float fwd[3] = {hud_cam->forward.x, hud_cam->forward.y, hud_cam->forward.z};
            const float up[3] = {hud_cam->up.x, hud_cam->up.y, hud_cam->up.z}, pt[3] = {p.x, p.y, p.z};
            return ui::project_to_virtual(eye, fwd, up, hud_cam->fov_x, aspect, pt);
        };
        if (hud_cam) {
            s.fwd_x = hud_cam->forward.x, s.fwd_y = hud_cam->forward.y, s.fov = hud_cam->fov_x;
        } else { // debug orbit: the body's heading, no sight
            const Mat4& m = hero.frame.matrix;
            s.fwd_x = m.at(0, 1), s.fwd_y = m.at(1, 1), s.has_camera = false;
        }
        // Radar: the detected units; the ping period range·0.002 s (property 0xa9 stand-in); altitude above
        // the map's lowest point (mission+0xad8 inferred); speed in km/h.
        s.radar_range = kRadarRange;
        const double period_ms = kRadarRange * 0.002 * 1000;
        s.sweep = float(std::fmod(sim_ms, period_ms) / period_ms);
        for (std::int32_t i : detected()) {
            const Vec3 d = combat.unit(std::size_t(i)).unit->position() - hero_pos;
            ui::HudBlip b;
            b.dx = d.x, b.dy = d.y, b.argb = unit_colour(std::size_t(i));
            b.kind = combat.unit(std::size_t(i)).building()                        ? ui::BlipKind::Building
                     : contains_ci(placement_of(std::size_t(i)).resource, "\\ANIMAL\\") ? ui::BlipKind::Flyer
                                                                                        : ui::BlipKind::Ground;
            b.selected = i == hud_target;
            s.blips.push_back(b);
        }
        s.altitude = hero_pos.z - ground.bounds_min().z;
        const Vec3 v = motion.world_velocity();
        s.speed_kmh = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) * 3.6f;
        max_speed_seen = std::max(max_speed_seen, s.speed_kmh);
        s.max_speed_kmh = max_speed_seen;
        s.lamps[3] = 1; // piloted directly (top view state 0)
        s.lamps[5] = 1; // behaviour mode 0 (unit+0x9c; port default)
        // Weapon rows: the hero's guns in HERO.TBL order, named by iron3d 3071..3074 (CHeroUnit_Ctor).
        for (std::size_t g = 0; g < hero_combat.guns.size(); ++g) {
            const combat::GunDevice& gun = hero_combat.guns[g];
            ui::HudWeapon w;
            w.name = g < 4 ? ui_text(3071 + std::uint32_t(g)) : utf8_to_cp1251(gun.spec().bullet.second);
            w.ammo = gun.ammo(), w.state = gun.state();
            w.selected = g < hero_combat.selected.size() && hero_combat.selected[g];
            w.energy = gun.param(1) > 0 ? std::clamp(gun.energy() / gun.param(1), 0.f, 1.f) : 1.f;
            // Lock progress (control slot 0x28, inferred): guided guns only, 1 − timer / lock time.
            if (gun.param(9) > 0)
                w.lock = gun.state() == combat::kGunIdle      ? 1.f
                         : gun.state() == combat::kGunLocking ? std::clamp(1 - gun.lock_timer() / gun.param(9), 0.01f, 1.f)
                                                              : 0.f;
            s.weapons.push_back(w);
        }
        if (hero_combat.target >= 0) s.lock_point = project(combat.unit(std::size_t(hero_combat.target)).aim_centre());
        // Own unit (page 2) and the selected target (page 1, with its on-screen box).
        s.own.name = unit_name(hero_slot);
        s.own.health = hero_combat.damage.hp_ratio() * 100;
        s.own.energy = hero_combat.battery_fraction(); // unit property 0x73: the batteries' charge
        s.own.shields = hero_combat.shield_charges(); // property 0x79 (CShieldDisplay_Draw only with it)
        const std::int32_t shown = hud_target >= 0 ? hud_target : opt.demo_aim ? hero_combat.target : -1;
        if (shown >= 0 && !combat.unit(std::size_t(shown)).removed) {
            const combat::Combatant& t = combat.unit(std::size_t(shown));
            ui::HudUnitPanel p;
            p.name = unit_name(std::size_t(shown));
            p.health = t.damage.hp_ratio() * 100;
            p.energy = t.battery_fraction();
            p.shields = t.shield_charges();
            const Vec3 d = t.unit->position() - hero_pos;
            p.distance = std::sqrt(dot(d, d));
            s.target = p;
            if (auto sp = project(t.aim_centre())) {
                // Port: the box half-size 200·s is 2/3 of the projected bounding radius (the original's
                // projection terms p[4]/p[5] are not decoded).
                const Vec3 e = t.aim_centre() - hud_cam->eye;
                const float dist = std::max(0.1f, std::sqrt(dot(e, e)));
                ui::HudTargetMarker m;
                m.x = sp->x, m.y = sp->y, m.argb = unit_colour(std::size_t(shown));
                m.size = t.radius() / (dist * std::tan(hud_cam->fov_x / 2)) * 320.f / 200.f * (2.f / 3.f);
                m.since_change_s = float((sim_ms - hud_target_since_ms) / 1000);
                s.marker = m;
            }
        }
        // The satellite map: units as dots (x·256/S, S = the map's width).
        s.map_visible = map_visible, s.map_alpha = map_alpha, s.map_alpha_label = sim_ms - map_alpha_changed_ms < 1000;
        const float S = std::max(1.f, ground.bounds_max().x);
        for (std::size_t i = 0; i < combat.size(); ++i) {
            const combat::Combatant& c = combat.unit(i);
            if (c.removed || !c.alive()) continue;
            const Vec3 p = c.unit->position();
            s.map_dots.push_back({p.x * 256 / S, p.y * 256 / S, i == hero_slot ? 0xffffffffu : unit_colour(i)});
        }
        return s;
    };

    // HUD text. Positions are in the 640×480 virtual screen; the font set is chosen by the window's size
    // in points and its glyphs scaled by the pixel density (ui/hud_text.hpp).
    // The HQ (notes/hq-ui.md): the controller over the mission's world model and economy, its host hooks,
    // and what lies under the pointer (HqUi_ClassifyPointer's picks: the world ray against the terrain and the
    // objects' faces [port: the combat world's trace stands in for 10036270, not traced]).
    std::map<std::string, std::string> ui_sounds; // ui/game_resources.cfg `object sounds`
    if (auto p = resolve_path(lib.game_dir(), "UI/game_resources.cfg")) {
        const Bytes b = read_file(*p);
        for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()}))
            if (o.name == "sounds")
                for (const auto& [k, v] : o.entries) ui_sounds[k] = v;
    }
    if (runtime) {
        ui::HqHost host;
        host.text = ui_text;
        host.health = [&](std::uint32_t lid) {
            auto it = unit_of.find(lid);
            return it != unit_of.end() ? combat.unit(it->second).damage.hp_ratio() * 100.f : 100.f;
        };
        host.name = [&](std::uint32_t lid) {
            auto it = unit_of.find(lid);
            return it != unit_of.end() ? unit_name(it->second) : std::string();
        };
        host.voice = [&](const std::string& key, std::uint32_t) { play_voice(key); };
        host.pager = [&](const std::string& h, const std::string& b) { pager = ui::PagerMessage{h, b, {}, sim_ms}; };
        host.sound = [&](const char* key) {
            if (auto v = ui_sounds.find(key); sound && v != ui_sounds.end()) sound->play({"sounds.lib", v->second});
        };
        host.passable = [&](float x, float y) { // FUN_10076bc0's ground cell [inferred: inside the landscape]
            return x > ground.bounds_min().x && y > ground.bounds_min().y && x < ground.bounds_max().x && y < ground.bounds_max().y;
        };
        host.log = [&](const std::string& line) { SDL_Log("play: %.1f s hq: %s", sim_ms / 1000, line.c_str()); };
        hq = std::make_unique<ui::HqController>(runtime->world(), eco, hero_clan, std::move(host));
        // The designer's recent projects start with mission.cfg `object prebuild` (CConstructor_LoadPrebuildProjects:
        // "units\units\prebld\" + modelN, each pushed in front).
        if (eco)
            if (auto p = resolve_path(*dir, "mission.cfg")) {
                const Bytes b = read_file(*p);
                for (const auto& o : game::parse_cfg({reinterpret_cast<const char*>(b.data()), b.size()}))
                    if (o.name == "prebuild")
                        for (const auto& [k, v] : o.entries) {
                            const std::string file = "UNITS\\UNITS\\PREBLD\\" + v;
                            if (const mission::Scheme* sc = eco->schemes().get(file)) hq->add_project({file, *sc});
                            else SDL_Log("play: prebuild %s not readable", file.c_str());
                        }
            }
        hq->project = [&](Vec3 p) -> std::optional<ui::Vec2f> {
            if (!hud_cam) return std::nullopt;
            const float eye[3] = {hud_cam->eye.x, hud_cam->eye.y, hud_cam->eye.z};
            const float fwd[3] = {hud_cam->forward.x, hud_cam->forward.y, hud_cam->forward.z};
            const float up[3] = {hud_cam->up.x, hud_cam->up.y, hud_cam->up.z}, pt[3] = {p.x, p.y, p.z};
            return ui::project_to_virtual(eye, fwd, up, hud_cam->fov_x, hq_aspect, pt);
        };
    }
    hq_pick = [&](float vx, float vy) {
        ui::HqPick pk;
        if (!hq || hq->view_mode() != 2) return pk;
        const Vec3 eye = hq->camera.pos, dir = ui::hq_pointer_ray(hq->camera, vx, vy, hq_aspect);
        if (auto h = ground.trace(eye, eye + dir * 3000.f)) pk.ground = h->point;
        // The object: the first unit or building face along the ray that faces the eye (the combat world's
        // trace; [inferred] back faces are skipped as a rendered pick would cull them).
        Vec3 from = eye;
        for (int k = 0; k < 8; ++k) {
            const auto h = combat.trace(from, eye + dir * 3000.f, std::int32_t(hero_slot));
            if (!h || h->kind != combat::WorldHit::Kind::Unit || h->unit < 0) break;
            if (dot(h->face_normal, dir) < 0) {
                pk.object = placement_of(std::size_t(h->unit)).identity;
                break;
            }
            from = h->point + dir * 0.05f;
        }
        return pk;
    };
    // Port debug aids (src/app/main.cpp usage): --enter puts the hero into an own building as the lift does,
    // --hq-screen opens a screen once the interface is up, --click clicks the virtual screen at a time.
    bool hq_entered = false, hq_screen_done = false;
    std::size_t hq_clicks_done = 0;
    std::function<void()> hq_debug_tick;
    hq_debug_tick = [&] {
        if (!hq) return;
        if (opt.hq_enter && !hq_entered) {
            hq_entered = true;
            hq->hero_enters_building(hero.placement->identity, *opt.hq_enter);
        }
        if (opt.hq_screen >= 0 && !hq_screen_done && hq->interface_up()) {
            hq_screen_done = true;
            hq->expand_now();
            hq->set_screen(opt.hq_screen);
        }
        while (hq_clicks_done < opt.hq_clicks.size() && sim_ms >= opt.hq_clicks[hq_clicks_done].t * 1000) {
            const auto& ck = opt.hq_clicks[hq_clicks_done++];
            hq->mouse_move(ck.x, ck.y);
            const ui::HqPick pk = hq_pick(ck.x, ck.y);
            if (ck.move_only) continue;
            if (ck.right) hq->rbutton_down();
            else hq->lbutton_down(ck.x, ck.y, pk), hq->lbutton_up(ck.x, ck.y, pk);
            SDL_Log("play: %.1f s debug click (%.0f, %.0f)%s: object %08x, state %d, screen %d", sim_ms / 1000, ck.x, ck.y, ck.right ? " right" : "",
                    pk.object, hq->state(), hq->screen());
        }
    };

    hq_frame = [&] {
        if (!hq) return;
        // HqCam_Update 10037bc0 in view mode 2, by the game clock.
        if (hq->view_mode() == 2) {
            const float dtc = hq_cam_ms < 0 ? 0.f : float((sim_ms - hq_cam_ms) / 1000);
            hq_cam_ms = sim_ms;
            hq->camera.update(sim_ms / 1000, dtc, hq->mouse_x, hq->mouse_y, hq->cursor() != ui::kCurBox,
                              [&](float x, float y) { // FUN_100a1470(x, y, 1, …): [inferred] the landscape or an object's top
                                  auto g = ground.probe({x, y, 2000.f}, 4000.f);
                                  float z = g ? g->z : 0.f;
                                  if (auto h = combat.trace_floor({x, y, 2000.f}, {x, y, z})) z = std::max(z, h->point.z);
                                  return z;
                              },
                              runtime ? runtime->world().map_max_x : ground.bounds_max().x);
        } else {
            hq_cam_ms = -1;
        }
        hq_debug_tick();
        hq->update(sim_ms, hq_last_pick);
    };

    vo.overlay_textures = std::move(font_textures);
    vo.overlay = [&](OverlayFrame& f) {
        if (font_sets.empty()) return;
        const ui::VirtualScreen vs{float(f.width), float(f.height), f.scale};
        const int want = ui::nearest_font_set(float(f.width) / f.scale, float(f.height) / f.scale);
        std::size_t k = 0;
        while (k + 1 < font_sets.size() && font_sets[k].size != want) ++k;
        const ui::HudFont game_font{&font_sets[k].game, std::int32_t(2 * k)}, menu_font{&font_sets[k].menu, std::int32_t(2 * k + 1)};
        // DrawGameStateMessage: once the mission is over the HUD goes and only the message is drawn.
        if (!end_ctl.hud_visible()) {
            ui::draw_end_screen(f, vs, game_font, menu_font, end_ctl.screen(), end_str);
            if (end_ctl.state() == ui::GameState::Leaving) ++leaving_frames;
            return;
        }
        if (briefing) {
            // CBriefing_Draw 100317b0 (CHud_Draw draws nothing else in view mode 5): the fade over the 3D image,
            // the letterbox bars, the mission title, then the subtitles when [CS] SUBTITLES is on.
            ui::fill_rect(f, vs, 0, 0, 640, 480, std::uint32_t(std::nearbyint(std::clamp(briefing->fade(), 0.f, 1.f) * 255.f)) << 24);
            ui::fill_rect(f, vs, 0, 0, 640, 75, 0xff000000u);
            ui::fill_rect(f, vs, 0, 405, 640, 480, 0xff000000u);
            ui::draw_text(f, vs, menu_font, 20, 20, briefing_title, 0xff808080u);
            const auto& wps = briefing->waypoints();
            const int cur = briefing->index();
            if (opt.subtitles && cur >= 0 && std::size_t(cur) < wps.size() && !wps[std::size_t(cur)].text_id.empty()) {
                const std::string text = utf8_to_cp1251(texts.get(std::string_view(wps[std::size_t(cur)].text_id)));
                // WrapTextToLines(GAME_FONT, 640): width ≤ round(ScaleX · 0.98 · 640) screen px; line step
                // round((height + 1) / ScaleY).
                const auto lines = ui::wrap_text(vs, *game_font.font, text, std::nearbyint(vs.sx() * 0.98f * 640.f));
                const float step = std::nearbyint(float(game_font.font->height + 1) * vs.glyph_scale / vs.sy());
                float y = 410;
                for (const auto& l : lines) ui::draw_text(f, vs, game_font, 10, y, l, 0xff808080u), y += step;
            }
            return;
        }
        if (pager && sim_ms - pager->shown_ms >= ui::kPagerLifetimeMs) pager.reset();
        if (objectives_visible && objectives_auto_hide && sim_ms - objectives_shown_ms >= ui::kObjectivesAutoHideMs)
            objectives_visible = false;
        // CHud_Draw: with the objectives screen up only it is drawn; otherwise the cockpit, the satellite
        // map, then the pager.
        hq_aspect = float(f.width) / float(std::max(1u, f.height));
        if (hq && hq->interface_up()) {
            // CHud_Draw in the HQ states 3–5: the HQ interface instead of the cockpit (HqUi_Draw), the satellite
            // map, the pager in its HQ place.
            hq_last_pick = hq_pick(hq->mouse_x, hq->mouse_y);
            const ui::HudContext hc{vs, game_font, hud_tex, hud_strings};
            if (const auto& g = hq->ghost(); g && g->on_ground && eco) {
                // [port] The placement ghost (the building's model tinted by validity, Placement_Update) is drawn as
                // its bounding circle on the ground in the validity colour.
                game::WorldObject probe;
                probe.type = g->type;
                const float r = eco->radius(probe);
                const std::uint32_t argb = g->validity == 0 ? 0xffff0000u : g->validity == 1 ? 0xff00ff00u : 0xffffff00u;
                std::optional<ui::Vec2f> prev;
                for (int k = 0; k <= 32; ++k) {
                    const float a = 6.2831853f * float(k) / 32;
                    const auto p = hq->project({g->pos.x + r * std::cos(a), g->pos.y + r * std::sin(a), g->pos.z + 0.5f});
                    if (p && prev) ui::draw_line(f, vs, prev->x, prev->y, p->x, p->y, argb);
                    prev = p;
                }
            }
            ui::draw_hq(f, hc, hq_sprites, *hq, objectives_visible);
            if (!objectives_visible && hq->map_open) {
                ui::HudState hs = build_hud(hq_aspect);
                hs.cockpit = false, hs.map_visible = true;
                ui::draw_hud(f, hc, hs);
            }
            if (pager && !objectives_visible) ui::draw_pager(f, vs, game_font, *pager, true);
        } else if (!objectives_visible) {
            const ui::HudState hs = build_hud(float(f.width) / float(std::max(1u, f.height)));
            ui::draw_hud(f, ui::HudContext{vs, game_font, hud_tex, hud_strings}, hs);
            if (pager) ui::draw_pager(f, vs, game_font, *pager);
        }
        if (objectives_visible && runtime) {
            ui::ObjectivesScreen o;
            o.dark = hq && hq->interface_up(); // views 3/4/5: the 80% backdrop
            o.title = ui_text(3062), o.bonus_title = ui_text(3061), o.footer = with_key(ui_text(1017), objectives_key);
            // Port deviation: the RU string 1017 hard-codes "F12" (no %s; ENG has %s), so the bound key replaces it.
            if (ui_text(1017).find("%s") == std::string::npos)
                if (auto p12 = o.footer.find("F12"); p12 != std::string::npos) o.footer.replace(p12, 3, objectives_key);
            o.state_word[0] = ui_text(1014), o.state_word[1] = ui_text(1027), o.state_word[2] = ui_text(1015);
            const auto& cfg = runtime->config();
            const auto& st = runtime->objectives();
            for (std::size_t i = 0; i < cfg.primary_objectives.size(); ++i)
                o.primary.push_back({utf8_to_cp1251(cfg.primary_objectives[i]), i < st.size() ? st[i] : 0});
            for (std::size_t i = 0; i < cfg.bonus_objectives.size(); ++i) {
                const std::size_t j = cfg.primary_objectives.size() + i;
                o.bonus.push_back({utf8_to_cp1251(cfg.bonus_objectives[i]), j < st.size() ? st[j] : 0});
            }
            ui::draw_objectives(f, vs, menu_font, o);
        }
    };

    // Sky and lighting of the mission.
    std::optional<mission::Ske> ske;
    if (auto p = resolve_path(*dir, "sky.ske")) ske = mission::decode_ske(read_file(*p));
    if (ske) {
        vo.sky_textures = load_sky_textures(lib, opt.mission);
        vo.sky = [&](double seconds, Vec3 forward, SkyDome& dome, SkyFrame& f) {
            const auto a = mission::evaluate_ske(*ske, seconds);
            f.lighting = scene_lighting(a, forward);
            if (opt.no_fog) f.lighting.fog_end = 0;
            colour_sky_dome(dome, a, horizon_colour(a, forward));
            f.cloud_color = a.cloud;
            f.ms = seconds * 1000;
        };
    }
    const Vec3 start_pos = hero.position();
    SDL_Log("play: hero %s at (%.0f, %.0f, %.0f), motion graph %s; %zu live units, %zu buildings", tma.objects[*hero_index].resource.c_str(),
            start_pos.x, start_pos.y, start_pos.z, hero.model.ctl_entry.c_str(), live.size(), buildings.size());
    for (auto& u : live) unit_start.push_back(u->position());
    int rc = run_scene_viewer(window, std::move(scene), vo);
    PlayOutcome outcome;
    if (end_ctl.state() == ui::GameState::Leaving) {
        if (end_ctl.won()) record_mission_complete(lib, opt.mission);
        if (end_ctl.pending_action() == ui::EndAction::Restart) rc = kPlayRestart;
        outcome.won = end_ctl.won();
        outcome.exit = end_ctl.pending_action() == ui::EndAction::Restart    ? PlayOutcome::Exit::Restart
                       : end_ctl.pending_action() == ui::EndAction::LoadGame ? PlayOutcome::Exit::LoadGame
                                                                             : PlayOutcome::Exit::Shell;
    }
    if (window_closed) outcome.exit = PlayOutcome::Exit::QuitApp;
    if (opt.outcome) *opt.outcome = outcome;
    for (std::size_t i = 0; i < live.size(); ++i) {
        const Vec3 a = unit_start[i], b = live[i]->position();
        const Vec3 h = b - hero.position();
        SDL_Log("play: unit %zu %08x %s moved %.1f m (order %u, state %u, moving %d, v=(%.2f %.2f %.2f), at (%.1f, %.1f, %.1f), placed z %.1f); %.0f m from the hero, "
                "%.0f / %.0f HP%s, target %d",
                i, live[i]->placement->identity, live[i]->placement->resource.c_str(), std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)),
                brains[i] ? brains[i]->current_order() : 0u, live[i]->motion->current_state(), brains[i] ? int(brains[i]->moving()) : -1,
                live[i]->motion->velocity().x, live[i]->motion->velocity().y, live[i]->motion->velocity().z, b.x, b.y, b.z, live[i]->placement->position[2], std::sqrt(dot(h, h)),
                combat.unit(live_ci[i]).damage.total, combat.unit(live_ci[i]).damage.initial, combat.unit(live_ci[i]).removed ? " (removed)" : "",
                combat.unit(live_ci[i]).target);
    }
    const Vec3 hero_pos = hero.position();
    SDL_Log("play: hero ended at (%.2f, %.2f, %.2f) after %.0f ms, state %u; %zu collision pushes; %zu wall triangles; "
            "%zu footsteps",
            hero_pos.x, hero_pos.y, hero_pos.z, sim_ms, motion.current_state(), pushed_steps, statics.triangle_count(), footsteps);
    SDL_Log("play: combat: %zu shots, %zu bullet hits, %zu kills; hero %.0f / %.0f HP, battery %.4f of %.0f, drain %.2f / %.2f per s",
            combat.shots, combat.hits, kills, hero_combat.damage.total, hero_combat.damage.initial, hero_combat.battery_fraction(),
            hero_combat.energy.battery_capacity().value_or(0.f), hero_combat.energy.drain_a, hero_combat.energy.drain_b);
    for (std::size_t g = 0; g < hero_combat.guns.size(); ++g)
        SDL_Log("play: hero gun %zu %s: %u shots, ammo %d, energy %.1f, state %u", g + 1, hero_combat.guns[g].spec().bullet.second.c_str(),
                hero_combat.guns[g].shots(), hero_combat.guns[g].ammo(), hero_combat.guns[g].energy(), unsigned(hero_combat.guns[g].state()));
    const Mat4& hm = hero.frame.matrix;
    SDL_Log("play: body forward (%.3f, %.3f), view forward (%.3f, %.3f, %.3f)", hm.at(0, 1), hm.at(1, 1), last_cam_forward.x,
            last_cam_forward.y, last_cam_forward.z);
    return rc;
}

} // namespace parkan