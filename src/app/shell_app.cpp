#include "app/shell_app.hpp"

#include "app/play.hpp"
#include "app/screen2d.hpp"
#include "audio/audio_engine.hpp"
#include "core/paths.hpp"
#include "game/mission_runtime.hpp"
#include "res/library.hpp"
#include "res/texm.hpp"
#include "scene/model.hpp"
#include "res/textres.hpp"
#include "res/tft.hpp"
#include "shell/ini.hpp"
#include "shell/shell.hpp"
#include "input/scancodes.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace parkan {

namespace {

std::string text_of(const Bytes& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

std::string read_text(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return {};
    return text_of(read_file(p));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// The user's copies of the files the original rewrote in the game folder (iron_3d.ini, dispatcher.ini):
// <pref>/<name>, seeded from the game's own file the first time.
std::string user_file(const std::filesystem::path& pref, AssetLibrary& lib, const char* name, const char* game_rel) {
    const auto p = pref / name;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return read_text(p);
    if (auto g = resolve_path(lib.game_dir(), game_rel)) return text_of(read_file(*g));
    return {};
}

// Windows virtual-key code of an SDL key (the keys the shell reads; letters and digits as their ASCII).
int vk_of(const SDL_KeyboardEvent& k) {
    switch (k.scancode) {
    case SDL_SCANCODE_ESCAPE: return shell::kVkEscape;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return shell::kVkReturn;
    case SDL_SCANCODE_SPACE: return shell::kVkSpace;
    case SDL_SCANCODE_TAB: return shell::kVkTab;
    case SDL_SCANCODE_BACKSPACE: return shell::kVkBack;
    case SDL_SCANCODE_LEFT: return shell::kVkLeft;
    case SDL_SCANCODE_RIGHT: return shell::kVkRight;
    case SDL_SCANCODE_UP: return shell::kVkUp;
    case SDL_SCANCODE_DOWN: return shell::kVkDown;
    default: break;
    }
    if (k.key >= 'a' && k.key <= 'z') return int(k.key - 'a' + 'A');
    if (k.key >= '0' && k.key <= '9') return int(k.key);
    return 0;
}

// Everything the shell draws with: the texture pages, the background pieces, the fonts.
struct ShellResources {
    std::vector<ModelTexture> textures;         // uploaded in order
    std::map<std::string, std::int32_t> slots;  // lower-case name → index
    struct FontSet {
        int size;
        TftFont menu, game, tool;
    };
    std::vector<FontSet> fonts;
    std::map<std::string, std::string> sounds; // key → sounds.lib entry
    std::string sounds_lib = "sounds.lib";
};

std::string lib_path(std::string s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            out += '/';
            while (i + 1 < s.size() && s[i + 1] == '\\') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

ShellResources load_resources(AssetLibrary& lib, const shell::MenuResources& mr) {
    ShellResources r;
    auto add_lib = [&](const std::string& library, const std::map<std::string, std::uint32_t>& names) {
        try {
            const NResArchive& a = lib.archive(lib_path(library));
            for (const auto& [name, index] : names) {
                if (index >= a.entries().size() || r.slots.count(name)) continue;
                try {
                    Texm t = decode_texm(a.entries()[index].data);
                    if (t.mips_rgba.empty()) continue;
                    r.slots[name] = std::int32_t(r.textures.size());
                    r.textures.push_back({name, t.width, t.height, t.mips_rgba.front()});
                } catch (const std::exception& ex) {
                    SDL_Log("shell: texture %s: %s", name.c_str(), ex.what());
                }
            }
        } catch (const std::exception& ex) {
            SDL_Log("shell: %s: %s", library.c_str(), ex.what());
        }
    };
    add_lib(mr.textures_library, mr.textures);
    add_lib(mr.backgrounds_library, mr.backgrounds);
    try {
        const auto& a = lib.archive("UI/font.lib");
        for (int size : {640, 800, 1024}) {
            const std::string s = std::to_string(size);
            const NResEntry *m = a.find_name("mf_" + s + ".tft"), *g = a.find_name("gf_" + s + ".tft"), *t = a.find_name("tf_" + s + ".tft");
            if (!m || !g || !t) continue;
            r.fonts.push_back({size, decode_tft(m->data), decode_tft(g->data), decode_tft(t->data)});
        }
    } catch (const std::exception& ex) {
        SDL_Log("shell: fonts: %s", ex.what());
    }
    r.sounds = mr.sounds;
    r.sounds_lib = mr.sounds_library;
    return r;
}

// SAVE/saveslots.cfg: `object saveslots quantity = N`, `object slotK name, filename, empty`.
std::vector<std::string> load_save_slots(AssetLibrary& lib) {
    std::vector<std::string> out;
    auto p = resolve_path(lib.game_dir(), "SAVE/saveslots.cfg");
    if (!p) return out;
    const auto objs = game::parse_cfg(text_of(read_file(*p)));
    int n = 0;
    for (const auto& o : objs)
        if (lower(o.name) == "saveslots")
            if (const std::string* q = o.get("quantity")) n = std::atoi(q->c_str());
    for (int i = 1; i <= n; ++i) {
        std::string name;
        for (const auto& o : objs)
            if (lower(o.name) == "slot" + std::to_string(i)) {
                const std::string* e = o.get("empty");
                const std::string* nm = o.get("name");
                if (!(e && lower(*e) == "true") && nm) name = *nm;
            }
        out.push_back(name);
    }
    return out;
}

} // namespace

int run_shell_app(SDL_Window* window, AssetLibrary& lib, const ShellAppOptions& opt, bool& change_data_source) {
    change_data_source = false;
    const auto& game = lib.game_dir();
    // Data shared by every shell instance.
    shell::Shell::Config base;
    if (auto p = resolve_path(game, "UI/shell_ctrls.cfg")) base.ctrls = shell::parse_shell_ctrls(text_of(read_file(*p)));
    shell::MenuResources mr;
    if (auto p = resolve_path(game, "UI/menu_resources.cfg")) mr = shell::parse_menu_resources(text_of(read_file(*p)));
    if (auto p = resolve_path(game, "UI/cursor.cfg")) {
        const std::string t = text_of(read_file(*p));
        const auto sprites = ui::parse_sprite_cfg(t);
        if (auto it = sprites.find("arrow"); it != sprites.end()) base.cursor = it->second;
        for (const auto& o : game::parse_cfg(t))
            if (lower(o.name) == "arrow")
                if (const std::string* d = o.get("phase_delay")) base.cursor_phase = float(std::atof(d->c_str())) / 1000.f;
    }
    TextRes strings;
    if (auto p = resolve_path(game, "iron3d.dll")) strings = TextRes::from_pe(read_file(*p));
    base.str = [&strings](int id) { return utf8_to_cp1251(strings.get(std::uint32_t(id))); };
    base.game_dir = game;
    if (auto p = resolve_path(game, "DATA/credits.txt")) {
        std::string c = text_of(read_file(*p));
        std::erase(c, '\r'); // the scroller reads the file dropping every CR
        base.credits = std::move(c);
    }
    base.save_slots = load_save_slots(lib);
    base.video_modes = {"640x480x32", "800x600x32", "1024x768x32", "1280x960x32", "1600x1200x32"};
    const bool russian = !base.str(6135).empty() && (unsigned char)base.str(6135)[0] >= 0x80;
    base.data_source_label = utf8_to_cp1251(russian ? "Данные игры..." : "Game data...");
    base.intro = !opt.skip_intro;
    base.user_dir = opt.pref;
    ShellResources res = load_resources(lib, mr);

    std::string ini = user_file(opt.pref, lib, "iron_3d.ini", "Iron_3D.ini");
    auto save_ini = [&] {
        if (opt.pref.empty()) return;
        std::ofstream(opt.pref / "iron_3d.ini", std::ios::binary) << ini;
    };

    int mode = 0;
    shell::LaunchBlock last;
    const bool shot = !opt.shot_path.empty();
    for (;;) {
        // ---- The shell (CShell::Init / Run / Shutdown) ----
        shell::Shell::Config cfg = base;
        cfg.ini = ini;
        cfg.dispatcher = user_file(opt.pref, lib, "dispatcher.ini", "MISSIONS/dispatcher.ini");
        auto sh = std::make_unique<shell::Shell>(std::move(cfg), mode, last);
        auto screen = std::make_unique<Screen2D>(window);
        std::map<std::string, std::int32_t> slot_of;
        for (std::size_t i = 0; i < res.textures.size(); ++i) {
            const auto& t = res.textures[i];
            slot_of[t.name] = screen->add_texture(t.width, t.height, t.rgba.data());
        }
        std::vector<std::int32_t> font_slots;
        for (const auto& f : res.fonts)
            for (const TftFont* t : {&f.menu, &f.game, &f.tool}) font_slots.push_back(screen->add_texture(t->atlas_width, t->atlas_height, t->rgba.data()));
        const std::int32_t movie_slot = screen->add_texture(640, 480, nullptr, true, true);
        slot_of["movie"] = movie_slot;
        std::unique_ptr<audio::AudioEngine> sound;
        if (!shot) {
            sound = std::make_unique<audio::AudioEngine>(lib);
            if (!sound->ok()) sound.reset();
        }
        auto apply_volumes = [&] {
            if (!sound) return;
            sound->set_gain(audio::Bus::Sfx, sh->sfx_volume);
            sound->set_gain(audio::Bus::Music, sh->cd_volume);
        };
        apply_volumes();
        audio::Handle movie_voice = 0;

        shell::GuiEnv env;
        env.ctrls = &sh->config().ctrls;
        env.texture = [&](std::string_view n) {
            auto it = slot_of.find(std::string(n));
            return it == slot_of.end() ? std::int32_t(-1) : it->second;
        };
        auto setup_env = [&] {
            const Screen2D::Area a = screen->area();
            env.vs = {a.width, a.height, a.scale};
            const int want = ui::nearest_font_set(a.width / a.scale, a.height / a.scale);
            std::size_t k = 0;
            while (k + 1 < res.fonts.size() && res.fonts[k].size != want) ++k;
            if (k < res.fonts.size()) {
                env.menu_font = {&res.fonts[k].menu, font_slots[3 * k]};
                env.game_font = {&res.fonts[k].game, font_slots[3 * k + 1]};
                env.tool_font = {&res.fonts[k].tool, font_slots[3 * k + 2]};
            }
        };
        setup_env();
        const Uint64 t0 = SDL_GetTicks();
        env.now_ms = 0;
        // StartShellMusicTrack("main_menu_track"): stop, then the configured track (−1 random), looped.
        auto handle_effects = [&] {
            shell::ShellEffects e = sh->take_effects();
            if (e.settings_changed) ini = sh->settings_text(), save_ini();
            if (e.sfx_volume || e.cd_volume) apply_volumes();
            if (sound) {
                if (e.music == shell::ShellEffects::Music::Stop) sound->stop_music();
                if (e.music == shell::ShellEffects::Music::MenuTrack) {
                    sound->stop_music();
                    if (sh->music_on) {
                        const int track = sh->config().ctrls.main_menu_track;
                        if (track < 0) sound->play_random_music(true);
                        else sound->play_music(track, true);
                    }
                }
                if (e.start_movie && !e.start_movie->audio().empty()) {
                    // The movie's sound: DirectShow's audio renderer at full volume (no IBasicAudio).
                    auto pcm = std::make_shared<Pcm>();
                    pcm->rate = e.start_movie->audio_rate(), pcm->channels = e.start_movie->audio_channels();
                    pcm->samples = e.start_movie->audio();
                    sound->set_gain(audio::Bus::Sfx, 1.f);
                    movie_voice = sound->play_pcm(std::move(pcm));
                }
                if (e.stop_movie_audio) {
                    sound->stop(movie_voice), movie_voice = 0;
                    apply_volumes();
                }
            }
            if (e.apply_video) {
                const int w = sh->setting("DISPLAY_WIDTH", 1024), h = sh->setting("DISPLAY_HEIGHT", 768);
                SDL_SetWindowSize(window, w, h);
            }
            if (e.change_data_source) change_data_source = true;
        };
        auto play_sounds = [&] {
            for (const std::string& key : env.sounds) {
                auto it = res.sounds.find(key);
                if (sound && it != res.sounds.end()) sound->play({res.sounds_lib, it->second});
            }
            env.sounds.clear();
        };

        sh->start(env);
        if (shot && !opt.handoff_mission.empty()) {
            // Port check of the renderer hand-off (shell 2D → mission SDL_GPU → shell 2D) without input.
            OverlayFrame f;
            sh->draw(f, env);
            screen->present(f);
            sound.reset(), screen.reset();
            PlayOptions po;
            po.mission = opt.handoff_mission;
            po.briefing = true;
            po.shot_path = opt.shot_path + ".mission.bmp";
            po.start_time = 8;
            run_play(window, lib, po);
            screen = std::make_unique<Screen2D>(window);
            slot_of.clear();
            for (const auto& t : res.textures) slot_of[t.name] = screen->add_texture(t.width, t.height, t.rgba.data());
            font_slots.clear();
            for (const auto& fs : res.fonts)
                for (const TftFont* t : {&fs.menu, &fs.game, &fs.tool}) font_slots.push_back(screen->add_texture(t->atlas_width, t->atlas_height, t->rgba.data()));
            setup_env();
        }
        if (shot) {
            // --shell-shot: drive the shell to the named screen, then advance the clock to --time.
            const std::string f = opt.shot_form;
            auto go = [&](int id) { sh->show_form(env, id); };
            if (f == "main") go(shell::kMainMenu);
            else if (f == "single") go(shell::kSingleGame);
            else if (f == "campaigns") go(shell::kCampaignList);
            else if (f.rfind("missions", 0) == 0) {
                go(shell::kCampaignList);
                const int cc = f.size() > 9 ? std::atoi(f.c_str() + 9) : 0;
                sh->form(shell::kCampaignList)->on_command(env, {0, 0, cc, 0});
            } else if (f == "singles") {
                go(shell::kSingleGame);
                sh->form(shell::kSingleGame)->on_command(env, {2, 0, 0, 0});
            } else if (f == "load") go(shell::kLoadGame);
            else if (f == "options") go(shell::kOptions);
            else if (f == "video") go(shell::kVideoOptions);
            else if (f == "audio") go(shell::kAudioOptions);
            else if (f == "controls") go(shell::kRedefineControls);
            else if (f == "tuning") go(shell::kControlsTuning);
            else if (f == "credits") go(shell::kCredits);
            else if (f == "quit") go(shell::kAskQuit);
            // "intro": mode 0 already shows logo1.
            env.mouse_x = env.mouse_y = -1000;
            for (double t = 0; t <= opt.shot_time * 1000; t += 1000.0 / 30) {
                env.now_ms = t;
                sh->update(env);
                handle_effects();
                OverlayFrame scratch;
                scratch.width = std::uint32_t(env.vs.width), scratch.height = std::uint32_t(env.vs.height);
                sh->draw(scratch, env);
                if (const shell::Movie* m = sh->movie(); m && m->shown_frame() >= 0) screen->update_texture(movie_slot, m->frame().data());
            }
            env.sounds.clear();
            OverlayFrame frame;
            frame.width = std::uint32_t(env.vs.width), frame.height = std::uint32_t(env.vs.height);
            sh->draw(frame, env);
            const bool ok = screen->save_bmp(frame, opt.shot_path);
            SDL_Log("shell screenshot %s (%s): %s", opt.shot_path.c_str(), f.c_str(), ok ? "saved" : SDL_GetError());
            return ok ? 0 : 1;
        }

        bool quit_app = false;
        int last_movie_frame = -1;
        while (!sh->finished() && !quit_app && !change_data_source) {
            setup_env();
            env.now_ms = double(SDL_GetTicks() - t0);
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_EVENT_QUIT: quit_app = true; break;
                case SDL_EVENT_KEY_DOWN: {
                    const int vk = vk_of(ev.key) ? vk_of(ev.key) : ev.key.scancode == SDL_SCANCODE_DELETE ? 0x2e : 0;
                    env.scan_name = std::string(input::scan_name(input::scan_from_hid(int(ev.key.scancode))));
                    if (vk) sh->key_down(env, vk);
                    // WM_CHAR for letters (the ask-quit form's Y / N).
                    if (ev.key.key >= 'a' && ev.key.key <= 'z')
                        sh->char_in(env, (unsigned char)((ev.key.mod & SDL_KMOD_SHIFT) ? ev.key.key - 'a' + 'A' : ev.key.key));
                    break;
                }
                case SDL_EVENT_KEY_UP:
                    if (const int vk = vk_of(ev.key)) sh->key_up(env, vk);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    screen->to_virtual(ev.motion.x, ev.motion.y, env.mouse_x, env.mouse_y);
                    sh->mouse_move(env, env.mouse_x, env.mouse_y);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_RIGHT && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        screen->to_virtual(ev.button.x, ev.button.y, env.mouse_x, env.mouse_y);
                        sh->rbutton_down(env, env.mouse_x, env.mouse_y);
                    }
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    screen->to_virtual(ev.button.x, ev.button.y, env.mouse_x, env.mouse_y);
                    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) sh->lbutton_down(env, env.mouse_x, env.mouse_y);
                    else sh->lbutton_up(env, env.mouse_x, env.mouse_y);
                    break;
                default: break;
                }
            }
            sh->update(env);
            handle_effects();
            if (const shell::Movie* m = sh->movie(); m && m->shown_frame() >= 0 && m->shown_frame() != last_movie_frame) {
                screen->update_texture(movie_slot, m->frame().data());
                last_movie_frame = m->shown_frame();
            }
            // The cursor: the shell's software arrow, or the system one when the hardware cursor is chosen.
            const bool hw = sh->setting("FORCE_SOFTWARE_CURSOR", 1) == 0;
            if (hw && sh->cursor_visible()) SDL_ShowCursor();
            else SDL_HideCursor();
            OverlayFrame frame;
            frame.width = std::uint32_t(env.vs.width), frame.height = std::uint32_t(env.vs.height);
            env.software_cursor = !hw;
            sh->draw(frame, env);
            play_sounds();
            screen->present(frame);
        }
        // CShell::Run returns once the exit sound has played (the queue drained).
        play_sounds();
        if (sh->exit_code() >= 0 && sound) {
            const Uint64 wait = SDL_GetTicks();
            while (SDL_GetTicks() - wait < 600) SDL_Delay(10);
        }
        SDL_ShowCursor();
        if (quit_app || change_data_source || sh->exit_code() == 0) return 0;

