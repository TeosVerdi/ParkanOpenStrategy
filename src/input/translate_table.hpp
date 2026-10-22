// Vehicle-control binding tables (M1.TBL, M2.TBL, HERO.TBL): World3D LoadTranslateTable (1000b4d0).
// Spec: notes/input-system.md ".tbl line format".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace parkan::input {

enum class Device : std::uint8_t { Key = 0, Mouse = 1, Joy = 2 };

struct Binding {
    Device device = Device::Key;
    std::uint16_t modifier = 0; // scan that must be held; 0 = none
    std::uint16_t key = 0;
    bool on_press = true;       // false: fires on release
    std::uint32_t cls = 0;      // CICLS_*: 0 the vehicle, 1 turret, 2 multigun, ...
    std::uint32_t cmd = 0;      // MCMD_*
    float value = 0;
    std::int32_t index = 0;     // n-th component (1-based), -1 = all / selected
    std::int32_t state = 0;     // CIS_* flag, MAN_WRAP 0 / MAN_NOTWRAP 1, or a number
    float accel = 0;            // ramp step; 0 = jump to value
    std::int32_t ramp_ms = 0;   // 0 = one-shot
    std::string comment;
};

// MCMD ids (World3D ParseManualCmd 1000d7b0).
enum Mcmd : std::uint32_t {
    kMcmdDummy = 0, kMcmdState = 1, kMcmdRotateX = 2, kMcmdRotateY = 3, kMcmdRotateZ = 4, kMcmdAngleX = 5,
    kMcmdAngleY = 6, kMcmdForward = 7, kMcmdBack = 8, kMcmdLeft = 9, kMcmdRight = 10, kMcmdUp = 0xb,
    kMcmdDown = 0xc, kMcmdSelect = 0xd, kMcmdSelectNext = 0xe, kMcmdTable = 0xf, kMcmdAngleZ = 0x10,
    kMcmdMissile = 0x11, kMcmdFireAll = 0x12, kMcmdWalkF = 0x13, kMcmdWalkB = 0x14, kMcmdLock = 0x15,
};

// Parses a .tbl file (cp1251 text). Lines not starting with KEY/MOUSE/JOY (comments, "not
// implemented" notes) are skipped; an unknown name in a binding line throws FormatError.
std::vector<Binding> parse_translate_table(std::string_view text);

// MacBook defaults: keys a MacBook lacks move to main-block keys (same mapping as
// tools/mac_keymap.py, which applied it to the CrossOver install): keypad 5 → L, gray + → =,
// gray - → -, gray * → \, gray / → Backspace, Page Up → ', Page Down → /. Idempotent.
void apply_mac_defaults(std::vector<Binding>& table);

} // namespace parkan::input
