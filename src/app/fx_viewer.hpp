// `parkan --effect <name>`: one FXID effect from effects.rlb, simulated by src/fx at the origin above
// a ground grid, drawn with its own minimal SDL_GPU path (alpha, additive and multiply blending,
// depth-tested but not depth-written transparent quads). Loops the effect; Space pauses, R restarts.
#pragma once

#include <SDL3/SDL.h>

#include <string>

namespace parkan {

class AssetLibrary;

struct EffectViewerOptions {
    std::string effect;    // effects.rlb entry
    double time = 0.5;     // --shot: simulated seconds since the start (30 Hz steps)
    std::string shot_path; // render one frame to this BMP and return
    float distance = 0;    // camera distance; 0 = fit the effect
    float pitch = 0.3f;    // camera elevation (radians)
};

int run_effect_viewer(SDL_Window* window, AssetLibrary& lib, const EffectViewerOptions& opt);

} // namespace parkan
