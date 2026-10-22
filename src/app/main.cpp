// Entry point. The original flow (iron_3d.exe → iron3d.dll shell → game) is in notes/ui-shell.md.
//
//   parkan [<game dir>]                                               placeholder game window
//   parkan --view <archive> <mesh> [--state <n>] [--time <s>] [--shot <out.bmp>] [<game dir>]
//                                                   model viewer; plays motion state n of the model's CTLD
//   parkan --terrain <map dir> [--shot <out.bmp>] [<game dir>]        terrain viewer (e.g. DATA/MAPS/Tut_2)
//   parkan --unit <scheme .dat> [--shot <out.bmp>] [<game dir>]      unit design (e.g. UNITS/AUTO/swlklas.dat)
//   parkan --play <mission dir> [--classic-keys] [--time <s>] [--no-fog] [--walk] [--shot <out.bmp>] [<game dir>]
//                                                   drive the mission's hero (M1.TBL keys; --walk holds forward)
//   parkan --mission <mission dir> [--time <s>] [--no-fog] [--shot <out.bmp>] [<game dir>]
//                                                   mission scene: terrain, placed objects, sky, lighting
//   parkan --sound <lib> <entry> [<game dir>]                         play one sample (e.g. sounds.lib i_button.wav)
//   parkan --music <n> [<game dir>]                                   play CD index n (MUSIC/Track{n+1:02}.ogg)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/play.hpp"
#include "app/scene_viewer.hpp"
#include "audio/audio_engine.hpp"
#include "res/ctld.hpp"
#include "mission/ske.hpp"
#include "scene/mission_scene.hpp"
#include "scene/sky.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <filesystem>
#include <string>

namespace {

// Data directory: the positional argument, else $PARKAN_DATA, else the GOG install inside CrossOver.
std::filesystem::path default_game_dir() {
    if (const char* env = std::getenv("PARKAN_DATA")) return env;
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "") /
           "Library/Application Support/CrossOver/Bottles/ParkanIS/drive_c/GOG Games/Parkan - Iron Strategy";
}

int run_placeholder() {
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
        }
        SDL_Delay(16);
    }
    return 0;
}

