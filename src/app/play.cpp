#include "app/play.hpp"

#include "app/scene_viewer.hpp"
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
    // Static colliders: every placed object's faces (the terrain's draws use transform 0).
    StaticColliders statics;
    statics.add(scene, 1, std::uint32_t(scene.transforms.size()));
    statics.finish();
    const std::uint32_t hero_first = append_render_mesh(scene, hero->mesh, Mat4::identity());
    // The hero's collision sphere: around the chassis's static-pose bounds (inferred; the original's
    // radius source, CCollObject +0x50, is not traced).
    const ModelAsset& chassis = hero->parts.front().model;
    const Vec3 sphere_centre = (chassis.bounds_min + chassis.bounds_max) * 0.5f;
    const Vec3 ext = chassis.bounds_max - chassis.bounds_min;
    const float sphere_radius = 0.5f * std::max(ext.x, ext.y);

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

    // The turret: the first component whose CTLD has a turret device (CTurretDevice + CCameraDevice).
    std::size_t turret_part = 0;
    std::optional<TurretRig> rig;
    std::vector<ContactPoint> turret_points;
    for (std::size_t i = 1; i < hero->parts.size() && !rig; ++i) {
        const UnitPart& p = hero->parts[i];
        if (const NResEntry* te = lib.find(p.archive, p.ctl_entry)) {
            rig = TurretRig::from_ctld(decode_ctld(te->data));
            turret_part = i;
            if (const NResEntry* pe = lib.find(p.archive, p.cpt_entry)) turret_points = decode_ctpt(pe->data);
        }
    }

    // Input: the hero is driven through HERO.TBL (iron3d loads it for the hero; warbots use M1/M2),
    // with the MacBook remap.
    std::vector<input::Binding> table;
    if (auto tbl = resolve_path(lib.game_dir(), "hero.tbl")) {
        const Bytes b = read_file(*tbl);
        table = input::parse_translate_table({reinterpret_cast<const char*>(b.data()), b.size()});
        input::apply_mac_defaults(table);
    }
    input::ManManager man(std::move(table));
    input::CommandState commands;
    std::vector<input::InputEvent> events;
    float mouse_dx = 0, mouse_dy = 0;

    double sim_ms = 0;
    float last_angle_z = 0.5f;
    // Cameras: 0 = TURRET view (first person, the default), 1..4 = OUTER presets (C cycles them),
    // -1 = the port's debug orbit (Tab).
    int camera_mode = 0, outer_preset = 0;
    struct OuterParams {
        float yaw, height, dist;
    };
    static constexpr OuterParams kOuter[5] = {{0, 0, 0}, {0.4f, 0.15f, 2.5f}, {0.2f, 0.15f, 4.5f}, {-0.2f, 0.15f, 4.5f}, {-0.4f, 0.15f, 2.5f}};
    if (opt.camera_preset > 0 && opt.camera_preset < 5) camera_mode = 1, outer_preset = opt.camera_preset;
    OuterParams outer_cur = kOuter[outer_preset];
    double outer_blend_start = -1;
    MotionFrame last_frame;
    Vec3 last_cam_forward{};
    std::size_t pushed_steps = 0;
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
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_C) {
                // JAMES_OUTER_CAMERA (C): AdvanceOuterMode; preset 0 blends back into the TURRET view.
                outer_preset = (outer_preset + 1) % 5;
                if (outer_preset != 0) camera_mode = 1;
                outer_blend_start = sim_ms;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_TAB)
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
        const bool left = commands.get(0, 0, kMcmdLeft) > 0.5f || opt.demo_strafe, right = commands.get(0, 0, kMcmdRight) > 0.5f;
        float heading = 0;
        if (left != right) {
            const float k = forward != 0 ? 0.5f * (forward < 0 ? -1.f : 1.f) : 1.f;
            heading = (left ? 1.f : -1.f) * k * 1.5707963f;
            if (forward == 0) forward = 1;
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

        // CCollManager::Update for the hero: out of static walls, inside the landscape box; the push
        // of a ground unit (state flag 4) is made horizontal (ApplyCollisionPush).
        const Vec3 centre = motion.interpolate(float(sim_ms)).matrix.transform_point(sphere_centre);
        Vec3 push = statics.push_sphere(centre, sphere_radius);
        push = push + contain_in_world_bounds(centre + push, sphere_radius, ground.bounds_min(), ground.bounds_max());
        if (motion.live().flags & 4) push = horizontal_push(push);
        if (dot(push, push) > 0) {
            motion.apply_offset(push);
            ++pushed_steps;
        }
    };

    vo.animate = [&](double seconds, std::vector<Mat4>& transforms) {
        const double now = seconds * 1000.0;
        while (sim_ms + 1e-6 < now) step(std::min(now, sim_ms + 33.0));
        const MotionFrame f = motion.interpolate(float(sim_ms));
        last_frame = f;
        hero_pos = translation(f.matrix);
        if (rig) // TURRET ANGLE_X/Y (HERO.TBL: mouse Y raises the barrel); manual entry starts at 0.5, 0.5
            rig->node_frames(commands.get(1, 1, input::kMcmdAngleX), commands.get(1, 1, input::kMcmdAngleY),
                             f.attitude_offset.z, hero->parts[turret_part].node_frames);
        pose_unit(*hero, f.matrix, f.frame_a, f.frame_b, f.weight_b, transforms, hero_first);
        // From its own TURRET camera the unit is not drawn (inferred from CCameraDevice::CreateCamera
        // handing the camera to IAniMesh slot 0x84): collapse its transforms. The camera reads the posed
        // model, not these.
        if (camera_mode == 0)
            for (std::size_t i = hero_first; i < transforms.size(); ++i) transforms[i] = Mat4{};
    };
    vo.follow = [&](Vec3& centre) { centre = hero_pos + Vec3{0, 0, 2.f}; };
    vo.relative_mouse = true;
    // CCameraDevice::Update: eye = CameraCenter, X = TargetDirect (the barrel), up = CameraCenter's
    // vector; free look (LShift + mouse) rotates locally by Rz((0.5 − ax)·2π) · Ry((0.5 − ay)·π).
    vo.camera = [&](ViewerOptions::Camera& cam) {
        if (camera_mode < 0 || !rig || rig->camera_eye_point < 0 || std::size_t(rig->camera_eye_point) >= turret_points.size() ||
            std::size_t(rig->camera_axis_point) >= turret_points.size())
            return false;
        const auto& nodes = hero->parts[turret_part].model.node_world;
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
            const float h = std::atan2(fwd.y, fwd.x) + outer_cur.yaw, R = sphere_radius;
            cam.eye = cam.eye - Vec3{std::cos(h) * R * outer_cur.dist, std::sin(h) * R * outer_cur.dist, R * outer_cur.height};
            // ClipOuterToWorld, reduced to the ground: keep the camera 0.75 above the landscape.
            if (auto g = ground.probe(cam.eye + Vec3{0, 0, 50}, 100.f); g && cam.eye.z < g->z + 0.75f) cam.eye.z = g->z + 0.75f;
            cam.znear = 0.5f, cam.zfar = 175.f, cam.fov_x = 1.3f;
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
    SDL_Log("play: hero %s at (%.0f, %.0f, %.0f), motion graph %s (%zu states)", hero_obj.resource.c_str(), hero_pos.x,
            hero_pos.y, hero_pos.z, hero->ctl_entry.c_str(), ctld.states.size());
    const int rc = run_scene_viewer(window, std::move(scene), vo);
    SDL_Log("play: hero ended at (%.2f, %.2f, %.2f) after %.0f ms, state %u; %zu collision pushes; %zu wall triangles",
            hero_pos.x, hero_pos.y, hero_pos.z, sim_ms, motion.current_state(), pushed_steps, statics.triangle_count());
    const Mat4& hm = last_frame.matrix;
    SDL_Log("play: body forward (%.3f, %.3f), view forward (%.3f, %.3f, %.3f)", hm.at(0, 1), hm.at(1, 1), last_cam_forward.x,
            last_cam_forward.y, last_cam_forward.z);
    return rc;
}

} // namespace parkan
