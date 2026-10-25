// First playable slice: a mission with its sky, the player's hero unit driven through the original
// binding table (M1.TBL with the MacBook remap) → motion graph → mover, following the ground, and a
// camera that orbits the hero.
#pragma once

#include "input/keymap.hpp"
#include "res/library.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace parkan {

// What the mission ended with (the exe's launch block after CGame::Run: launch+0x14c mode, launch+0x157 won;
// notes/mission-end.md §5). The shell reads it to record a win and pick the form to show.
struct PlayOutcome {
    enum class Exit {
        Shell,    // mode 1: Esc after the end message, leaving the mission, or an only_briefing mission's end
        Restart,  // mode 2: R after a failure — run the same mission again
        LoadGame, // mode 3: L after a failure — the shell's load-game form
        QuitApp,  // port: the window was closed
    } exit = Exit::Shell;
    bool won = false; // launch+0x157 (the win is already recorded in dispatcher.ini when set)
};

struct PlayOptions {
    std::string mission;    // game-relative mission folder
    std::string shot_path;  // render one frame and exit
    double start_time = 0;  // seconds; with --shot, the simulation runs up to this time first
    bool no_fog = false;
    bool demo_walk = false;   // hold "forward" from the start (for --shot checks)
    bool demo_strafe = false; // hold "strafe left" from the start
    bool demo_fire = false;   // hold the fire button (LMB) from the start, every gun selected
    bool demo_aim = false;    // port debug aid: turn to the nearest hostile unit and aim every bullet at it
    int camera_preset = 0;    // 0 TURRET view, 1..4 OUTER presets (for --shot checks)
    input::KeyLayout layout = input::KeyLayout::Modern; // --classic-keys: the 2001 layout
    bool stabilize_camera = true; // --camera-sway: the original's data-driven cockpit sway
    // Port debug aids (buildings, notes/buildings.md): --goto x,y[,heading°] puts the hero there at the
    // start; --target <hex LogicID> makes --aim pick that object (a building too); --damage-mult k
    // multiplies the hero's bullet damage (gun property 0xb4).
    std::optional<std::pair<float, float>> goto_xy;
    std::optional<float> goto_heading;
    std::optional<std::uint32_t> aim_target;
    float damage_mult = 1;
    // The shell's entry: play the mission's briefing first (mission.cfg `object briefing`); --play skips it
    // unless --briefing is given.
    bool briefing = false;
    bool subtitles = true; // iron_3d.ini [CS] SUBTITLES (the shipped file has 1)
    // iron_3d.ini [CS] SFX_VOLUME / CD_VOLUME (÷ 100) and PLAY_CD_MUSIC; negative = leave the mixer as it is.
    float sfx_volume = -1, music_volume = -1;
    bool music = true;
    bool software_cursor = true; // iron_3d.ini [CS] FORCE_SOFTWARE_CURSOR (shipped 1); false: the system pointer in the HQ
    PlayOutcome* outcome = nullptr; // filled when the mission ends
    // Port debug aids for the HQ screens (notes/hq-ui.md): --enter <hex LogicID> puts the hero into that own
    // building at the start (as the capture lift's callback does), --hq-screen <n> opens screen n once the HQ
    // interface is up, --click x,y[,t][,r|m] clicks the 640×480 virtual screen at game time t s (r: right button, m: only moves
    // the pointer there).
    std::optional<std::uint32_t> hq_enter;
    // --pilot <hex LogicID> [t [t_exit]]: the hero boards that own unit (ENTER_STATE → view state 1) at game time
    // t s and gets out (ROLLBACK_STATE) at t_exit.
    std::optional<std::uint32_t> pilot;
    double pilot_time = 0, pilot_exit = -1;
    int hq_screen = -1;
    struct VirtualClick {
        float x = 0, y = 0;
        double t = 0;
        bool right = false, move_only = false;
    };
    std::vector<VirtualClick> hq_clicks;
};

// Returns the viewer's code, or kPlayRestart when the player restarted the mission (R after a failure).
inline constexpr int kPlayRestart = 2;
int run_play(SDL_Window* window, AssetLibrary& lib, const PlayOptions& opt);

} // namespace parkan
