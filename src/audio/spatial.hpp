// The 3D sound model of the shipped configuration (Ngi32 hardware DS3D, NO_VIRTUALIZATION; notes/audio.md
// §2.4), as pure functions so it can be tested without an audio device. Engine coordinates throughout.
//
// DS3D's own virtualisation is not reproducible; the port uses plain stereo panning from the source
// direction in the listener frame (right = forward × up), attenuating only the far channel.
#pragma once

#include "core/math.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace parkan::audio {

constexpr float kSpeedOfSound = 343.3f; // DirectSound's default, distance factor 1 (units = metres)
constexpr float kPanDepth = 0.8f;       // far-channel cut at 90°; full cut sounds unnatural on headphones

// DirectSound inverse-distance law clamped at min (6 dB per doubling); 0 beyond max
// (DSBCAPS_MUTE3DATMAXDISTANCE, and the engine cancels/suspends such channels anyway).
inline float distance_gain(float d, float min_distance, float max_distance) {
    if (d > max_distance) return 0;
    if (d <= min_distance) return 1;
    return min_distance / (min_distance + (d - min_distance)); // rolloff 1.0
}

struct StereoGain {
    float left = 1, right = 1;
};

// `to_source` = source − listener (any length); a source at the listener is centred.
inline StereoGain pan_gain(Vec3 to_source, Vec3 forward, Vec3 up) {
    const float len = std::sqrt(dot(to_source, to_source));
    if (len <= 1e-4f) return {};
    const float x = std::clamp(dot(normalize(cross(forward, up)), to_source) / len, -1.f, 1.f);
    return {1 - kPanDepth * std::max(x, 0.f), 1 - kPanDepth * std::max(-x, 0.f)};
}

// Doppler frequency ratio (DS3D doppler factor 1): positive when source and listener close in.
inline float doppler_ratio(Vec3 source, Vec3 source_vel, Vec3 listener, Vec3 listener_vel) {
    const Vec3 d = source - listener;
    const float len = std::sqrt(dot(d, d));
    if (len <= 1e-4f) return 1;
    const Vec3 u = d * (1 / len); // listener → source
    const float c = kSpeedOfSound, lim = 0.5f * kSpeedOfSound;
    const float vl = std::clamp(dot(listener_vel, u), -lim, lim); // listener towards the source
    const float vs = std::clamp(dot(source_vel, u), -lim, lim);   // source away from the listener
    return (c + vl) / (c + vs);
}

// MusicPlayRandomTrack (iron3d 1008e950): any present track but the current one; a lone track repeats.
// `random` is any uniformly distributed value. Returns -1 when there are no tracks.
inline int pick_random_track(std::span<const int> tracks, int current, std::uint32_t random) {
    if (tracks.empty()) return -1;
    const auto others = std::size_t(tracks.size() - std::size_t(std::count(tracks.begin(), tracks.end(), current)));
    if (others == 0) return tracks.front();
    std::size_t k = random % others;
    for (int t : tracks)
        if (t != current && k-- == 0) return t;
    return -1; // unreachable
}

} // namespace parkan::audio
