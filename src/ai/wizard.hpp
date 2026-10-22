// The unit pilot: CWizard (Wizard.dll, iid 0x201, vtable 1000c190). Takt (10001d50) runs once per
// motion step, just before SelectNextMotion, and flies a cubic Hermite segment between timed route
// points: BeginSegment (10002c80), HermiteCubic::Init (10003d80), EvaluateSegment (100031a0),
// ComputeTurnCommand (100034c0), OutputToControl (10003750) → IControl SetLinearVelocity (body-local
// m/s) and SetAngularVelocity ({0, 0, yaw}, a −1..1 fraction of body[11]).
// Spec: notes/unit-ai-runtime.md §2.8.
//
// Not modelled: the network/replay queue at WZ+0x30, the "Wizard panic" point edits (slots 5–7),
// manual mode and per-channel overrides (slot 9): the wizard is always in AI mode unless `ai_mode`
// is cleared.
#pragma once

#include "core/math.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

namespace parkan::ai {

// WizardPoint (0x2c bytes). Flags: bit 0 = heading interpolated linearly; nibble (flags >> 4 + 4i) for
// body axis i: 1 = no negative values, 2 = no positive values, 3 = force 0, 4 (with 1 or 2) = ±max
// speed (runtime body[6 + i]).
struct WizardPoint {
    Vec3 pos{}, vel{}, heading{};
    std::uint32_t time = 0; // absolute game time (ms)
    std::uint32_t flags = 0;
};

// The two IControl requests of one Takt.
struct WizardOutput {
    Vec3 linear{}; // SetLinearVelocity
    float yaw = 0; // SetAngularVelocity({0, 0, yaw})
};

class Wizard {
public:
    // IWizard slots 4 / 8 / 17 / 18 / 19.
    std::size_t point_count() const { return route_.size(); }
    const WizardPoint& point(std::size_t i) const { return route_.at(i); }
    void add_point(const WizardPoint& p) { route_.push_back(p); }
    // Clears the route and the destination; t1 := the last tick time, so the next Takt starts a segment.
    void clear_points();
    // Ignored while a destination is set unless it moved by more than 0.5 m.
    void set_destination(const WizardPoint& p);
    // WZ+0x1a8: the time of the last tick message (the owner stamps it every game tick).
    void set_tick_time(std::uint32_t t) { tick_time_ = t; }

    // Takt at `now` = round(motion clock): `step_ms` is the duration of the motion step in progress
    // (ctl+0xf0, the look-ahead), `placement` the unit's matrix (forward = column 1), `body` the runtime
    // CTLD body. Returns the IControl requests, or nullopt when disabled / suspended / not in AI mode /
    // output disabled.
    std::optional<WizardOutput> takt(std::uint32_t now, float step_ms, const Mat4& placement, const std::array<float, 27>& body);

    bool disabled = false;       // WZ+0xc
    bool suspended = false;      // WZ+0x1f6
    bool ai_mode = true;         // WZ+0x1f7
    bool output_enabled = true;  // WZ+0x1f8 (Behaviour sets it to UseWizard = 1)

    // Segment state, for inspection.
    std::uint32_t segment_start() const { return t0_; }
    std::uint32_t segment_end() const { return target_.time; }
    const WizardPoint& segment_target() const { return target_; }
    Vec3 evaluated_position() const { return eval_pos_; }
    Vec3 evaluated_velocity() const { return eval_vel_; }
    Vec3 evaluated_heading() const { return eval_heading_; }

private:
    void begin_segment(std::uint32_t now);
    void evaluate_segment(std::uint32_t now);
    float compute_turn_command() const;
    WizardOutput output_to_control() const;

    std::deque<WizardPoint> route_;   // WZ+0x6c
    WizardPoint destination_{};       // WZ+0x8c
    bool has_destination_ = false;    // WZ+0xb8
    // Segment start p0 / v0 / d0 / t0 / flags (WZ+0xbc..+0xe4) and target (WZ+0xe8..+0x110).
    Vec3 p0_{}, v0_{}, d0_{};
    std::uint32_t t0_ = 0, start_flags_ = 0;
    WizardPoint target_{};
    Vec3 c2_{}, c3_{};   // position cubic
    Vec3 hc2_{}, hc3_{}; // heading cubic (p0 → p1 with tangents d0 → d1)
    Vec3 eval_pos_{}, eval_vel_{}, eval_heading_{};
    std::uint32_t active_flags_ = 0; // WZ+0x19c
    float seg_len_ = 0;              // WZ+0x1a0 T (s)
    float look_ahead_ = 0;           // WZ+0x1a4 L (s)
    std::array<float, 27> body_{};   // WZ+0x1ac
    Mat4 placement_ = Mat4::identity(); // WZ+0x1b0
    std::uint32_t tick_time_ = 0;    // WZ+0x1a8
};

} // namespace parkan::ai
