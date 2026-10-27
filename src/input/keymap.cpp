#include "input/keymap.hpp"

#include "core/bytes.hpp"
#include "core/paths.hpp"
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

// The modern layout's interface keys (docs/CONTROLS.md), replacing addition.man's key of the command; the
// commands not listed keep theirs.
struct UiEntry {
    const char* command;
    const char* key;
};
constexpr UiEntry kModernUi[] = {
    {"CMD_JAMES_HQ_MOVE_FORWARD", "SCAN_W"},     {"CMD_JAMES_HQ_MOVE_BACKWARD", "SCAN_S"},
    {"CMD_JAMES_HQ_MOVE_LEFT", "SCAN_A"},        {"CMD_JAMES_HQ_MOVE_RIGHT", "SCAN_D"},
    {"CMD_JAMES_HQ_MOVE_UP", "SCAN_BLANK"},      {"CMD_JAMES_HQ_MOVE_DOWN", "SCAN_LCTRL"},
    {"CMD_JAMES_BASE_ROTLEFT", "SCAN_Q"},        {"CMD_JAMES_BASE_ROTRIGHT", "SCAN_E"},
    {"CMD_JAMES_SELECT_ENEMY", "SCAN_G"},        {"CMD_JAMES_AIM_TARGET", "SCAN_T"},
    {"CMD_JAMES_ZOOM_MODE", "SCAN_RMOUSE"},      {"CMD_JAMES_AUTO_DRIVER", "SCAN_P"},
    {"CMD_JAMES_OUTER_CAMERA", "SCAN_V"},        {"CMD_TERMINAL_NEW_MESSAGE", "SCAN_Y"},
    {"CMD_JAMES_MISSION_OBJ", "SCAN_O"},         {"CMD_QUICK_SAVE", "SCAN_F5"},
    {"CMD_QUICK_LOAD", "SCAN_F9"},
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

void apply_layout(ManTable& rows, KeyLayout layout) {
    apply_mac_defaults(rows);
    if (layout == KeyLayout::Classic) return;
    for (const auto& r : rows) // idempotence, as above
        if (r.key == sc("SCAN_W") || r.key == sc("SCAN_A")) return;
    const auto& m = modern_vehicle_map();
    for (auto& r : rows) {
        if (auto it = m.find(r.key); it != m.end()) r.key = it->second;
        if (auto it = m.find(r.modifier); it != m.end()) r.modifier = it->second;
    }
}

std::optional<UiKey> UiKeys::key(std::string_view command) const {
    const auto id = man_command(command);
    if (!id) return std::nullopt;
    for (const auto& r : rows_)
        if (r.cmd == *id) return UiKey{r.modifier, r.key};
    return std::nullopt;
}

bool UiKeys::is(std::uint16_t scan, std::string_view command) const {
    const auto id = man_command(command);
    return id && scan && ui_lookup(rows_, scan) == *id;
}

UiKeys ui_keys(ManTable addition, KeyLayout layout, bool custom) {
    if (custom || layout == KeyLayout::Classic) {
        apply_mac_defaults(addition);
    } else {
        for (auto& r : addition)
            for (const auto& e : kModernUi)
                if (man_command(e.command) == r.cmd) r.modifier = 0, r.key = sc(e.key);
    }
    return UiKeys(std::move(addition));
}

GameBindings load_game_bindings(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                                KeyLayout layout) {
    GameBindings g;
    auto text = [&](const char* name) {
        std::error_code ec;
        std::optional<std::filesystem::path> p;
        if (!user_dir.empty() && std::filesystem::exists(user_dir / name, ec)) p = user_dir / name, g.custom = true;
        else p = resolve_path(game_dir, name);
        if (!p) return std::string();
        const Bytes b = read_file(*p);
        return std::string(b.begin(), b.end());
    };
    const std::string addition = text("addition.man"), table_1 = text("table_1.man"), hero = text("hero.man");
    g.hero = compile_man(hero, true, false);
    g.unit = compile_man(table_1, false, false);
    for (auto* t : {&g.hero, &g.unit}) {
        if (g.custom) apply_mac_defaults(*t);
        else apply_layout(*t, layout);
    }
    g.ui = ui_keys(parse_man(addition, false), layout, g.custom);
    return g;
}

} // namespace parkan::input
