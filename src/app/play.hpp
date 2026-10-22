// First playable slice: a mission with its sky, the player's hero unit driven through the original
// binding table (M1.TBL with the MacBook remap) → motion graph → mover, following the ground, and a
// camera that orbits the hero.
#pragma once

#include "res/library.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace parkan {

struct PlayOptions {
    std::string mission;    // game-relative mission folder
    std::string shot_path;  // render one frame and exit
    double start_time = 0;  // seconds; with --shot, the simulation runs up to this time first
    bool no_fog = false;
    bool demo_walk = false; // hold "forward" from the start (for --shot checks)
};

int run_play(SDL_Window* window, AssetLibrary& lib, const PlayOptions& opt);

} // namespace parkan
