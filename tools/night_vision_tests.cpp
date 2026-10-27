// Infrared / night-vision camera tests (notes/infrared-camera.md §7): the colour transforms with the spec's worked
// values, the sky conversion, and the toggle and voice rules. Linked into parkan-tests.
#include "scene/night_vision.hpp"
#include "scene/sky.hpp"

#include <cmath>
#include <cstdio>

using namespace parkan;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_fail;                                                       \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void test_colours() {
    CHECK(night_vision_color(0xFFFFFFFFu) == 0xFF00FF00u);  // clamped
    CHECK(night_vision_color(0x80102030u) == 0x8000DE00u);  // 96·0.33·7 = 221.76 → 222
    CHECK(night_vision_color(0x00010101u) == 0x00000700u);  // 6.93 → 7
    CHECK(night_vision_color(0x7F000000u) == 0x7F000000u);  // black stays black, alpha kept
    const Vec3 white = night_vision_material({1, 1, 1});
    CHECK(white.x == 0 && white.z == 0 && near(white.y, 6.93f));  // not clamped
    const Vec3 amb = night_vision_material({0.2f, 0.2f, 0.2f});
    CHECK(amb.x == 0 && amb.z == 0 && near(amb.y, 1.386f));
    const Vec3 f = night_vision_rgb({1, 1, 1});
    CHECK(f.x == 0 && f.y == 1 && f.z == 0);
}

void test_lighting() {
    CHECK(near(night_vision_white_gain(), 6.93f));
    const Vec3 red = night_vision_lit_white({1, 0, 0});  // a pure red light lights nothing
    CHECK(red.x == 0 && red.y == 0 && red.z == 0);
    const Vec3 w = night_vision_lit_white({1, 1, 1});  // a white light: green only
    CHECK(w.x == 0 && w.z == 0 && near(w.y, 6.93f));
}

void test_sky() {
    SkyDome d = build_sky_dome(16);
    for (auto& v : d.vertices) v.color[0] = 0x10, v.color[1] = 0x20, v.color[2] = 0x30, v.color[3] = 0x80;
    night_vision_dome(d);
    CHECK(d.vertices[0].color[0] == 0 && d.vertices[0].color[1] == 0xDE && d.vertices[0].color[2] == 0 && d.vertices[0].color[3] == 0x80);
    CHECK(d.vertices.back().color[1] == 0xDE);
    SceneLighting l;
    l.fog_color = {0.5f, 0.5f, 0.5f};
    night_vision_lighting(l);
    CHECK(l.night_vision && l.fog_color.x == 0 && l.fog_color.z == 0 && near(l.fog_color.y, 1.f));
    CHECK(l.clear_color.y == l.fog_color.y && l.clear_color.x == 0);
    CHECK(kNightVisionNebulaTint[0] == 0 && kNightVisionNebulaTint[1] == 1 && kNightVisionNebulaTint[2] == 0);
}

void test_toggles() {
    for (int m : {1, 4, 5, 6}) CHECK(!night_vision_command_allowed(m));
    CHECK(night_vision_command_allowed(2) && night_vision_command_allowed(3));
    CHECK(night_vision_device_allowed(true, false));
    CHECK(!night_vision_device_allowed(true, true));  // replay lock
    CHECK(!night_vision_device_allowed(false, false)); // no camera object
    CHECK(night_vision_apply_state(false, 0x1000) && night_vision_apply_state(true, 0x1000));
    CHECK(!night_vision_apply_state(true, 0x2000) && !night_vision_apply_state(false, 0x2000));
    CHECK(night_vision_apply_state(false, 0x4000) && !night_vision_apply_state(true, 0x4000));
    CHECK(night_vision_apply_state(night_vision_apply_state(true, 0x4000), 0x4000));  // twice: original
    CHECK(night_vision_apply_state(true, 0x0100) && !night_vision_apply_state(false, 0x0100));  // not a camera state
}

void test_voice() {
    NightVisionVoice v;
    CHECK(v.update(false, 1) == 0);
    CHECK(v.update(true, 1) == 1);
    CHECK(v.update(true, 1) == 0);   // no change, no voice
    CHECK(v.update(false, 3) == -1);
    CHECK(v.update(true, 2) == 0);   // changed in HQ view: silent
    CHECK(v.update(true, 6) == 0);   // and the latch already followed
    CHECK(v.update(false, 6) == -1);
    CHECK(v.update(true, 4) == 0 && v.update(false, 5) == 0);
}

} // namespace

int night_vision_unit_tests() {
    g_fail = 0;
    std::printf("night vision\n");
    test_colours();
    test_lighting();
    test_sky();
    test_toggles();
    test_voice();
    return g_fail;
}
