#include "input/keymap.hpp"

#include "input/scancodes.hpp"

#include <map>

namespace parkan::input {

namespace {

std::uint16_t sc(const char* name) { return scan_from_name(name).value(); }

// Modern layout as a substitution from the shipped keys and from their MacBook-remapped forms
// (mac_keymap.py / apply_mac_defaults). Applied simultaneously: LSHIFT (free-look modifier) moves to
// Option while `\` (cruise at full speed) moves to C; Shift becomes the sprint modifier below.
const std::map<std::uint16_t, std::uint16_t>& modern_vehicle_map() {
    static const std::map<std::uint16_t, std::uint16_t> m = {
        {sc("SCAN_G_UP"), sc("SCAN_W")},         {sc("SCAN_G_DOWN"), sc("SCAN_S")},
        {sc("SCAN_G_LEFT"), sc("SCAN_A")},       {sc("SCAN_G_RIGHT"), sc("SCAN_D")},
        {sc("SCAN_COMMA"), sc("SCAN_Q")},        {sc("SCAN_DOT"), sc("SCAN_E")},
        {sc("SCAN_G_PGUP"), sc("SCAN_BLANK")},   {sc("SCAN_QUOTE"), sc("SCAN_BLANK")},
        {sc("SCAN_G_PGDN"), sc("SCAN_LCTRL")},   {sc("SCAN_SLASH"), sc("SCAN_LCTRL")},
        {sc("SCAN_G_ASTERISK"), sc("SCAN_C")},   {sc("SCAN_BSLASH"), sc("SCAN_C")},
        {sc("SCAN_G_SLASH"), sc("SCAN_X")},      {sc("SCAN_BS"), sc("SCAN_X")},
        {sc("SCAN_G_PLUS"), sc("SCAN_W_PLUS")},  {sc("SCAN_G_SUB"), sc("SCAN_W_SUB")},
        {sc("SCAN_G_5"), sc("SCAN_L")},
        {sc("SCAN_LSHIFT"), sc("SCAN_LALT")},
    };
    return m;
}

struct UiEntry {
    const char* command;
    const char* classic_mod;
    const char* classic_key;
    const char* modern_mod;
    const char* modern_key;
};

// ui_other_d.man / addition.man / ui_hq_d.man defaults (classic) and the modern layout.
constexpr UiEntry kUi[] = {
    {"CMD_JAMES_HQ_MOVE_FORWARD", "SCAN_NULL", "SCAN_G_UP", "SCAN_NULL", "SCAN_W"},
    {"CMD_JAMES_HQ_MOVE_BACKWARD", "SCAN_NULL", "SCAN_G_DOWN", "SCAN_NULL", "SCAN_S"},
    {"CMD_JAMES_HQ_MOVE_LEFT", "SCAN_NULL", "SCAN_G_LEFT", "SCAN_NULL", "SCAN_A"},
    {"CMD_JAMES_HQ_MOVE_RIGHT", "SCAN_NULL", "SCAN_G_RIGHT", "SCAN_NULL", "SCAN_D"},
    {"CMD_JAMES_HQ_MOVE_UP", "SCAN_NULL", "SCAN_QUOTE", "SCAN_NULL", "SCAN_BLANK"}, // classic: MacBook form of PgUp
    {"CMD_JAMES_HQ_MOVE_DOWN", "SCAN_NULL", "SCAN_SLASH", "SCAN_NULL", "SCAN_LCTRL"},
    {"CMD_JAMES_BASE_ROTLEFT", "SCAN_NULL", "SCAN_COMMA", "SCAN_NULL", "SCAN_Q"},
    {"CMD_JAMES_BASE_ROTRIGHT", "SCAN_NULL", "SCAN_DOT", "SCAN_NULL", "SCAN_E"},
    {"CMD_ENTER_STATE", "SCAN_NULL", "SCAN_W_ENTER", "SCAN_NULL", "SCAN_W_ENTER"},
    {"CMD_ROLLBACK_STATE", "SCAN_NULL", "SCAN_ESC", "SCAN_NULL", "SCAN_ESC"},
    {"CMD_JAMES_SELECT_TARGET", "SCAN_NULL", "SCAN_TAB", "SCAN_NULL", "SCAN_TAB"},
    {"CMD_JAMES_SELECT_ENEMY", "SCAN_NULL", "SCAN_E", "SCAN_NULL", "SCAN_G"},
    {"CMD_JAMES_SELECT_FRIEND", "SCAN_NULL", "SCAN_F", "SCAN_NULL", "SCAN_F"},
    {"CMD_JAMES_AIM_TARGET", "SCAN_NULL", "SCAN_RMOUSE", "SCAN_NULL", "SCAN_T"},
    {"CMD_JAMES_ZOOM_MODE", "SCAN_NULL", "SCAN_Z", "SCAN_NULL", "SCAN_RMOUSE"},
    {"CMD_JAMES_WINGMAN_MENU", "SCAN_NULL", "SCAN_TILDA", "SCAN_NULL", "SCAN_TILDA"},
    {"CMD_JAMES_AUTO_DRIVER", "SCAN_NULL", "SCAN_A", "SCAN_NULL", "SCAN_P"},
    {"CMD_JAMES_OUTER_CAMERA", "SCAN_NULL", "SCAN_C", "SCAN_NULL", "SCAN_V"},
    {"CMD_JAMES_COCKPIT_OFF", "SCAN_NULL", "SCAN_U", "SCAN_NULL", "SCAN_U"},
    {"CMD_JAMES_SATELLITE_MAP", "SCAN_NULL", "SCAN_M", "SCAN_NULL", "SCAN_M"},
    {"CMD_DEC_MAP_ALPHA", "SCAN_NULL", "SCAN_LBRACKET", "SCAN_NULL", "SCAN_LBRACKET"},
    {"CMD_INC_MAP_ALPHA", "SCAN_NULL", "SCAN_RBRACKET", "SCAN_NULL", "SCAN_RBRACKET"},
    {"CMD_HELP", "SCAN_NULL", "SCAN_F1", "SCAN_NULL", "SCAN_F1"},
    {"CMD_PAGER", "SCAN_NULL", "SCAN_F2", "SCAN_NULL", "SCAN_F2"},
    {"CMD_GAME_MENU", "SCAN_NULL", "SCAN_F3", "SCAN_NULL", "SCAN_F3"},
    {"CMD_TERMINAL", "SCAN_NULL", "SCAN_F4", "SCAN_NULL", "SCAN_F4"},
    {"CMD_TERMINAL_NEW_MESSAGE", "SCAN_NULL", "SCAN_F5", "SCAN_NULL", "SCAN_Y"},
    {"CMD_JAMES_MISSION_OBJ", "SCAN_NULL", "SCAN_F12", "SCAN_NULL", "SCAN_O"},
    {"CMD_QUICK_SAVE", "SCAN_NULL", "SCAN_F7", "SCAN_NULL", "SCAN_F5"},
    {"CMD_QUICK_LOAD", "SCAN_NULL", "SCAN_F8", "SCAN_NULL", "SCAN_F9"},
};

} // namespace

void apply_layout(std::vector<Binding>& table, KeyLayout layout) {
    apply_mac_defaults(table);
    if (layout == KeyLayout::Classic) return;
    // Idempotence: a table already in the modern layout has W/A/S/D bound and no arrow keys; the
    // substitution would then wrongly move its LSHIFT (full speed) to the middle button.
    for (const auto& b : table)
        if (b.key == sc("SCAN_W") || b.key == sc("SCAN_A")) return;
    const auto& m = modern_vehicle_map();
    for (auto& b : table) {
        if (auto it = m.find(b.key); it != m.end()) b.key = it->second;
        if (auto it = m.find(b.modifier); it != m.end()) b.modifier = it->second;
    }
    // Sprint: the movement keys walk at half throttle; with Shift held the same keys give the
    // original's full value. Each movement line gets a Shift twin (press and release), so the
    // modifier switching of CManManager (UpdateModifierBindings) changes gait while a key is held.
    std::vector<Binding> sprint;
    for (auto& b : table) {
        const bool move = b.cls == 0 && (b.cmd == kMcmdWalkF || b.cmd == kMcmdWalkB || b.cmd == kMcmdLeft || b.cmd == kMcmdRight);
        if (!move || b.device != Device::Key || b.modifier != 0) continue;
        Binding fast = b;
        fast.modifier = sc("SCAN_LSHIFT");
        sprint.push_back(fast);
        b.value *= 0.5f;
    }
    table.insert(table.end(), sprint.begin(), sprint.end());
}

std::optional<UiKey> ui_key(std::string_view command, KeyLayout layout) {
    for (const auto& e : kUi) {
        if (command != e.command) continue;
        const bool modern = layout == KeyLayout::Modern;
        return UiKey{sc(modern ? e.modern_mod : e.classic_mod), sc(modern ? e.modern_key : e.classic_key)};
    }
    return std::nullopt;
}

} // namespace parkan::input
