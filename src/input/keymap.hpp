// Default key layouts. The original's binding tables (.tbl) are compiled from the options-menu lists
// (.man) by World3D CreateTranslateTable, whose templates depend only on the command and the device
// class (key/button, mouse axis, joystick axis). Rebinding a key to another key or button therefore
// only changes the scan code in the compiled lines, so a layout is a scan-code substitution applied
// when a table is loaded (notes/input-system.md).
#pragma once

#include "input/translate_table.hpp"

#include <string_view>

namespace parkan::input {

enum class KeyLayout {
    Classic, // the 2001 tables as shipped, with keys a MacBook lacks moved (apply_mac_defaults)
    Modern,  // WASD movement at half throttle, Shift sprints, Option free look (default); docs/CONTROLS.md
};

// Rebinds a vehicle table (M1/M2/HERO.TBL) for `layout`. Accepts the shipped tables and tables that
// already carry the MacBook remap; idempotent.
void apply_layout(std::vector<Binding>& table, KeyLayout layout);

// Interface commands (iron3d CKeyBindings, ui_*.man): the scan bound to `command` (e.g.
// "CMD_JAMES_OUTER_CAMERA") in `layout`, and the modifier (0 = none); nullopt when unbound.
struct UiKey {
    std::uint16_t modifier = 0, key = 0;
};
std::optional<UiKey> ui_key(std::string_view command, KeyLayout layout);

} // namespace parkan::input