// --sound / --music smoke path: plays one sample or one music track to its end, then exits.
int run_audio(const std::filesystem::path& game, const std::string& lib_name, const std::string& entry, int music) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    auto quit = [] { // Ctrl-C arrives as SDL_EVENT_QUIT
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT) return true;
        return false;
    };
    int rc = 1;
    try {
        parkan::AssetLibrary lib(game);
        parkan::audio::AudioEngine audio(lib);
        if (!audio.ok()) {
            SDL_Log("no audio device");
        } else if (music >= 0) {
            if (audio.play_music(music, false)) {
                SDL_Log("music: index %d (Track%02d.ogg)", music, music + 1);
                while (!audio.music_finished() && !quit()) SDL_Delay(50);
                rc = 0;
            } else {
                SDL_Log("music index %d: no MUSIC/Track%02d.ogg", music, music + 1);
            }
        } else if (auto pcm = audio.load(lib_name, entry)) {
            SDL_Log("%s/%s: %d Hz, %d ch, %.2f s", lib_name.c_str(), entry.c_str(), pcm->rate, pcm->channels,
                    pcm->seconds());
            const parkan::audio::Handle h = audio.play({lib_name, entry});
            while (audio.is_playing(h) && !quit()) SDL_Delay(20);
            rc = 0;
        }
        if (rc == 0) SDL_Delay(250); // let the device drain its buffer
    } catch (const std::exception& ex) {
        SDL_Log("error: %s", ex.what());
    }
    SDL_Quit();
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path game = default_game_dir();
    std::string view_archive, view_mesh, unit, terrain, mission, shot;
    double start_time = 0;
    int start_state = 0;
    bool no_fog = false, demo_walk = false, demo_strafe = false, classic_keys = false, camera_sway = false;
    int camera_preset = 0;
    std::string play, sound_lib, sound_entry;
    int music = -1;
    float pitch = -1; // camera elevation override (radians); negative = the mode's default
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--view" && i + 2 < argc) {
            view_archive = argv[++i];
            view_mesh = argv[++i];
        } else if (a == "--sound" && i + 2 < argc) {
            sound_lib = argv[++i];
            sound_entry = argv[++i];
        } else if (a == "--music" && i + 1 < argc) {
            music = std::atoi(argv[++i]);
        } else if (a == "--unit" && i + 1 < argc) {
            unit = argv[++i];
        } else if (a == "--play" && i + 1 < argc) {
            play = argv[++i];
        } else if (a == "--walk") {
            demo_walk = true;
        } else if (a == "--camera-sway") {
            camera_sway = true;
        } else if (a == "--classic-keys") {
            classic_keys = true;
        } else if (a == "--strafe") {
            demo_strafe = true;
        } else if (a == "--outer" && i + 1 < argc) {
            camera_preset = std::atoi(argv[++i]);
        } else if (a == "--mission" && i + 1 < argc) {
            mission = argv[++i];
        } else if (a == "--terrain" && i + 1 < argc) {
            terrain = argv[++i];
        } else if (a == "--state" && i + 1 < argc) {
            start_state = std::atoi(argv[++i]);
        } else if (a == "--pitch" && i + 1 < argc) {
            pitch = float(std::atof(argv[++i]));
        } else if (a == "--no-fog") {
            no_fog = true;
        } else if (a == "--time" && i + 1 < argc) {
            start_time = std::atof(argv[++i]);
        } else if (a == "--shot" && i + 1 < argc) {
            shot = argv[++i];
        } else {
            game = a;
        }
    }
    if (!parkan::resolve_path(game, "MISSIONS")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Parkan",
                                 ("Game data not found in:\n" + game.string() +
                                  "\n\nPass the folder as an argument or set PARKAN_DATA.").c_str(),
                                 nullptr);
        return 1;
    }
    if (!sound_entry.empty() || music >= 0) return run_audio(game, sound_lib, sound_entry, music);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Parkan: Iron Strategy", 1024, 768,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                              (shot.empty() ? 0 : SDL_WINDOW_HIDDEN));
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Log("game data: %s", game.string().c_str());

    int rc = 0;
    try {
        parkan::AssetLibrary lib(game);
        parkan::ViewerOptions opt;
        opt.shot_path = shot;
        opt.start_time = start_time;
        if (!play.empty()) {
            parkan::PlayOptions po;
            po.mission = play;
            po.shot_path = shot;
            po.start_time = start_time;
            po.no_fog = no_fog;
            po.demo_walk = demo_walk;
            po.demo_strafe = demo_strafe;
            po.camera_preset = camera_preset;
            po.stabilize_camera = !camera_sway;
            po.layout = classic_keys ? parkan::input::KeyLayout::Classic : parkan::input::KeyLayout::Modern;
            rc = parkan::run_play(window, lib, po);
        } else if (!view_mesh.empty()) {
            opt.title = view_mesh;
            // Animation: the motion states of the model's CTLD (same archive, same base name). A state
            // plays track B (the clip; track A is usually empty and blended in only below full speed)
            // from its first to its last frame over its duration, every node at the same time; without
            // a duration, at kAnimFps (the game derives it from the requested speed).
            constexpr double kAnimFps = 15;
            parkan::ModelAsset posed;
            std::optional<parkan::Ctld> ctld;
            const std::string base = view_mesh.substr(0, view_mesh.rfind('.'));
            if (const parkan::NResEntry* ce = lib.find(view_archive, base + ".ctl")) ctld = parkan::decode_ctld(ce->data);
            int motion = ctld && !ctld->states.empty() ? std::clamp(start_state, 0, int(ctld->states.size()) - 1) : -1;
            opt.reload = [&](int state, int lod) {
                posed = parkan::load_model(lib, view_archive, view_mesh, state, lod);
                return parkan::to_render_mesh(posed);
            };
            opt.cycle = [&](int step) -> std::string {
                if (motion < 0) return "static";
                const int n = int(ctld->states.size());
                motion = (motion + step + n) % n;
                const auto& st = ctld->states[std::size_t(motion)];
                char buf[96];
                std::snprintf(buf, sizeof buf, "motion %d/%d  frames %g..%g", motion, n, st.track_b[0], st.track_b[1]);
                return buf;
            };
            opt.animate = [&](double seconds, std::vector<parkan::Mat4>& transforms) {
                if (motion < 0 || transforms.size() < posed.node_world.size()) return;
                const auto& st = ctld->states[std::size_t(motion)];
                const double from = st.track_b[0], to = st.track_b[1];
                const double dur = st.duration_ms > 0 ? st.duration_ms / 1000.0 : std::max(std::abs(to - from), 1.0) / kAnimFps;
                parkan::pose_model(posed, float(from + std::fmod(seconds / dur, 1.0) * (to - from)));
                std::copy(posed.node_world.begin(), posed.node_world.end(), transforms.begin());
            };
            rc = parkan::run_scene_viewer(window, opt.reload(0, 0), opt);
        } else if (!unit.empty()) {
            // Posable parts: the chassis plays motion state `--state` of its CTLD (track B, as in
            // --view) and the turrets and guns ride on their mount nodes.
            auto u = parkan::load_unit_model(lib, unit);
            if (!u) throw std::runtime_error("unit design not found: " + unit);
            SDL_Log("unit: %zu parts, motion graph %s", u->parts.size(), u->ctl_entry.c_str());
            std::optional<parkan::Ctld> ctld;
            if (const parkan::NResEntry* ce = lib.find(u->ctl_archive, u->ctl_entry)) ctld = parkan::decode_ctld(ce->data);
            int motion = ctld && !ctld->states.empty() ? std::clamp(start_state, 0, int(ctld->states.size()) - 1) : -1;
            opt.title = unit;
            opt.cycle = [&](int step) -> std::string {
                if (motion < 0) return "static";
                const int n = int(ctld->states.size());
                motion = (motion + step + n) % n;
                char buf[96];
                std::snprintf(buf, sizeof buf, "motion %d/%d", motion, n);
                return buf;
            };
            opt.animate = [&](double seconds, std::vector<parkan::Mat4>& transforms) {
                if (motion < 0) return;
                const auto& st = ctld->states[std::size_t(motion)];
                const double from = st.track_b[0], to = st.track_b[1];
                const double dur = st.duration_ms > 0 ? st.duration_ms / 1000.0 : std::max(std::abs(to - from), 1.0) / 15.0;
                const float t = float(from + std::fmod(seconds / dur, 1.0) * (to - from));
                parkan::pose_unit(*u, parkan::Mat4::identity(), -1, t, 1, transforms, 0);
            };
            rc = parkan::run_scene_viewer(window, u->mesh, opt);
        } else if (!mission.empty()) {
            parkan::MissionSceneStats st;
            auto scene = parkan::load_mission_render_mesh(lib, mission, &st);
            SDL_Log("mission: %zu objects, %zu placed, %zu unresolved", st.objects, st.placed, st.unresolved);
            opt.title = mission;
            opt.pitch = pitch >= 0 ? pitch : 0.7f;
            opt.distance = 1.6f;
            // The mission sky (sky.ske + sky.wea): time of day from the ske clock, which starts at the
            // file's start time; `--time` advances it (a day lasts the key period, usually 900 s).
            std::optional<parkan::mission::Ske> ske;
            if (auto dir = parkan::resolve_path(game, mission))
                if (auto p = parkan::resolve_path(*dir, "sky.ske")) ske = parkan::mission::decode_ske(parkan::read_file(*p));
            if (ske) {
                opt.sky_textures = parkan::load_sky_textures(lib, mission);
                opt.sky = [&, no_fog](double seconds, parkan::Vec3 forward, parkan::SkyDome& dome, parkan::SkyFrame& f) {
                    const auto a = parkan::mission::evaluate_ske(*ske, seconds);
                    f.lighting = parkan::scene_lighting(a, forward);
                    if (no_fog) f.lighting.fog_end = 0;
                    parkan::colour_sky_dome(dome, a, parkan::horizon_colour(a, forward));
                    f.cloud_color = a.cloud;
                    f.ms = seconds * 1000;
                };
            }
            rc = parkan::run_scene_viewer(window, std::move(scene), opt);
        } else if (!terrain.empty()) {
            opt.title = terrain;
            opt.pitch = 0.7f;
            opt.distance = 1.6f;
            rc = parkan::run_scene_viewer(window, parkan::load_terrain_render_mesh(lib, terrain), opt);
        } else {
            rc = run_placeholder();
        }
    } catch (const std::exception& ex) {
        SDL_Log("error: %s", ex.what());
        rc = 1;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
