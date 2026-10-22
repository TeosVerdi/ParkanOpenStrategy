// Where effects come from: the local frame each effect gets from the CTLD event that spawns it
// (type 4, fx::contact_frame). Effects are authored in those units — bullet tracers ×0.02…0.2, gun
// flashes ×0.3 — so an effect shown on its own needs its spawner's frame (notes/effects.md §9).
#pragma once

#include "core/math.hpp"

#include <map>
#include <string>

namespace parkan {
class AssetLibrary;
}

namespace parkan::fx {

// Lower-case effects.rlb name → the largest spawn frame found: every bullet's situation-0 events
// (load_bullet) and the type-4 events of every CTLD in the object archives whose .cpt shares its base
// name. Effects spawned otherwise (type 3/5 events, EXPL, code) are absent.
std::map<std::string, Mat4> collect_spawn_frames(AssetLibrary& lib);

} // namespace parkan::fx
