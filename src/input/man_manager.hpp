// Manual control: World3D CManManager::Process (1000f100) and ComputeManualValue (10010a50) — turns
// input events into command values through a binding table. Spec: notes/input-system.md
// "Runtime (per frame)". ExecuteBinding (1000fb40) is reduced to storing each (class, index,
// command) channel's value; the vehicle reads the channels it understands.
#pragma once

#include "input/translate_table.hpp"

#include <array>
#include <map>
#include <span>
#include <tuple>

namespace parkan::input {

struct InputEvent {
    bool pressed = false;
    std::uint16_t scan = 0;
};

// Current value of every command channel. ANGLE_* are absolute 0..1 (MAN_WRAP wraps, MAN_NOTWRAP
// clamps; default 0.5 = centred); the others are -1..1. WALK_F and WALK_B are separate channels
// that the vehicle sums, so they cancel when both are held.
class CommandState {
public:
    float get(std::uint32_t cls, std::int32_t index, std::uint32_t cmd) const;
    void set(std::uint32_t cls, std::int32_t index, std::uint32_t cmd, float v);
    // One-shot actions (SELECT, TABLE, LOCK, STATE, ...) since the last take_actions().
    std::vector<Binding> take_actions();
    void push_action(const Binding& b) { actions_.push_back(b); }

private:
    std::map<std::tuple<std::uint32_t, std::int32_t, std::uint32_t>, float> values_;
    std::vector<Binding> actions_;
};

class ManManager {
public:
    explicit ManManager(std::vector<Binding> table);
    void set_table(std::vector<Binding> table);

    void set_mouse_sensitivity(float s) { mouse_sens_ = s; }
    void set_inverse(bool mouse_x, bool mouse_y) { inv_x_ = mouse_x ? -1.f : 1.f, inv_y_ = mouse_y ? -1.f : 1.f; }

    // One frame: `events` in order, raw mouse deltas accumulated since the last call, the frame
    // length in ms. Results go to `out`.
    void process(std::span<const InputEvent> events, float mouse_dx, float mouse_dy, float dt_ms, CommandState& out);

    bool held(std::uint16_t scan) const { return scan < held_.size() && held_[scan]; }

private:
    struct Live {
        Binding b;
        int active = 0;       // 0 off, 1 runs every frame (ramps), 2 analog
        float elapsed_ms = 0; // since activation (ramps)
        float start = 0;      // channel value at activation
    };
    void activate(Live& l, const CommandState& out);
    void execute(Live& l, float dt_ms, CommandState& out);
    bool modifier_ok(const Live& l) const;
    void on_edge(std::uint16_t scan, bool pressed, const CommandState& out);

    std::vector<Live> live_;
    std::array<bool, 0x300> held_{};
    float smooth_x_ = 0, smooth_y_ = 0, acc_x_ = 0, acc_y_ = 0, acc_ms_ = 0;
    float mouse_sens_ = 1, inv_x_ = 1, inv_y_ = 1;
};

} // namespace parkan::input
