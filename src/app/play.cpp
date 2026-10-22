#include "app/play.hpp"

#include "app/scene_viewer.hpp"
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
#include "world/ground.hpp"

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

Vec3 translation(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

// The hero: the player's unit whose design lives under UNITS\UNITS\HERO (iron3d finds it by type
// 0x1020000 in the unit registry; the TMA has no such flag).
std::optional<std::size_t> find_hero(const mission::Tma& t) {
    for (std::size_t i = 0; i < t.objects.size(); ++i)
        if (contains_ci(t.objects[i].resource, "\\HERO\\") && contains_ci(t.objects[i].resource, ".dat")) return i;
    return std::nullopt;
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
    const mission::TmaObject& hero_obj = tma.objects[*hero_index];

    // World: the static scene without the hero, the hero appended as separately posed parts.
    RenderMesh scene = load_mission_render_mesh(lib, opt.mission, nullptr, *hero_index);
    auto hero = load_unit_model(lib, hero_obj.resource);
    if (!hero) throw std::runtime_error("hero design not found: " + hero_obj.resource);
    const std::uint32_t hero_first = append_render_mesh(scene, hero->mesh, Mat4::identity());

    std::string map_dir = tma.land_path;
    if (auto cut = map_dir.find_last_of("\\/"); cut != std::string::npos) map_dir.resize(cut);
    auto land = resolve_path(lib.game_dir(), map_dir + "/Land.msh");
    if (!land) throw std::runtime_error("no Land.msh for " + map_dir);
    const Terrain terrain = decode_terrain(read_file(*land));
    const TerrainGround ground(terrain);

    // Motion: the chassis's CTLD and CTPT drive the hero (Control.dll's motion graph + mover).
    const NResEntry* ce = lib.find(hero->ctl_archive, hero->ctl_entry);
    if (!ce) throw std::runtime_error("hero has no motion graph");
    const Ctld ctld = decode_ctld(ce->data);
    const Mesh& chassis_mesh = hero->parts.front().model.mesh;
    std::vector<ContactPoint> points;
    const std::string base = hero->ctl_entry.substr(0, hero->ctl_entry.rfind('.'));
    if (const NResEntry* pe = lib.find(hero->ctl_archive, base + ".cpt")) points = decode_ctpt(pe->data);
    UnitMotion motion(ctld, chassis_mesh, points);
    Mat4 spawn = tma_placement(hero_obj);
    if (auto g = ground.probe(translation(spawn) + Vec3{0, 0, 50}, 100.f))
        spawn.at(2, 3) = g->z;
    motion.start(0, spawn);

    // Input: M1.TBL through the original binding runtime, with the MacBook remap.
    std::vector<input::Binding> table;
    if (auto tbl = resolve_path(lib.game_dir(), "m1.tbl")) {
        const Bytes b = read_file(*tbl);
        table = input::parse_translate_table({reinterpret_cast<const char*>(b.data()), b.size()});
        input::apply_mac_defaults(table);
    }
    input::ManManager man(std::move(table));
    input::CommandState commands;
    std::vector<input::InputEvent> events;
    float mouse_dx = 0, mouse_dy = 0;

    double sim_ms = 0;
    Vec3 hero_pos = translation(spawn);

    ViewerOptions vo;
    vo.title = "Parkan — " + opt.mission;
    vo.shot_path = opt.shot_path;
    vo.start_time = opt.start_time;
    vo.pitch = 0.35f;
    vo.orbit_distance = 9.f;

    vo.on_event = [&](const SDL_Event& ev) {
        if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat) {
            if (const auto scan = input::scan_from_hid(int(ev.key.scancode)))
                events.push_back({ev.type == SDL_EVENT_KEY_DOWN, scan});
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            mouse_dx += ev.motion.xrel, mouse_dy += ev.motion.yrel;
        }
    };

    // One simulation frame at most 33 ms long: input → IControl requests → motion → ground.
    auto step = [&](double to_ms) {
        const float dt = float(to_ms - sim_ms);
        man.process(events, mouse_dx, mouse_dy, dt, commands);
        events.clear();
        mouse_dx = mouse_dy = 0;
        using namespace input;
        // ExecuteBinding (vehicle class 0): throttle.y from FORWARD/BACK/WALK_F/WALK_B, the angular
        // command from ROTATE_*; strafing (LEFT/RIGHT heading) is not modelled yet.
        float forward = commands.get(0, 0, kMcmdForward) + commands.get(0, 0, kMcmdBack) +
                        commands.get(0, 0, kMcmdWalkF) + commands.get(0, 0, kMcmdWalkB);
        if (opt.demo_walk) forward = 1;
        motion.set_throttle({0, std::clamp(forward, -1.f, 1.f), commands.get(0, 0, kMcmdUp) + commands.get(0, 0, kMcmdDown)});
        motion.set_angular_command(
            {commands.get(0, 0, kMcmdRotateX), commands.get(0, 0, kMcmdRotateY), commands.get(0, 0, kMcmdRotateZ)});
        sim_ms = to_ms;
        motion.update(float(sim_ms));

        // FollowGround, reduced: the feet (the CTPT contact points of the motion graph) are probed and
        // the body is moved so the lowest support stands on the ground (dz = max(ground − foot)).
        const MotionFrame f = motion.interpolate(float(sim_ms));
        pose_model_blend(hero->parts.front().model, f.frame_a, f.frame_b, f.weight_b);
        const auto& nodes = hero->parts.front().model.node_world;
        float dz = -FLT_MAX;
        std::vector<std::pair<std::size_t, float>> clearance;
        for (std::size_t c = 0; c < motion.contacts().size(); ++c) {
            const auto pi = motion.contacts()[c].point;
            if (pi >= points.size() || points[pi].part >= nodes.size()) continue;
            const Vec3 foot = (f.matrix * nodes[points[pi].part]).transform_point(points[pi].pos);
            if (auto g = ground.probe(foot)) {
                dz = std::max(dz, g->z - foot.z);
                clearance.push_back({c, foot.z - g->z});
            }
        }
        if (dz == -FLT_MAX) {
            if (auto g = ground.probe(translation(f.matrix))) dz = g->z - translation(f.matrix).z;
            else dz = 0;
        }
        motion.apply_offset({0, 0, dz});
        for (auto [c, gap] : clearance) motion.set_contact_touch(c, gap + dz < 0.1f);
    };

    vo.animate = [&](double seconds, std::vector<Mat4>& transforms) {
        const double now = seconds * 1000.0;
        while (sim_ms + 1e-6 < now) step(std::min(now, sim_ms + 33.0));
        const MotionFrame f = motion.interpolate(float(sim_ms));
        hero_pos = translation(f.matrix);
        pose_unit(*hero, f.matrix, f.frame_a, f.frame_b, f.weight_b, transforms, hero_first);
    };
    vo.follow = [&](Vec3& centre) { centre = hero_pos + Vec3{0, 0, 2.f}; };

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
    SDL_Log("play: hero %s at (%.0f, %.0f, %.0f), motion graph %s (%zu states)", hero_obj.resource.c_str(), hero_pos.x,
            hero_pos.y, hero_pos.z, hero->ctl_entry.c_str(), ctld.states.size());
    const int rc = run_scene_viewer(window, std::move(scene), vo);
    SDL_Log("play: hero ended at (%.2f, %.2f, %.2f) after %.0f ms, state %u", hero_pos.x, hero_pos.y, hero_pos.z, sim_ms,
            motion.current_state());
    return rc;
}

} // namespace parkan
