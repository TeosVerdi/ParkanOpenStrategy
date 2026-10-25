// Default key layouts. The original's binding tables (.tbl) are compiled from the options-menu lists
// (.man) by World3D CreateTranslateTable, whose templates depend only on the command and the device
// class (key/button, mouse axis, joystick axis). Rebinding a key to another key or button therefore
// only changes the scan code in the compiled lines, so a layout is a scan-code substitution applied
// when a table is loaded (notes/input-system.md).
#pragma once

#include "input/man_tables.hpp"
#include "input/translate_table.hpp"

#include <filesystem>
#include <string_view>

namespace parkan::input {

enum class KeyLayout {
    Classic, // the 2001 tables as shipped, with keys a MacBook lacks moved (apply_mac_defaults)
    Modern,  // WASD movement at half throttle, Shift sprints, Option free look (default); docs/CONTROLS.md
};

// Rebinds a vehicle table (M1/M2/HERO.TBL) for `layout`. Accepts the shipped tables and tables that
// already carry the MacBook remap; idempotent.
void apply_layout(std::vector<Binding>& table, KeyLayout layout);

// The interface commands' keys (iron3d CKeyBindings table 3, addition.man), looked up as DispatchUIKeyMessage
// (10072060) does: a key press gives the command of the first row bound to that key without a modifier.
struct UiKey {
    std::uint16_t modifier = 0, key = 0;
};
class UiKeys {
public:
    UiKeys() = default;
    explicit UiKeys(ManTable rows) : rows_(std::move(rows)) {}
    // The key of `command` (e.g. "CMD_JAMES_OUTER_CAMERA"), for the texts that name it; nullopt when unbound.
    std::optional<UiKey> key(std::string_view command) const;
    // Whether pressing `scan` gives `command`.
    bool is(std::uint16_t scan, std::string_view command) const;

private:
    ManTable rows_;
};
// addition.man in `layout`. `custom`: the player saved bindings in the options menu, which are used as they
// are (only the keys a MacBook lacks move, apply_mac_defaults); otherwise the layout applies.
UiKeys ui_keys(ManTable addition, KeyLayout layout, bool custom);

// The mission's bindings as initGame (1005cbb0) makes them: addition.man, table_1.man and hero.man (the player's
// copies in `user_dir` when the options menu saved them, else the game folder's), compiled to M1.TBL / HERO.TBL
// (compile_man; no joystick) and laid out (apply_layout; with saved bindings only apply_mac_defaults).
// table_2.man (M2.TBL) is not loaded: nothing switches to it (CHANGE_TABLE is unbound) [port].
struct GameBindings {
    std::vector<Binding> hero, unit;
    UiKeys ui;
    bool custom = false; // the options menu's lists were used
};
GameBindings load_game_bindings(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                                KeyLayout layout);

} // namespace parkan::input
