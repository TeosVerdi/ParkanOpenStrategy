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

namespace parkan {

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
};

// Returns the viewer's code, or kPlayRestart when the player restarted the mission (R after a failure).
inline constexpr int kPlayRestart = 2;
int run_play(SDL_Window* window, AssetLibrary& lib, const PlayOptions& opt);

} // namespace parkan
