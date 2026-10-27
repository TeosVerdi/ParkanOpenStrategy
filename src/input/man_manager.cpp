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

// The vehicle (class 0) has one set of channels: ExecuteBinding ignores INDEX for it (HERO.TBL's
// strafe-release lines carry index 1). FORWARD, BACK, WALK_F and WALK_B all read and write the single
// throttle.y (ExecuteBinding 1000fb40, GetProperty 0x24 / SetThrottle +0xc), so they share one channel: a walk-key
// release writes 0 and so also cancels a cruise speed.
static void vehicle_channel(std::uint32_t cls, std::int32_t& index, std::uint32_t& cmd) {
    if (cls != 0) return;
    index = 0;
    if (cmd == kMcmdBack || cmd == kMcmdWalkF || cmd == kMcmdWalkB) cmd = kMcmdForward;
}

float CommandState::get(std::uint32_t cls, std::int32_t index, std::uint32_t cmd) const {
    vehicle_channel(cls, index, cmd);
    auto it = values_.find({cls, index, cmd});
    return it == values_.end() ? (is_angle(cmd) ? 0.5f : 0.f) : it->second;
}

void CommandState::set(std::uint32_t cls, std::int32_t index, std::uint32_t cmd, float v) {
    vehicle_channel(cls, index, cmd);
    values_[{cls, index, cmd}] = v;
}

std::vector<Binding> CommandState::take_actions() { return std::exchange(actions_, {}); }

ManManager::ManManager(std::vector<Binding> table) { set_table(std::move(table)); }

void ManManager::set_table(std::vector<Binding> table) {
    live_.clear();
    walk_held_ = {};
    for (auto& b : table) {
        Live l{std::move(b)};
        // Axes are evaluated whenever their device moves; mouse buttons are edges like keys.
        if (l.b.device == Device::Mouse && (l.b.key == kScanMouseX || l.b.key == kScanMouseY)) l.active = 2;
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
        if (l.active == 2 || l.b.key != scan) continue;
        if (l.b.on_press == pressed) {
            if (modifier_ok(l)) activate(l, out);
        } else {
            l.active = 0; // the opposite edge ends a held or ramping binding
        }
    }
    // UpdateModifierBindings: a modifier changing state re-evaluates the press bindings of keys
    // that are still held, so Shift+key and key switch cleanly while the key stays down.
    for (auto& l : live_) {
        if (l.active == 2 || !l.b.on_press || l.b.key == scan || !held(l.b.key)) continue;
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
    float target = b.value;
    if (b.cls == 0 && (b.cmd == kMcmdWalkF || b.cmd == kMcmdWalkB)) {
        // WALK_F / WALK_B track which is held (manager +0x20 / +0x24): releasing one while the other is held sends a
        // hard -1.0 (releasing WALK_F, WALK_B held) or +1.0 (releasing WALK_B), whatever the bindings' values.
        const int me = b.cmd == kMcmdWalkB, other = 1 - me;
        walk_held_[me] = b.on_press;
        if (!b.on_press && walk_held_[other]) target = me == 0 ? -1.f : 1.f;
    }
    if (b.ramp_ms <= 0) { // one-shot: jump to the value
        cur = target;
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

// stdClearKeyboard 10011830 (and the focus-loss loop of stdSetApplicationState 100145f0): clears every held scan
// and queues a release event for it. With input enabled the release edges run now and the release bindings execute
// on the next process(); while input is disabled (+0x32 == 0) ExecuteBinding drops them, so nothing is released.
void ManManager::release_all(CommandState& out) {
    for (std::size_t scan = 0; scan < held_.size(); ++scan) {
        if (!held_[scan]) continue;
        held_[scan] = false;
        if (input_enabled_) on_edge(std::uint16_t(scan), false, out);
    }
}

// CManManager::DisableInput 1000f0a0: only +0x32 = 0.
void ManManager::disable_input() { input_enabled_ = false; }

// CManManager::EnableInput 1000f0b0: zeroes the walk-held flags (+0x20/+0x24) and the case-4 latches, then
// ResetBindings 1000eb70 zeroes the shared turn globals and stops every ramp (active == 1; analog stays), then
// +0x32 = 1. It does not touch the unit's throttle. [inferred: the ROTATE_Z latches (+0x28/+0x29/+0x2c) and the turn
// globals have no port counterpart]
void ManManager::enable_input() {
    walk_held_ = {};
    for (auto& l : live_)
        if (l.active == 1) l.active = 0;
    input_enabled_ = true;
}

// Menu transitions (ViewTrans_*ToMenu: DisableInput, stdClearKeyboard; ViewTrans_MenuTo*: EnableInput, stdClearKeyboard).
bool ManManager::sync_menu(bool open, CommandState& out) {
    if (open == menu_open_) return false;
    menu_open_ = open;
    if (open) disable_input();
    else enable_input();
    release_all(out);
    return true;
}

void ManManager::process(std::span<const InputEvent> events, float mouse_dx, float mouse_dy, float dt_ms, CommandState& out) {
    if (!input_enabled_) return; // ExecuteBinding returns at once while +0x32 == 0
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
