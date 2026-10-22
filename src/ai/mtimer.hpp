// MTimer (Behavior.dll, 8 bytes): SetPeriod 1004c4c0, Rearm 1004c500, Reset 1004c540, CheckAndRearm
// 1004c550. Spec: notes/unit-ai-runtime.md §1.2. Implemented in behaviour.cpp.
#pragma once

#include "ai/pathfind.hpp"

#include <cstdint>

namespace parkan::ai {

// base and jitter in units of 64 ms; fires at most once per `next`.
struct MTimer {
    std::uint32_t next = 0;
    std::uint16_t base = 0, jitter = 0;

    void set_period(float min_s, float rand_s); // SetPeriod 1004c4c0 (also clears next)
    void rearm(std::uint32_t now, const RandomSource& rand); // Rearm 1004c500
    bool check_and_rearm(std::uint32_t now, const RandomSource& rand); // CheckAndRearm 1004c550
    void reset() { next = 0; } // 1004c540: fires on the next check
};

} // namespace parkan::ai
