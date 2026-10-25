// The options-menu binding lists (.man) and what the game makes of them (notes/input-system.md,
// notes/shell.md "Redefine controls"):
//   - iron3d CKeyBindings: a .man file is a list of {command, modifier scan, key scan} rows
//     (CKeyBindings_ParseManFile 1003a5f0, ParseManLine 1003a2c0), written back by
//     SaveManAndBuildTable 1003ad90 ("%s %s %s\n" with the canonical names).
//   - The shell's OK (CFormRedefineControls_BuildGameTables 10004fb0) merges the four tab lists into
//     addition.man, table_2.man, table_1.man and hero.man.
//   - initGame (1005cbb0) loads those four, rewrites them and compiles table_1 → M1.TBL, table_2 → M2.TBL,
//     hero → HERO.TBL with World3D CreateTranslateTable (10011930); addition.man stays the UI list that
//     DispatchUIKeyMessage (10072060) looks keys up in (CKeyBindings::Lookup 1003b150).
#pragma once

#include "input/translate_table.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parkan::input {

struct ManRow {
    std::uint32_t cmd = 0;      // World3D manual command 1–0x42 or iron3d UI command 0x2d3–0x2f2 (0x23 is both)
    std::uint16_t modifier = 0; // scan that must be held; 0 = none
    std::uint16_t key = 0;
};
using ManTable = std::vector<ManRow>;

// ConvertCommand (World3D 1000d9e0), else ConvertUICommand (iron3d 10039da0); case-insensitive.
std::optional<std::uint32_t> man_command(std::string_view name);
// ManualCommand2Str, else UICommand2Str; empty for an unknown id.
std::string_view man_command_name(std::uint32_t cmd);

// CKeyBindings_ParseManFile: `CMD [KEY|JOY|MOUSE] MODIFIER KEY` per line; reading stops at the first line
// that does not parse (an unknown command or scan name, a missing field, a blank line). Afterwards the key
// axes follow the joystick setting: with `joystick` MOUSE_X/Y become JOY_X/Y, without it the reverse.
ManTable parse_man(std::string_view text, bool joystick);
// SaveManAndBuildTable's file: "CMD MODIFIER KEY\n" per row.
std::string write_man(const ManTable& rows);

// World3D CreateTranslateTable: the .tbl text for a .man text (`hero`: the target is HERO.TBL, which has its
// own strafe-release and turn templates).
std::string create_translate_table(std::string_view man_text, bool hero);

// What initGame does with one vehicle list: parse, rewrite, compile, load.
std::vector<Binding> compile_man(std::string_view man_text, bool hero, bool joystick);

// BuildGameTables: the four in-game lists from the shell's tab lists (0 other, 1 hq, 2 bots, 3 hero), in
// the order {addition, table_2, table_1, hero}. Each entry copies the first row of its source tab with
// that command; a command missing from the tab is left out [inferred].
std::array<ManTable, 4> build_game_tables(const std::array<ManTable, 4>& tabs);
inline constexpr const char* kGameTableFiles[4] = {"addition.man", "table_2.man", "table_1.man", "hero.man"};

// CKeyBindings::Lookup(table, modifier 0, scan): the command of the first row bound to `scan` without a
// modifier (a row with a modifier never matches here).
std::optional<std::uint32_t> ui_lookup(const ManTable& rows, std::uint16_t scan);

// apply_mac_defaults for a list (modifiers and keys).
void apply_mac_defaults(ManTable& rows);

} // namespace parkan::input
