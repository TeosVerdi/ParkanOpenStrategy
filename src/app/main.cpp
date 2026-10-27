// Entry point. The original flow (iron_3d.exe → iron3d.dll shell → game) is in notes/ui-shell.md.
//
//   parkan [<game dir>] [--skip-intro] [--classic-keys] [--launcher]  the game: intro, shell, missions (app/shell_app)
//   parkan --shell-shot <screen> <out.bmp> [--time <s>] [<game dir>]  one shell screen offscreen: intro, main, single,
//                                                   campaigns, missions[:CC], singles, load, options, video, audio,
//                                                   controls, tuning, credits, quit
//   parkan --play <mission dir> --briefing ...                        start the mission with its briefing
//   parkan --view <archive> <mesh> [--state <n>] [--time <s>] [--shot <out.bmp>] [<game dir>]
//                                                   model viewer; plays motion state n of the model's CTLD
//   parkan --terrain <map dir> [--shot <out.bmp>] [<game dir>]        terrain viewer (e.g. DATA/MAPS/Tut_2)
//   parkan --unit <scheme .dat> [--shot <out.bmp>] [<game dir>]      unit design (e.g. UNITS/AUTO/swlklas.dat)
//   parkan --play <mission dir> [--classic-keys] [--time <s>] [--no-fog] [--walk] [--fire] [--aim] [--shot <out.bmp>] [<game dir>]
//        [--goto x,y[,heading°]] [--target <hex LogicID>] [--damage-mult k]   port debug aids (buildings)
//        [--pilot <hex LogicID> [t [t_exit]]]            the hero boards that own unit at game time t (view state 1)
//        [--help-tab <n>]                                the help screen (F1) open on tab n (0 mission, 1 controls, 2 messages)
//        [--enter <hex LogicID>] [--hq-screen <n>] [--click x,y[,t][,r|m]]... port debug aids (HQ screens): the
//                                                   hero enters that own building, screen n opens, virtual
//                                                   640×480 clicks at game time t (r = right button, m = move)
//                                                   drive the mission's hero (M1.TBL keys; --walk holds forward,
//                                                   --fire holds LMB with every gun selected, --aim turns to the
//                                                   nearest hostile unit and aims the bullets at it)
//   parkan --mission <mission dir> [--time <s>] [--no-fog] [--shot <out.bmp>] [<game dir>]
//                                                   mission scene: terrain, placed objects, sky, lighting
//   parkan --sound <lib> <entry> [<game dir>]                         play one sample (e.g. sounds.lib i_button.wav)
//   parkan --music <n> [<game dir>]                                   play CD index n (MUSIC/Track{n+1:02}.ogg)
//   parkan --effect <name> [--time <s>] [--shot <out.bmp>] [<game dir>] one effects.rlb effect (src/fx)
//   --lang en                                       use the English GOG install when no game dir is given
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/fx_viewer.hpp"
#include "app/play.hpp"
#include "app/scene_viewer.hpp"
#include "app/shell_app.hpp"
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

// Data directory: the positional argument, else $PARKAN_DATA, else a GOG install inside CrossOver:
// the Russian release (2.0.0.4), or the English one (2.0.0.3) with `--lang en` or when it is the only
// one installed (notes/releases.md).
std::filesystem::path default_game_dir(bool english) {
    if (const char* env = std::getenv("PARKAN_DATA")) return env;
    const char* home = std::getenv("HOME");
    const auto gog = std::filesystem::path(home ? home : "") / "Library/Application Support/CrossOver/Bottles/ParkanIS/drive_c/GOG Games";
    const auto ru = gog / "Parkan - Iron Strategy", en = gog / "Parkan - Iron Strategy - ENG";
    if (english) return en;
    std::error_code ec;
    return !std::filesystem::exists(ru, ec) && std::filesystem::exists(en, ec) ? en : ru;
}