        // ---- The game (CGame::Run), repeated for R (mode 2) ----
        last = sh->launch();
        sound.reset();
        sh.reset();
        screen.reset();
        PlayOptions po;
        po.mission = last.path;
        po.briefing = true;
        po.subtitles = shell::ini_get_int(ini, "CS", "SUBTITLES", 0) != 0;
        po.sfx_volume = float(shell::ini_get_int(ini, "CS", "SFX_VOLUME", 25)) / 100.f;
        po.music_volume = float(shell::ini_get_int(ini, "CS", "CD_VOLUME", 25)) / 100.f;
        po.music = shell::ini_get_int(ini, "CS", "PLAY_CD_MUSIC", 1) != 0;
        po.software_cursor = shell::ini_get_int(ini, "CS", "FORCE_SOFTWARE_CURSOR", 1) != 0;
        po.layout = opt.classic_keys ? input::KeyLayout::Classic : input::KeyLayout::Modern;
        po.user_dir = opt.pref;
        PlayOutcome out;
        po.outcome = &out;
        do {
            out = {};
            run_play(window, lib, po);
        } while (out.exit == PlayOutcome::Exit::Restart);
        if (out.exit == PlayOutcome::Exit::QuitApp) return 0;
        last.mode = out.exit == PlayOutcome::Exit::LoadGame ? 3 : 1;
        last.won = out.won;
        mode = 2;
    }
}

} // namespace parkan
