// The game as launched without debug flags: iron_3d.exe's loop (401190) — the shell (src/shell) until it
// starts a mission, the mission (app/play), back to the shell with the launch block, until the player
// quits. Also the port's first-run data launcher (choose an installed game folder or a disc image).
#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

namespace parkan {

class AssetLibrary;

struct ShellAppOptions {
    std::string shot_form;  // --shell-shot <form>: render that screen offscreen and exit
    std::string shot_path;  // its BMP
    double shot_time = 0;   // seconds of shell time before the shot (intro movies, credits, blinking)
    bool skip_intro = false; // --skip-intro
    bool classic_keys = false;
    std::filesystem::path pref; // SDL_GetPrefPath("", "Parkan")
    std::string handoff_mission; // --handoff-test: shell frame, a mission frame (GPU), then the shell shot
};

// Returns the process exit code. `change_data_source` is set when the player asked for the launcher
// (the options form's port button): the caller re-runs the launcher and then the shell.
int run_shell_app(SDL_Window* window, AssetLibrary& lib, const ShellAppOptions& opt, bool& change_data_source);

// The first-run launcher (a port addition: the original was installed by its setup program). Shows the
// candidates and lets the player pick a game folder or import a disc image. Returns the chosen game
// folder, or empty when the window was closed. `remembered` is offered first when valid.
std::filesystem::path run_data_launcher(SDL_Window* window, const std::filesystem::path& pref, const std::filesystem::path& candidate,
                                        const std::string& shot_path = {});

// The remembered data folder (<pref>/launcher.ini `data=`), if it still holds a complete game.
std::filesystem::path remembered_data_dir(const std::filesystem::path& pref);
void remember_data_dir(const std::filesystem::path& pref, const std::filesystem::path& dir);

} // namespace parkan
