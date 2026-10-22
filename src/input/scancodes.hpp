// The original's input codes (World3D WinMsg2ScanCode, notes/input-system.md): PC set-1 scan codes
// (+0x100 for extended keys), mouse axes/buttons and joystick inputs, with their SCAN_* names.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace parkan::input {

constexpr std::uint16_t kScanNull = 0;
constexpr std::uint16_t kScanMouseX = 0x1fe, kScanMouseY = 0x1ff;
constexpr std::uint16_t kScanMouseLeft = 0x200, kScanMouseRight = 0x201, kScanMouseMiddle = 0x202;

std::optional<std::uint16_t> scan_from_name(std::string_view name);
std::string_view scan_name(std::uint16_t code); // empty when unnamed

// USB HID keyboard usage (= SDL_Scancode) → scan code; kScanNull for keys the original cannot see
// (Command/Windows keys). Alt/Option, which the original ignored (WM_SYSKEYDOWN), maps to the
// port-added SCAN_LALT/SCAN_RALT.
std::uint16_t scan_from_hid(int usage);

} // namespace parkan::input
