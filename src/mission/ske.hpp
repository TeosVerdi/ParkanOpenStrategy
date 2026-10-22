// Time-of-day keyframes: MISSIONS/<...>/sky.ske, read by Terrain.dll LoadSkeFile (1003dec0) /
// ReadSkeLayer (1003b7e0) into CAtmData, evaluated by CAtmData_GetObjectParams (1003ed20).
// Spec: notes/atmosphere-render.md §2–§3, §8.
#pragma once

#include "core/bytes.hpp"
#include "core/math.hpp"

#include <array>
#include <optional>

namespace parkan::mission {

// T32: 6 × i32 + 8 bytes; the game reads [0] (key, in the start record), [3] hours and [4] minutes.
struct SkeTime {
    std::int32_t key = 0, hours = 0, minutes = 0;
    int total_minutes() const { return hours * 60 + minutes; }
};

struct SkeLayer {
    SkeTime time;                  // on the virtual 24 h clock
    std::int32_t event = 7;        // 0 sun/moon on, 1 off, 3/4 rain, 5/6 snow, 8/9 lightning, 7 none
    std::int32_t unused_2c = 0;
    std::array<std::uint32_t, 4> horizon{}; // ARGB, disk order L30 L34 L38 L3C
    float fog_start = 0, fog_end = 0;       // × 700 world units
    std::array<std::uint32_t, 4> mid{};     // L48 L4C L50 L54
    std::array<std::uint32_t, 4> high{};    // L58 L5C L60 L64
    std::uint32_t zenith = 0, unused_70 = 0, sprite = 0, cloud = 0, sun_light = 0, ambient = 0, back_light = 0;
    std::array<std::string, 6> strings;     // [0] = "sun" or "moon" on event 0
    float sprite_w = 0, sprite_h = 0, intensity = 0, density = 0;
    std::vector<std::string> list;          // rain sound or lightning effect
};

struct SkeKey {
    bool present = false;
    SkeTime time_a;
    SkeTime period; // real-time length of this day
    std::vector<SkeLayer> layers;
    double period_seconds() const { return period.total_minutes() * 60.0; }
};

struct Ske {
    std::int32_t version = 0;
    std::vector<SkeKey> keys;
    SkeTime start;
    std::int32_t tail_a = 0, tail_b = 0;
};

Ske decode_ske(std::span<const std::uint8_t> data);

// The atmosphere at one moment (values after interpolation, as pushed into CSky/CSun).
struct AtmosphereState {
    // Quadrant q faces azimuth q·90° (direction (sin φ, cos φ)): q0 +Y, q1 +X, q2 -Y, q3 -X.
    std::array<std::uint32_t, 4> low{}, mid{}, high{}; // ARGB
    std::uint32_t zenith = 0;
    Vec3 cloud{1, 1, 1}, ambient{};
    float fog_start = 0, fog_end = 0; // world units
    bool sun = false;                 // a sun or moon is up (directional lights exist)
    bool is_moon = false;
    Vec3 light_dir{0, 0, -1};         // direction the light travels (from the body to the ground)
    Vec3 light_color{}, back_color{}; // light A (may exceed 1) and the opposite fill light B
    std::uint32_t sprite = 0;
    double day_seconds = 0;           // position in the cycle, for display
};

// `seconds` = game time since the mission started; the clock starts at Ske::start.
AtmosphereState evaluate_ske(const Ske& ske, double seconds);

// Linear per-byte ARGB interpolation (LerpArgbColor 100403a0).
std::uint32_t lerp_argb(std::uint32_t a, std::uint32_t b, float f);

} // namespace parkan::mission
