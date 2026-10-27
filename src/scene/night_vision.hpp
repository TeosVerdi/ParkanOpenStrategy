// Infrared / night-vision colour math and toggle rules (Terrain.dll; spec: notes/infrared-camera.md).
// The camera flag 0x20 reaches the renderer as the CShader night-vision flag for that camera's scene;
// every colour then goes through NV(r, g, b) = (0, (r + g + b)·0.33·7, 0), alpha kept. Pure functions, no
// platform dependencies.
#pragma once

#include "core/math.hpp"

#include <cstdint>

namespace parkan {

// Terrain constants 0.33 (100652e0) and 7.0 (100652dc).
constexpr float kNvThird = 0.33f, kNvGain = 7.f;

// ConvertToNightVisionColor 1002f410 (IShader slot 0x14), packed ARGB: G = min(255, round((R+G+B)·0.33·7)),
// R = B = 0, alpha kept (x87 round-to-nearest-even). Used for the sky dome, the fog colour and effect sprites.
std::uint32_t night_vision_color(std::uint32_t argb);
// The same for a 0..1 colour (quantised to bytes through the packed form).
Vec3 night_vision_rgb(Vec3 rgb);

// ShadeIndexedStrided 1002d570 / 1002e4a0: material colour quads and the merged ambient, not clamped
// (white → G = 6.93). Light colours are not converted.
Vec3 night_vision_material(Vec3 rgb);
// The green gain a white material gets (the lit path multiplies the light's green by it).
inline float night_vision_white_gain() { return night_vision_material({1, 1, 1}).y; }

// The lit path for a white material: the light accumulation Σ ndl·lightColour (light colours unconverted) times the
// converted material G' leaves only the green channel. Mirrored by the scene shader (vs_main, fog.z / fog.w).
inline Vec3 night_vision_lit_white(Vec3 light_sum) { return {0.f, light_sum.y * night_vision_white_gain(), 0.f}; }

// Camera flag bit 0x20 (CCamera+0x164; Camera_IsInfrared 10035da0).
constexpr std::uint32_t kCameraInfrared = 0x20;

// UI command 0x23 CMD_CAMERA_INFRARED (OnUICommandDown 10072212): only in view mode 2 (HQ) or 3 (outer).
bool night_vision_command_allowed(int view_mode);
// CCameraDevice::SetState 10023a00: needs the camera object and the owner control's replay lock (+0x618) clear.
bool night_vision_device_allowed(bool has_camera, bool replay_lock);
// The state's 0xf000 bits: 0x1000 set, 0x2000 clear, 0x4000 toggle; anything else leaves the flag.
bool night_vision_apply_state(bool on, std::uint32_t state);
// FUN_100a52b0: VOICE_INFRA_SYS_ON / _OFF are queued only in view modes 1, 3, 6.
bool night_vision_voice_mode(int view_mode);

// The per-frame edge detector (byte mission+0xaf4): returns +1 / -1 when the active camera's flag changed to
// on / off and the voice is allowed in `view_mode`, else 0. The latch follows the flag in every mode [inferred].
struct NightVisionVoice {
    bool last = false;
    int update(bool on, int view_mode);
};

} // namespace parkan
