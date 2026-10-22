#include "app/play.hpp"

#include "app/scene_viewer.hpp"
#include "audio/audio_engine.hpp"
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
#include "world/live_unit.hpp"
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
    RenderMesh scene = load_mission_render_mesh(lib, opt.mission, nullptr, [&](std::size_t i) { return is_live[i]; });
    StaticColliders statics; // the terrain's draws use transform 0
    statics.add(scene, 1, std::uint32_t(scene.transforms.size()));
    statics.finish();
    for (auto& u : live) u->first_transform = append_render_mesh(scene, u->model.mesh, Mat4::identity());
    LiveUnit& hero = *live[hero_slot];
    UnitMotion& motion = *hero.motion;
    std::vector<LiveUnit*> live_ptrs;
    for (auto& u : live) live_ptrs.push_back(u.get());

    // Sound (notes/audio.md): in a mission a random track plays once, then a different random one; the
    // listener is the rendered camera. Not started for --shot.
    std::unique_ptr<audio::AudioEngine> sound;
    if (opt.shot_path.empty()) {
        sound = std::make_unique<audio::AudioEngine>(lib);
        if (!sound->ok()) sound.reset();
        else sound->play_random_music(false);
    }
    std::size_t footsteps = 0;

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

    double sim_ms = 0;
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
            const auto outer_key = input::ui_key("CMD_JAMES_OUTER_CAMERA", opt.layout);
            if (ev.type == SDL_EVENT_KEY_DOWN && outer_key && input::scan_from_hid(int(ev.key.scancode)) == outer_key->key) {
                // JAMES_OUTER_CAMERA (C): AdvanceOuterMode; preset 0 blends back into the TURRET view.
                outer_preset = (outer_preset + 1) % 5;
                if (outer_preset != 0) camera_mode = 1;
                outer_blend_start = sim_ms;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F10) // port debug orbit
                camera_mode = camera_mode < 0 ? 0 : -1;
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            const std::uint16_t b = ev.button.button == SDL_BUTTON_LEFT    ? input::kScanMouseLeft
                                    : ev.button.button == SDL_BUTTON_RIGHT ? input::kScanMouseRight
                                                                           : input::kScanMouseMiddle;
            events.push_back({ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, b});
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
        forward = std::clamp(forward, -1.f, 1.f);
        // Strafing (ExecuteBinding LEFT/RIGHT): walk (throttle.y = ±1) with an attitude offset of ±90°,
        // ±45° while also walking forward or back; the motion graph sees a plain walk request.
        // The LEFT/RIGHT value is the strafe throttle (1 in the original tables; the modern layout walks
        // at 0.5 and sprints at 1 with Shift).
        const float left_v = opt.demo_strafe ? 1.f : commands.get(0, 0, kMcmdLeft), right_v = commands.get(0, 0, kMcmdRight);
        const bool left = left_v > 0, right = right_v > 0;
        float heading = 0;
        if (left != right) {
            const float k = forward != 0 ? 0.5f * (forward < 0 ? -1.f : 1.f) : 1.f;
            heading = (left ? 1.f : -1.f) * k * 1.5707963f;
            if (forward == 0) forward = std::max(left_v, right_v);
        }
        motion.set_attitude_offset({0, 0, heading});
        motion.set_throttle({0, forward, commands.get(0, 0, kMcmdUp) + commands.get(0, 0, kMcmdDown)});
        // HERO.TBL mouse X → ANGLE_Z: the chassis attitude target (IControl +0x48) gets the mouse delta
        // on top of the remaining turn the mover has not consumed yet (GetProperty 0x30).
        const float angle_z = commands.get(0, 0, kMcmdAngleZ);
        float dz_turn = angle_z - last_angle_z;
        dz_turn -= std::round(dz_turn);
        last_angle_z = angle_z;
        if (dz_turn != 0) {
            Vec3 t = motion.attitude_target();
            t.z = t.z + dz_turn - std::floor(t.z + dz_turn);
            motion.set_attitude_target(t);
        } else if (commands.get(0, 0, kMcmdRotateZ) != 0) {
            motion.set_angular_command({0, 0, commands.get(0, 0, kMcmdRotateZ)});
        }
        sim_ms = to_ms;
        for (auto& u : live) u->motion->update(float(sim_ms));
        for (auto& u : live) {
            const Vec3 push = u->settle(float(sim_ms), ground, statics);
            if (u.get() == &hero && dot(push, push) > 0) ++pushed_steps;
        }
        collide_units(live_ptrs, float(sim_ms));
        // Footsteps: a touch-down starts its step effect (step_h* by surface; the default surface's
        // entry until surfaces are decoded); the effect's sound commands play at the foot.
        for (auto& u : live)
            for (const auto& td : u->touchdowns)
                if (auto fx = u->touchdown_effect(td.event_list)) {
                    ++footsteps;
                    if (sound)
                        for (const auto& s : load_fx_sounds(lib, fx->second)) sound->play(audio::fx_sound(s), td.position);
                }
        if (sound && sound->music_finished()) sound->play_random_music(false);
    };

    vo.animate = [&](double seconds, std::vector<Mat4>& transforms) {
        const double now = seconds * 1000.0;
        while (sim_ms + 1e-6 < now) step(std::min(now, sim_ms + 33.0));
        for (auto& u : live) {
            // HERO.TBL drives the hero's turret (mouse Y); others hold the manual-entry default for now.
            const bool is_hero = u.get() == &hero;
            const float tx = is_hero ? commands.get(1, 1, input::kMcmdAngleX) : TurretRig::kManualX;
            const float ty = is_hero ? commands.get(1, 1, input::kMcmdAngleY) : TurretRig::kManualY;
            // From its own TURRET camera the unit is not drawn (inferred from CCameraDevice::CreateCamera
            // handing the camera to IAniMesh slot 0x84). The camera reads the posed model, not these.
            u->pose(transforms, tx, ty, is_hero && camera_mode == 0);
        }
    };
    vo.follow = [&](Vec3& centre) { centre = hero.position() + Vec3{0, 0, 2.f}; };
    vo.relative_mouse = true;
    // CCameraDevice::Update: eye = CameraCenter, X = TargetDirect (the barrel), up = CameraCenter's
    // vector; free look (LShift + mouse) rotates locally by Rz((0.5 − ax)·2π) · Ry((0.5 − ay)·π).
    vo.camera = [&](ViewerOptions::Camera& cam) {
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
        const Mat4 me = last_frame.matrix * nodes[pe.part], ma = last_frame.matrix * nodes[pa.part];
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
        cam.fov_x = rig->camera_fov, cam.znear = rig->camera_near, cam.zfar = rig->camera_far;
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
        }
        return true;
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
    SDL_Log("play: hero %s at (%.0f, %.0f, %.0f), motion graph %s; %zu live units", tma.objects[*hero_index].resource.c_str(),
            start_pos.x, start_pos.y, start_pos.z, hero.model.ctl_entry.c_str(), live.size());
    const int rc = run_scene_viewer(window, std::move(scene), vo);
    const Vec3 hero_pos = hero.position();
    SDL_Log("play: hero ended at (%.2f, %.2f, %.2f) after %.0f ms, state %u; %zu collision pushes; %zu wall triangles; "
            "%zu footsteps",
            hero_pos.x, hero_pos.y, hero_pos.z, sim_ms, motion.current_state(), pushed_steps, statics.triangle_count(), footsteps);
    const Mat4& hm = hero.frame.matrix;
    SDL_Log("play: body forward (%.3f, %.3f), view forward (%.3f, %.3f, %.3f)", hm.at(0, 1), hm.at(1, 1), last_cam_forward.x,
            last_cam_forward.y, last_cam_forward.z);
    return rc;
}

} // namespace parkan