std::filesystem::path pref_path() {
    char* p = SDL_GetPrefPath("", "Parkan");
    if (!p) return {};
    std::filesystem::path out(p);
    SDL_free(p);
    return out;
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
    std::filesystem::path game;
    bool english = false;
    std::string view_archive, view_mesh, unit, terrain, mission, shot;
    double start_time = 0;
    int start_state = 0;
    bool no_fog = false, demo_walk = false, demo_strafe = false, demo_fire = false, demo_aim = false, classic_keys = false, camera_sway = false;
    int camera_preset = 0;
    std::string play, sound_lib, sound_entry, effect, shell_shot_form;
    bool skip_intro = false, force_launcher = false, lang_given = false;
    std::string handoff;
    int music = -1;
    float pitch = -1; // camera elevation override (radians); negative = the mode's default
    parkan::PlayOptions debug_play; // --goto / --target / --damage-mult (port debug aids)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--view" && i + 2 < argc) {
            view_archive = argv[++i];
            view_mesh = argv[++i];
        } else if (a == "--sound" && i + 2 < argc) {
            sound_lib = argv[++i];
            sound_entry = argv[++i];
        } else if (a == "--effect" && i + 1 < argc) {
            effect = argv[++i];
        } else if (a == "--music" && i + 1 < argc) {
            music = std::atoi(argv[++i]);
        } else if (a == "--unit" && i + 1 < argc) {
            unit = argv[++i];
        } else if (a == "--play" && i + 1 < argc) {
            play = argv[++i];
        } else if (a == "--briefing") {
            debug_play.briefing = true;
        } else if (a == "--walk") {
            demo_walk = true;
        } else if (a == "--goto" && i + 1 < argc) {
            float x = 0, y = 0, h = 0;
            const int n = std::sscanf(argv[++i], "%f,%f,%f", &x, &y, &h);
            if (n >= 2) debug_play.goto_xy = std::pair{x, y};
            if (n == 3) debug_play.goto_heading = h;
        } else if (a == "--target" && i + 1 < argc) {
            debug_play.aim_target = std::uint32_t(std::strtoul(argv[++i], nullptr, 16));
            demo_aim = true;
        } else if (a == "--pilot" && i + 1 < argc) {
            debug_play.pilot = std::uint32_t(std::strtoul(argv[++i], nullptr, 16));
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) debug_play.pilot_time = std::atof(argv[++i]);
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) debug_play.pilot_exit = std::atof(argv[++i]);
        } else if (a == "--enter" && i + 1 < argc) {
            debug_play.hq_enter = std::uint32_t(std::strtoul(argv[++i], nullptr, 16));
        } else if (a == "--hq-screen" && i + 1 < argc) {
            debug_play.hq_screen = std::atoi(argv[++i]);
        } else if (a == "--help-tab" && i + 1 < argc) {
            debug_play.help_tab = std::atoi(argv[++i]);
        } else if (a == "--click" && i + 1 < argc) {
            parkan::PlayOptions::VirtualClick c;
            char r = 0;
            const int n = std::sscanf(argv[++i], "%f,%f,%lf,%c", &c.x, &c.y, &c.t, &r);
            c.right = n == 4 && r == 'r', c.move_only = n == 4 && r == 'm';
            if (n >= 2) debug_play.hq_clicks.push_back(c);
        } else if (a == "--damage-mult" && i + 1 < argc) {
            debug_play.damage_mult = float(std::atof(argv[++i]));
        } else if (a == "--fire") {
            demo_fire = true;
        } else if (a == "--aim") {
            demo_aim = true;
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
        } else if (a == "--lang" && i + 1 < argc) {
            english = std::string_view(argv[++i]) == "en";
            lang_given = true;
        } else if (a == "--shell-shot" && i + 2 < argc) {
            shell_shot_form = argv[++i];
            shot = argv[++i];
        } else if (a == "--handoff-test" && i + 1 < argc) {
            handoff = argv[++i];
        } else if (a == "--skip-intro") {
            skip_intro = true;
        } else if (a == "--launcher") {
            force_launcher = true;
        } else {
            game = a;
        }
    }
    // The game itself (no debug mode): the data folder is the positional argument, $PARKAN_DATA or --lang
    // (overrides), else the folder remembered by the launcher, else the launcher runs (first start).
    const bool game_mode = view_mesh.empty() && unit.empty() && terrain.empty() && mission.empty() && play.empty() && effect.empty() &&
                           sound_entry.empty() && music < 0;
    const std::filesystem::path pref = pref_path();
    const bool overridden = !game.empty() || std::getenv("PARKAN_DATA") || lang_given;
    bool need_launcher = false;
    if (game.empty() && !overridden) game = parkan::remembered_data_dir(pref);
    if (game.empty()) {
        game = default_game_dir(english);
        need_launcher = game_mode && !overridden && shell_shot_form.empty();
    }
    if (force_launcher) need_launcher = true;
    if (!need_launcher && !parkan::resolve_path(game, "MISSIONS")) {
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
    if (need_launcher) {
        game = parkan::run_data_launcher(window, pref, game, shell_shot_form.empty() ? shot : std::string());
        if (!shot.empty() && shell_shot_form.empty()) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        if (game.empty()) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
    }
    SDL_Log("game data: %s", game.string().c_str());

    int rc = 0;
    try {
        parkan::AssetLibrary lib(game);
        parkan::ViewerOptions opt;
        opt.shot_path = shot;
        opt.start_time = start_time;
        if (!effect.empty()) {
            parkan::EffectViewerOptions eo;
            eo.effect = effect;
            eo.time = start_time > 0 ? start_time : 0.5;
            eo.shot_path = shot;
            if (pitch >= 0) eo.pitch = pitch;
            rc = parkan::run_effect_viewer(window, lib, eo);
        } else if (!play.empty()) {
            parkan::PlayOptions po = debug_play;
            po.mission = play;
            po.shot_path = shot;
            po.start_time = start_time;
            po.no_fog = no_fog;
            po.demo_walk = demo_walk;
            po.demo_fire = demo_fire;
            po.demo_aim = demo_aim;
            po.demo_strafe = demo_strafe;
            po.camera_preset = camera_preset;
            po.stabilize_camera = !camera_sway;
            po.layout = classic_keys ? parkan::input::KeyLayout::Classic : parkan::input::KeyLayout::Modern;
            po.user_dir = pref;
            do rc = parkan::run_play(window, lib, po);
            while (rc == parkan::kPlayRestart); // R after a failure reloads the mission
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
            // iron_3d.exe: the shell, the game, back to the shell (app/shell_app).
            parkan::ShellAppOptions so;
            so.shot_form = shell_shot_form;
            so.shot_path = shell_shot_form.empty() ? std::string() : shot;
            so.shot_time = start_time;
            so.skip_intro = skip_intro;
            so.classic_keys = classic_keys;
            so.pref = pref;
            so.handoff_mission = handoff;
            bool change = false;
            rc = parkan::run_shell_app(window, lib, so, change);
            while (change) {
                // The options form's "game data" button: the launcher, then the shell on the new folder.
                const auto next = parkan::run_data_launcher(window, pref, lib.game_dir());
                if (next.empty()) break;
                parkan::AssetLibrary next_lib(next);
                so.skip_intro = true;
                rc = parkan::run_shell_app(window, next_lib, so, change);
            }
        }
    } catch (const std::exception& ex) {
        SDL_Log("error: %s", ex.what());
        rc = 1;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
