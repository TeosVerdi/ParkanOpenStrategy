// Effect.dll random numbers: 16-bit LFSR pairs, one global pair per subsystem (notes/effects.md §6).
//
//   Lfsr16Next (10002220): a = (a << 1) ^ b; b = (b >> 1) ^ a; returns b (0..65535)
//   LfsrRandCentered (10002680): b · (r / 65536) − r / 2, uniform in [−r/2, r/2)
//
// The original seeds every pair at DLL load from ngiGetClocks() (static initialisers 10002660,
// 10003a50, 10009180, …), so the shipped game is not reproducible; the port seeds them from one
// 32-bit value. A zero pair would stay zero forever, which the seeding avoids.
#pragma once

#include <cstdint>

namespace parkan::fx {

struct Lfsr16 {
    std::uint16_t a = 0x1234, b = 0x5678;

    std::uint16_t next() {
        const std::uint16_t na = std::uint16_t(a << 1) ^ b;
        const std::uint16_t nb = std::uint16_t(b >> 1) ^ na;
        a = na, b = nb;
        return nb;
    }
    // FUN_10002680 and the inlined copies: the scale is rounded to float first, like the original.
    float centered(float range) {
        const float scale = range * 1.5258789e-05f;
        return float(double(next()) * double(scale) - double(range) * 0.5);
    }
    float unit() { return float(next()) * 1.5258789e-05f; } // [0, 1)
    void seed(std::uint32_t s) {
        a = std::uint16_t(s), b = std::uint16_t(s >> 16);
        if (!a && !b) a = 1;
    }
};

// The streams, named after the code that draws from them (globals in Effect.dll .bss).
struct FxRandom {
    Lfsr16 shared;   // 10023688: particle start/end positions and sizes, debris, light colour jitter
    Lfsr16 beam;     // 100238c0: random beam frame
    Lfsr16 instance; // 10024110: t jitter, position jitter, occlusion re-test interval
    Lfsr16 debris;   // 10024810: debris shapes, spin and vertex jitter
    Lfsr16 lod;      // 10024a48: per-particle LOD thresholds (FxSettings_RandomLodThreshold 1000ec50)
    Lfsr16 trail;    // 100250f0: trail spawn randoms
    Lfsr16 light;    // 10024c80: light colour/range jitter

    explicit FxRandom(std::uint32_t seed = 0x5eed1234u) { reseed(seed); }
    void reseed(std::uint32_t seed) {
        Lfsr16* all[] = {&shared, &beam, &instance, &debris, &lod, &trail, &light};
        std::uint32_t s = seed;
        for (Lfsr16* l : all) {
            s = s * 1664525u + 1013904223u; // spread one seed over the streams (port choice)
            l->seed(s);
        }
    }
};

} // namespace parkan::fx
