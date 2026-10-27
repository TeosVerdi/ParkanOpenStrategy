#include "scene/night_vision.hpp"

#include <algorithm>
#include <cmath>

namespace parkan {

std::uint32_t night_vision_color(std::uint32_t argb) {
    const double sum = double((argb >> 16) & 0xFF) + double((argb >> 8) & 0xFF) + double(argb & 0xFF);
    const double g = std::nearbyint(sum * double(kNvThird) * double(kNvGain));
    return (argb & 0xFF000000u) | (std::uint32_t(std::min(255.0, g)) << 8);
}

Vec3 night_vision_rgb(Vec3 rgb) {
    auto byte = [](float v) { return std::uint32_t(std::clamp(std::lround(v * 255.f), 0l, 255l)); };
    const std::uint32_t c = night_vision_color(0xFF000000u | byte(rgb.x) << 16 | byte(rgb.y) << 8 | byte(rgb.z));
    return {0.f, float((c >> 8) & 0xFF) / 255.f, 0.f};
}

Vec3 night_vision_material(Vec3 rgb) { return {0.f, (rgb.x + rgb.y + rgb.z) * kNvThird * kNvGain, 0.f}; }

bool night_vision_command_allowed(int view_mode) { return view_mode == 2 || view_mode == 3; }

bool night_vision_device_allowed(bool has_camera, bool replay_lock) { return has_camera && !replay_lock; }

bool night_vision_apply_state(bool on, std::uint32_t state) {
    if (state & 0x1000) return true;
    if (state & 0x2000) return false;
    if (state & 0x4000) return !on;
    return on;
}

bool night_vision_voice_mode(int view_mode) { return view_mode == 1 || view_mode == 3 || view_mode == 6; }

int NightVisionVoice::update(bool on, int view_mode) {
    const bool changed = on != last;
    last = on;
    return changed && night_vision_voice_mode(view_mode) ? (on ? 1 : -1) : 0;
}

} // namespace parkan
