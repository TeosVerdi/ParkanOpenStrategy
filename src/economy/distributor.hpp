// The per-object resource state (attribute 0x2000100, the batteries of iid 0x204, the power devices) and
// a clan's Resource Distributor (Behavior CreateDistributor 10019da0, ctor 10019de0, vtable 10059794;
// Step 10019e80). Spec: notes/base-building.md §1.1–§1.3.
#pragma once

#include "ai/mtimer.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace parkan::economy {

// One object's economy state (MBehaviour fields named in base-building.md).
struct EcoState {
    std::uint32_t logic_id = 0xFFFFFFFF;
    std::uint32_t type = 0; // ObjectTypeCode (GetCreationParam)
    float ore = 0, ore_max = 0; // attribute 0x2000100: value = stored, aux = capacity
    // Batteries (iid 0x204): property 2 = Σ capacity of the type-0x13 devices, property 1 = charge fraction.
    float battery_cap = 0, battery_frac = 1.0f;
    // MPowerDevices: SetPowerUsage (10019880) sets device property 0x500; GetPowerFactor (100198e0) = pf.
    float power_usage = 0;
    float ore_offboard = 0, power_out = 0; // profession Transfer_Ore_OffBoard / Transfer_Power_Out
    float ore_left = 0;                    // property 0x105 (MB+0x9e8): ore left in a mine's lodes
    // [port approximation, base-building.md §8 step 1] Until Control's energy budget (1002d340) is ported:
    // pf = 1 while the batteries hold charge (or the building draws nothing), 0 when they are empty.
    float power_factor() const { return battery_cap <= 0 || battery_frac > 0 ? 1.0f : 0.0f; }
    // Property 0x1002: Transfer_Ore_OffBoard · pf.
    float ore_give_rate() const { return ore_offboard * power_factor(); }
};

// GetSummary (slot 9) → float[5] at +0x30.
struct DistributorSummary {
    float power_available = 0; // fPowerAvailable (G_p)
    float ore_available = 0;   // fOreAvailable (G_o)
    float ore_in_buffer = 0;   // fOreInBuffer (Σ ore of mines and storages)
    float power_usage = 0;     // fPowerUsage (W_p)
    float ore_usage = 0;       // fOreUsage (W_o)
};

struct DistributorRecord { // 0x1c bytes
    std::uint32_t logic_id = 0;
    float share = 0;
    bool locked = false;
    float power_want = 0, power_recv = 0, ore_want = 0, ore_recv = 0;
};

class Distributor {
public:
    // `resolve` maps a member's LogicID to its live state (nullptr = gone or clanless: the record is skipped).
    using Resolve = std::function<EcoState*(std::uint32_t)>;
    Distributor() = default;
    explicit Distributor(Resolve resolve) : resolve_(std::move(resolve)) {}

    void set_share(std::uint32_t lid, float s, int lock);       // slot 3 (1001a9a0)
    void add_equal_share(std::uint32_t lid);                    // slot 4 (1001aac0)
    const DistributorRecord* find(std::uint32_t lid) const;     // slot 5
    const std::vector<DistributorRecord>& records() const { return recs_; } // slots 6/7
    void remove(std::uint32_t lid);                             // slot 8 (1001abf0)
    const DistributorSummary& summary() const { return summary_; } // slot 9
    void reset_shares();                                        // slot 10
    // Slot 11 Takt (1001adb0): on the MTimer (base 3, random 1 → every 192–255 ms) runs Step(dt), dt =
    // elapsed ms · 0.001; the first call only primes the clock. True when a step ran.
    bool takt(std::uint32_t now_ms, const ai::RandomSource& rand);
    void step(float dt); // 10019e80
    bool stepped() const { return stepped_; }

private:
    void normalize(std::uint32_t keep); // 1001ac90
    DistributorRecord* rec(std::uint32_t lid);

    Resolve resolve_;
    std::vector<DistributorRecord> recs_;
    DistributorSummary summary_;
    ai::MTimer timer_{0, 3, 1};
    std::uint32_t last_ = 0;
    bool primed_ = false, stepped_ = false;
};

} // namespace parkan::economy
