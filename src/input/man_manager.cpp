#include "input/man_manager.hpp"

#include "input/scancodes.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::input {

namespace {

bool is_angle(std::uint32_t cmd) { return cmd == kMcmdAngleX || cmd == kMcmdAngleY || cmd == kMcmdAngleZ; }

// Commands whose binding is an action rather than a value (ExecuteBinding calls a method once).
bool is_action(std::uint32_t cmd) {
    return cmd == kMcmdState || cmd == kMcmdSelect || cmd == kMcmdSelectNext || cmd == kMcmdTable || cmd == kMcmdMissile ||
           cmd == kMcmdFireAll || cmd == kMcmdLock || cmd == kMcmdDummy;
}

float wrap01(float v) { return v - std::floor(v); }

} // namespace

float CommandState::get(std::uint32_t cls, std::int32_t index, std::uint32_t cmd) const {
    auto it = values_.find({cls, index, cmd});
    return it == values_.end() ? (is_angle(cmd) ? 0.5f : 0.f) : it->second;
}

void CommandState::set(std::uint32_t cls, std::int32_t index, std::uint32_t cmd, float v) { values_[{cls, index, cmd}] = v; }

std::vector<Binding> CommandState::take_actions() { return std::exchange(actions_, {}); }

ManManager::ManManager(std::vector<Binding> table) { set_table(std::move(table)); }

void ManManager::set_table(std::vector<Binding> table) {
    live_.clear();
    for (auto& b : table) {
        Live l{std::move(b)};
        if (l.b.device != Device::Key) l.active = 2; // axes are evaluated whenever their device moves
        live_.push_back(std::move(l));
    }
}

// An unmodified binding is suppressed while any modifier of another binding on the same key and
// edge is held; a modified one needs its modifier held.
bool ManManager::modifier_ok(const Live& l) const {
    if (l.b.modifier) return held(l.b.modifier);
    for (const auto& o : live_)
        if (&o != &l && o.b.modifier && o.b.key == l.b.key && o.b.on_press == l.b.on_press && o.b.device == l.b.device &&
            held(o.b.modifier))
            return false;
    return true;
}

void ManManager::activate(Live& l, const CommandState& out) {
    l.active = 1;
    l.elapsed_ms = 0;
    l.start = out.get(l.b.cls, l.b.index, l.b.cmd);
}

void ManManager::on_edge(std::uint16_t scan, bool pressed, const CommandState& out) {
    for (auto& l : live_) {
        if (l.b.device != Device::Key || l.b.key != scan) continue;
        if (l.b.on_press == pressed) {
            if (modifier_ok(l)) activate(l, out);
        } else {
            l.active = 0; // the opposite edge ends a held or ramping binding
        }
    }
    // UpdateModifierBindings: a modifier changing state re-evaluates the press bindings of keys
    // that are still held, so Shift+key and key switch cleanly while the key stays down.
    for (auto& l : live_) {
        if (l.b.device != Device::Key || !l.b.on_press || l.b.key == scan || !held(l.b.key)) continue;
        const bool related = l.b.modifier == scan || std::any_of(live_.begin(), live_.end(), [&](const Live& o) {
                                 return o.b.key == l.b.key && o.b.modifier == scan;
                             });
        if (!related) continue;
        const bool ok = modifier_ok(l);
        if (ok && l.active == 0) activate(l, out);
        if (!ok) l.active = 0;
    }
}

void ManManager::execute(Live& l, float dt_ms, CommandState& out) {
    const Binding& b = l.b;
    if (is_action(b.cmd)) {
        out.push_action(b);
        l.active = 0;
        return;
    }
    float cur = out.get(b.cls, b.index, b.cmd);
    if (b.ramp_ms <= 0) { // one-shot: jump to the value
        cur = b.value;
        l.active = 0;
    } else {
        // ComputeManualValue: move toward VALUE by ACCEL·min(1, elapsed/RAMP_MS) per frame; done within 0.01.
        l.elapsed_ms += dt_ms;
        const float step = b.accel * std::min(1.f, l.elapsed_ms / float(b.ramp_ms));
        if (is_angle(b.cmd)) {
            cur += b.value < 0 ? -step : step; // a held turret/camera key keeps turning
        } else {
            cur += std::clamp(b.value - cur, -step, step);
            if (std::abs(cur - b.value) < 0.01f) cur = b.value, l.active = 0;
        }
    }
    if (is_angle(b.cmd)) cur = b.state == 1 ? std::clamp(cur, 0.f, 1.f) : wrap01(cur);
    else cur = std::clamp(cur, -1.f, 1.f);
    out.set(b.cls, b.index, b.cmd, cur);
}

void ManManager::process(std::span<const InputEvent> events, float mouse_dx, float mouse_dy, float dt_ms, CommandState& out) {
    for (const auto& e : events) {
        if (e.scan >= held_.size()) continue;
        if (e.pressed && held_[e.scan]) continue; // auto-repeat is ignored
        held_[e.scan] = e.pressed;
        on_edge(e.scan, e.pressed, out);
    }

    // Mouse smoothing: deltas are accumulated over frames shorter than 26 ms, then
    // smooth = smooth·0.05 + delta·sensitivity·0.95 (Y also ×1.2).
    acc_x_ += mouse_dx, acc_y_ += mouse_dy, acc_ms_ += dt_ms;
    bool moved = false;
    if (acc_ms_ >= 26.f) {
        moved = acc_x_ != 0 || acc_y_ != 0 || smooth_x_ != 0 || smooth_y_ != 0;
        smooth_x_ = smooth_x_ * 0.05f + acc_x_ * mouse_sens_ * 0.95f;
        smooth_y_ = smooth_y_ * 0.05f + acc_y_ * mouse_sens_ * 0.95f * 1.2f;
        if (std::abs(smooth_x_) < 1e-4f) smooth_x_ = 0;
        if (std::abs(smooth_y_) < 1e-4f) smooth_y_ = 0;
        acc_x_ = acc_y_ = acc_ms_ = 0;
    }

    for (auto& l : live_) {
        if (l.active == 2) {
            if (!moved || l.b.device != Device::Mouse || !modifier_ok(l)) continue;
            const bool x = l.b.key == kScanMouseX;
            if (!x && l.b.key != kScanMouseY) continue; // mouse buttons are keys
            const float v = std::clamp((x ? smooth_x_ : smooth_y_) * 0.006f, -1.f, 1.f) * (x ? inv_x_ : inv_y_) * l.b.value;
            float cur = out.get(l.b.cls, l.b.index, l.b.cmd);
            if (is_angle(l.b.cmd)) cur = l.b.state == 1 ? std::clamp(cur + v, 0.f, 1.f) : wrap01(cur + v);
            else cur = v;
            out.set(l.b.cls, l.b.index, l.b.cmd, cur);
        } else if (l.active == 1) {
            execute(l, dt_ms, out);
        }
    }
}

} // namespace parkan::input
