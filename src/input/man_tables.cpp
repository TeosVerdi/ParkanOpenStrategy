#include "input/man_tables.hpp"

#include "input/scancodes.hpp"

#include <cctype>
#include <cstdio>
#include <sstream>

namespace parkan::input {

namespace {

struct Named {
    std::uint32_t id;
    const char* name;
};

// ConvertCommand (World3D 1000d9e0), in its order; ManualCommand2Str is the inverse.
constexpr Named kManual[] = {
    {0x01, "CMD_OBJ_MOVE_LEFT"},     {0x02, "CMD_OBJ_MOVE_RIGHT"},     {0x03, "CMD_OBJ_MOVE_FORWARD"},
    {0x04, "CMD_OBJ_MOVE_BACKWARD"}, {0x05, "CMD_OBJ_MOVE_DOWN"},      {0x06, "CMD_OBJ_MOVE_UP"},
    {0x07, "CMD_OBJ_TURN_LEFT"},     {0x08, "CMD_OBJ_TURN_RIGHT"},     {0x09, "CMD_OBJ_TURN_UP"},
    {0x0a, "CMD_OBJ_TURN_DOWN"},     {0x0b, "CMD_OBJ_SPEED_MAX"},      {0x0c, "CMD_OBJ_SPEED_MORE"},
    {0x0d, "CMD_OBJ_SPEED_LESS"},    {0x0e, "CMD_OBJ_STOP"},           {0x14, "CMD_TURRET_LEFT"},
    {0x15, "CMD_TURRET_RIGHT"},      {0x16, "CMD_TURRET_UP"},          {0x17, "CMD_TURRET_DOWN"},
    {0x18, "CMD_TURRET_CENTER"},     {0x1e, "CMD_CAMERA_LEFT"},        {0x1f, "CMD_CAMERA_RIGHT"},
    {0x20, "CMD_CAMERA_UP"},         {0x21, "CMD_CAMERA_DOWN"},        {0x22, "CMD_CAMERA_CENTER"},
    {0x23, "CMD_CAMERA_INFRARED"},   {0x28, "CMD_SELECT_ALL_WEAPON"},  {0x29, "CMD_SELECT_WEAPON_1"},
    {0x2a, "CMD_SELECT_WEAPON_2"},   {0x2b, "CMD_SELECT_WEAPON_3"},    {0x2c, "CMD_SELECT_WEAPON_4"},
    {0x2d, "CMD_SELECT_WEAPON_5"},   {0x2e, "CMD_SELECT_WEAPON_6"},    {0x2f, "CMD_SELECT_WEAPON_7"},
    {0x30, "CMD_SELECT_WEAPON_8"},   {0x31, "CMD_SELECT_WEAPON_9"},    {0x32, "CMD_FIRE_SELECTED_CONT"},
    {0x33, "CMD_FIRE_SELECTED"},     {0x41, "CMD_FIRE_MISSILE"},       {0x42, "CMD_FIRE_ALL"},
    {0x3c, "CMD_CAMOUFLAGE_WEAR"},   {0x3d, "CMD_REPAIRSYS_ON"},       {0x40, "CMD_SPOTLIGHT"},
    {0x3f, "CMD_CHANGE_TABLE"},
};

// ConvertUICommand (iron3d 10039da0); UICommand2Str is the inverse.
constexpr Named kUi[] = {
    {0x2ed, "CMD_HELP"},
    {0x2d3, "CMD_JAMES_HQ_MOVE_LEFT"},     {0x2d4, "CMD_JAMES_HQ_MOVE_RIGHT"},    {0x2d5, "CMD_JAMES_HQ_MOVE_UP"},
    {0x2d6, "CMD_JAMES_HQ_MOVE_DOWN"},     {0x2d7, "CMD_JAMES_HQ_MOVE_FORWARD"},  {0x2d8, "CMD_JAMES_HQ_MOVE_BACKWARD"},
    {0x2d9, "CMD_PAGER"},                  {0x2ea, "CMD_TERMINAL"},               {0x2ec, "CMD_GAME_MENU"},
    {0x2eb, "CMD_TERMINAL_NEW_MESSAGE"},   {0x2da, "CMD_ENTER_STATE"},            {0x2db, "CMD_JAMES_MISSION_OBJ"},
    {0x2dc, "CMD_JAMES_SELECT_TARGET"},    {0x2dd, "CMD_JAMES_SELECT_ENEMY"},     {0x2de, "CMD_JAMES_SELECT_FRIEND"},
    {0x2ee, "CMD_JAMES_AIM_TARGET"},       {0x2df, "CMD_ROLLBACK_STATE"},         {0x2e0, "CMD_JAMES_OUTER_CAMERA"},
    {0x2e1, "CMD_JAMES_COCKPIT_OFF"},      {0x2e2, "CMD_JAMES_ZOOM_MODE"},        {0x2e3, "CMD_JAMES_SATELLITE_MAP"},
    {0x2e4, "CMD_JAMES_WINGMAN_MENU"},     {0x2e5, "CMD_JAMES_BASE_ROTLEFT"},     {0x2e8, "CMD_JAMES_AUTO_DRIVER"},
    {0x2e6, "CMD_JAMES_BASE_ROTRIGHT"},    {0x2ef, "CMD_INC_MAP_ALPHA"},          {0x2f0, "CMD_DEC_MAP_ALPHA"},
    {0x2f1, "CMD_QUICK_SAVE"},             {0x2f2, "CMD_QUICK_LOAD"},
};

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// The Convert*ScanCode functions compare case-insensitively.
std::optional<std::uint16_t> scan_of(std::string_view name) { return scan_from_name(upper(name)); }

std::string scan_text(std::uint16_t code) {
    const std::string_view n = scan_name(code);
    return n.empty() ? "SCAN_NULL" : std::string(n); // port: every code the parser can produce has a name
}

bool axis(std::uint16_t k) { // the mouse and joystick axes CreateTranslateTable treats as analog
    return k == kScanMouseX || k == kScanMouseY || k == 0x24e || k == 0x24f || k == 0x250 || k == 0x253;
}
bool mouse_axis(std::uint16_t k) { return k == kScanMouseX || k == kScanMouseY; }

// One CreateTranslateTable output line: "%s %s %s" = device, modifier and key as the .man spells them.
std::string fill(const char* fmt, const std::string& dev, const std::string& mod, const std::string& key) {
    char buf[512];
    std::snprintf(buf, sizeof buf, fmt, dev.c_str(), mod.c_str(), key.c_str());
    return buf;
}

// The templates (World3D 10024dcc–100269e0), named by their command.
#define T(body) "%s %s %s " body "\n"
constexpr const char* kDummy = T("1 CICLS_UNKNOWN MCMD_DUMMY 0.0 1 0 0.0 0 // DUMMY");
constexpr const char* kChangeTable = T("1 CICLS_UNKNOWN MCMD_TABLE 0.0 1 0 0.0 0 // CHANGE_TABLE");
constexpr const char* kRepair = T("1 CICLS_REPAIRSYS MCMD_STATE 0.0 1 CIS_SWITCH_INV 0.0 0 // REPAIRSYS_ON");
constexpr const char* kCamouflage = T("1 CICLS_DETECTSHIELD MCMD_STATE 0.0 1 CIS_CHAMELEON_INV 0.0 0 // CAMOUFLAGE_WEAR");
constexpr const char* kFireAllOff = T("0 CICLS_MULTIGUN MCMD_FIRE_ALL 0.0 -1 CIS_SWITCHOFF 0.0 0 // stop continue fire all");
constexpr const char* kFireAll = T("1 CICLS_MULTIGUN MCMD_FIRE_ALL 0.0 -1 CIS_CONTINUEFIGHT 0.0 0 // FIRE_ALL");
constexpr const char* kMissileOff = T("0 CICLS_MULTIGUN MCMD_MISSILE 0.0 -1 CIS_SWITCHOFF 0.0 0 // stop continue fire missile ");
constexpr const char* kMissile = T("1 CICLS_MULTIGUN MCMD_MISSILE 0.0 -1 CIS_CONTINUEFIGHT 0.0 0 // FIRE_SELECTED_MISSILE");
constexpr const char* kFireOff = T("0 CICLS_MULTIGUN MCMD_STATE 0.0 -1 CIS_SWITCHOFF 0.0 0 // stop continue fire ");
constexpr const char* kFireCont = T("1 CICLS_MULTIGUN MCMD_STATE 0.0 -1 CIS_CONTINUEFIGHT 0.0 0 // FIRE_SELECTED_CONT");
constexpr const char* kFireOnce = T("1 CICLS_MULTIGUN MCMD_STATE 0.0 -1 CIS_GROUPFIGHT 0.0 0 // FIRE_SELECTED_ONCE");
constexpr const char* kSelectAll = T("1 CICLS_MULTIGUN MCMD_SELECT 0.0 -1 0 0.0 0 // SELECT_ALL_WEAPON");
constexpr const char* kInfrared = T("1 CICLS_CAMERA MCMD_STATE 0.0 1 CIS_INFRARED_INV 0.0 0 // CAMERA_INFRARED");
#undef T

// "Command % not implemented for device: %s.\n": "% n" is a %n conversion (it stores the count into the command
// name buffer), so the 'n' of "not" is consumed and nothing is printed for it.
constexpr const char* kNotImplemented = "Command ot implemented for device: %s.\n";

struct Templates { // mouse axis, joystick axis, key/button (nullptr: not implemented)
    const char *mouse, *joy, *key;
};

} // namespace

std::optional<std::uint32_t> man_command(std::string_view name) {
    const std::string u = upper(name);
    for (const auto& c : kManual)
        if (u == c.name) return c.id;
    for (const auto& c : kUi)
        if (u == c.name) return c.id;
    return std::nullopt;
}

std::string_view man_command_name(std::uint32_t cmd) {
    for (const auto& c : kManual)
        if (cmd == c.id) return c.name;
    for (const auto& c : kUi)
        if (cmd == c.id) return c.name;
    return {};
}

ManTable parse_man(std::string_view text, bool joystick) {
    ManTable out;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd, a, b;
        if (!(ls >> cmd)) break;
        const auto id = man_command(cmd);
        if (!id || !(ls >> a)) break;
        const std::string ua = upper(a);
        if ((ua == "KEY" || ua == "JOY" || ua == "MOUSE") && !(ls >> a)) break; // the optional device word
        if (!(ls >> b)) break;
        const auto mod = scan_of(a), key = scan_of(b);
        if (!mod || !key) break;
        out.push_back({*id, *mod, *key});
    }
    for (auto& r : out) {
        if (joystick && r.key == kScanMouseX) r.key = 0x24e;
        else if (joystick && r.key == kScanMouseY) r.key = 0x24f;
        else if (!joystick && r.key == 0x24e) r.key = kScanMouseX;
        else if (!joystick && r.key == 0x24f) r.key = kScanMouseY;
    }
    return out;
}

std::string write_man(const ManTable& rows) {
    std::string out;
    for (const auto& r : rows)
        out += std::string(man_command_name(r.cmd)) + ' ' + scan_text(r.modifier) + ' ' + scan_text(r.key) + '\n';
    return out;
}

std::string create_translate_table(std::string_view man_text, bool hero) {
    std::string out;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> seen; // (modifier, key) pairs already written
    std::istringstream in{std::string(man_text)};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd_name, mod_name, key_name;
        ls >> cmd_name >> mod_name >> key_name;
        // ConvertCommand only: a UI command (or an unknown name) gives no line.
        std::optional<std::uint32_t> manual;
        for (const auto& c : kManual)
            if (upper(cmd_name) == c.name) manual = c.id;
        if (!manual) continue;
        const std::uint16_t mod = scan_of(mod_name).value_or(0);
        const auto key = scan_of(key_name);
        if (!key || *key == kScanNull) continue; // SCAN_NULL or an unknown name: no line
        const std::uint16_t k = *key;
        const char* dev = k < 0x155 ? "KEY  " : k >= 0x1fe && k <= 0x203 ? "MOUSE" : k >= 0x204 && k <= 0x28a ? "JOY  " : "BAD";
        bool dup = false;
        for (const auto& p : seen) dup |= p.first == mod && p.second == k;
        if (dup) continue;
        seen.emplace_back(mod, k);

        const std::string d = dev;
        auto line1 = [&](const char* fmt) { out += fill(fmt, d, mod_name, key_name); };
        auto not_impl = [&] {
            char buf[128];
            std::snprintf(buf, sizeof buf, kNotImplemented, dev);
            out += buf;
        };
        // A command with one template per device class.
        auto by_device = [&](const Templates& t) {
            const char* f = mouse_axis(k) ? t.mouse : axis(k) ? t.joy : t.key;
            if (f) line1(f);
            else not_impl();
        };
        // A key-only command with a press line and a release line.
        auto key_pair = [&](const char* press, const char* release) {
            if (axis(k)) return not_impl();
            line1(press);
            line1(release);
        };
#define T(body) "%s %s %s " body "\n"
        switch (*manual) {
        case 0x01:
            key_pair(T("1 CICLS_UNKNOWN MCMD_LEFT  1.0 0 0 0.0 0 // OBJ_MOVE_LEFT"),
                     hero ? T("0 CICLS_UNKNOWN MCMD_LEFT  0.0 1 0 0.0 0 // stop robot to left")
                          : T("0 CICLS_UNKNOWN MCMD_LEFT  0.0 0 0 0.0 0 // stop robot to left"));
            break;
        case 0x02:
            key_pair(T("1 CICLS_UNKNOWN MCMD_RIGHT  1.0 0 0 0.0 0 // OBJ_MOVE_RIGHT"),
                     hero ? T("0 CICLS_UNKNOWN MCMD_RIGHT  0.0 1 0 0.0 0 // stop robot to right")
                          : T("0 CICLS_UNKNOWN MCMD_RIGHT  0.0 0 0 0.0 0 // stop robot to right"));
            break;
        case 0x03:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_FORWARD -5.0 0 0 0.0 0 // OBJ_MOVE_FORWARD"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_FORWARD -1.0 0 0 0.0 0 // OBJ_MOVE_FORWARD"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_WALK_F  1.0 0 0 0.0 0 // OBJ_MOVE_FORWARD"));
                line1(T("0 CICLS_UNKNOWN MCMD_WALK_F  0.0 0 0 0.0 0 // stop robot forward"));
            }
            break;
        case 0x04:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_FORWARD -5.0 0 0 0.0 0 // OBJ_MOVE_BACKWARD"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_FORWARD -1.0 0 0 0.0 0 // OBJ_MOVE_BACKWARD"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_WALK_B -1.0 0 0 0.0 0 // OBJ_MOVE_BACKWARD"));
                line1(T("0 CICLS_UNKNOWN MCMD_WALK_B  0.0 0 0 0.0 0 // stop robot backward"));
            }
            break;
        case 0x05:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_DOWN 5.0 0 0 0.0 0 // OBJ_MOVE_DOWN"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_DOWN 1.0 0 0 0.0 0 // OBJ_MOVE_DOWN"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_DOWN -1.0 0 0 0.0 0 // OBJ_MOVE_DOWN"));
                line1(T("0 CICLS_UNKNOWN MCMD_DOWN  0.0 0 0 0.0 0 // stop robot down"));
            }
            break;
        case 0x06:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_UP 5.0 0 0 0.0 0 // OBJ_MOVE_UP"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_UP 1.0 0 0 0.0 0 // OBJ_MOVE_UP"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_UP  1.0 0 0 0.0 0 // OBJ_MOVE_UP"));
                line1(T("0 CICLS_UNKNOWN MCMD_UP  0.0 0 0 0.0 0 // stop robot up"));
            }
            break;
        case 0x07:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ANGLE_Z 0.15 0 MAN_WRAP 0.0 0 // OBJ_TURN_LEFT"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_Z -0.15 0 0 0.0 0 // OBJ_TURN_LEFT"));
            else {
                line1(hero ? T("1 CICLS_UNKNOWN MCMD_ROTATE_Z  0.1 0 0 0.0 0 // OBJ_TURN_LEFT")
                           : T("1 CICLS_UNKNOWN MCMD_ROTATE_Z  0.7 0 0 0.0 0 // OBJ_TURN_LEFT"));
                line1(T("0 CICLS_UNKNOWN MCMD_ROTATE_Z  0.0 0 0 0.0   0   // stop turn left"));
            }
            break;
        case 0x08:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ANGLE_Z 0.15 0 MAN_WRAP 0.0 0 // OBJ_TURN_RIGHT"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_Z -0.15 0 0 0.0 0 // OBJ_TURN_RIGHT"));
            else {
                line1(hero ? T("1 CICLS_UNKNOWN MCMD_ROTATE_Z -0.1 0 0 0.0 0 // OBJ_TURN_RIGHT")
                           : T("1 CICLS_UNKNOWN MCMD_ROTATE_Z -0.7 0 0 0.0 0 // OBJ_TURN_RIGHT"));
                line1(T("0 CICLS_UNKNOWN MCMD_ROTATE_Z  0.0 0 0 0.0   0   // stop turn right"));
            }
            break;
        case 0x09:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X -2.0 0 0 0.0 0 // OBJ_TURN_UP"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X -0.7 0 0 0.0 0 // OBJ_TURN_UP"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X -0.9 0 0 0.0 0 // OBJ_TURN_UP"));
                line1(T("0 CICLS_UNKNOWN MCMD_ROTATE_X  0.0 0 0 0.0   0   // stop turn up"));
            }
            break;
        case 0x0a:
            if (mouse_axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X -2.0 0 0 0.0 0 // OBJ_TURN_DOWN"));
            else if (axis(k)) line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X -0.7 0 0 0.0 0 // OBJ_TURN_DOWN"));
            else {
                line1(T("1 CICLS_UNKNOWN MCMD_ROTATE_X  0.9 0 0 0.0 0 // OBJ_TURN_DOWN"));
                line1(T("0 CICLS_UNKNOWN MCMD_ROTATE_X  0.0 0 0 0.0   0   // stop turn down"));
            }
            break;
        case 0x0b: by_device({nullptr, nullptr, T("1 CICLS_UNKNOWN MCMD_FORWARD  1.0 0 0 0.0 0 // OBJ_SPEED_MAX")}); break;
        case 0x0c:
            by_device({T("1 CICLS_UNKNOWN MCMD_FORWARD -5.0 0 0 0.0 0 // OBJ_SPEED_MORE"),
                       T("1 CICLS_UNKNOWN MCMD_FORWARD -1.0 0 0 0.0 0 // OBJ_SPEED_MORE"),
                       T("1 CICLS_UNKNOWN MCMD_FORWARD  1.0 0 0 0.05 1000 // OBJ_SPEED_MORE")});
            break;
        case 0x0d:
            by_device({T("1 CICLS_UNKNOWN MCMD_FORWARD -5.0 0 0 0.0 0 // OBJ_SPEED_LESS"),
                       T("1 CICLS_UNKNOWN MCMD_FORWARD -1.0 0 0 0.0 0 // OBJ_SPEED_LESS"),
                       T("1 CICLS_UNKNOWN MCMD_FORWARD -1.0 0 0 0.05 1000 // OBJ_SPEED_LESS")});
            break;
        case 0x0e: by_device({nullptr, nullptr, T("1 CICLS_UNKNOWN MCMD_FORWARD  0.0 0 0 0.0 0 // OBJ_STOP")}); break;
        case 0x14:
            by_device({T("1 CICLS_TURRET MCMD_ANGLE_X 0.15 1 MAN_WRAP 0.0 0 // TURRET_LEFT"),
                       T("1 CICLS_TURRET MCMD_ANGLE_X 0.02 1 MAN_WRAP 0.0 0 // TURRET_LEFT"),
                       T("1 CICLS_TURRET MCMD_ANGLE_X -1.0 1 MAN_WRAP 0.05 20 // TURRET_LEFT")});
            break;
        case 0x15:
            by_device({T("1 CICLS_TURRET MCMD_ANGLE_X 0.15 1 MAN_WRAP 0.0 0 // TURRET_RIGHT"),
                       T("1 CICLS_TURRET MCMD_ANGLE_X 0.02 1 MAN_WRAP 0.0 0 // TURRET_RIGHT"),
                       T("1 CICLS_TURRET MCMD_ANGLE_X 2.0 1 MAN_WRAP 0.05 20 // TURRET_RIGHT")});
            break;
        case 0x16:
            by_device({T("1 CICLS_TURRET MCMD_ANGLE_Y 0.25 1 MAN_NOTWRAP 0.0 0 // TURRET_UP"),
                       T("1 CICLS_TURRET MCMD_ANGLE_Y -0.03 1 MAN_NOTWRAP 0.0 0 // TURRET_UP"),
                       T("1 CICLS_TURRET MCMD_ANGLE_Y -1.0 1 MAN_NOTWRAP 0.05 20 // TURRET_UP")});
            break;
        case 0x17:
            by_device({T("1 CICLS_TURRET MCMD_ANGLE_Y 0.25 1 MAN_NOTWRAP 0.0 0 // TURRET_DOWN"),
                       T("1 CICLS_TURRET MCMD_ANGLE_Y -0.03 1 MAN_NOTWRAP 0.0 0 // TURRET_DOWN"),
                       T("1 CICLS_TURRET MCMD_ANGLE_Y 1.0 1 MAN_NOTWRAP 0.05 20 // TURRET_DOWN")});
            break;
        case 0x18: by_device({nullptr, nullptr, T("1 CICLS_TURRET MCMD_LOCK 0.5 1 MAN_NOTWRAP 0.0 0 // TURRET_LOCK")}); break;
        case 0x1e:
            by_device({T("1 CICLS_CAMERA MCMD_ANGLE_X 0.1 1 MAN_WRAP 0.0 0 // CAMERA_LEFT"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_X 0.01 1 MAN_WRAP 0.0 0 // CAMERA_LEFT"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_X -1.0 1 MAN_WRAP 0.05 2000 // CAMERA_LEFT")});
            break;
        case 0x1f:
            by_device({T("1 CICLS_CAMERA MCMD_ANGLE_X 0.1 1 MAN_WRAP 0.0 0 // CAMERA_RIGHT"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_X 0.01 1 MAN_WRAP 0.0 0 // CAMERA_RIGHT"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_X 1.0 1 MAN_WRAP 0.05 2000 // CAMERA_RIGHT")});
            break;
        case 0x20:
            by_device({T("1 CICLS_CAMERA MCMD_ANGLE_Y 0.15 1 MAN_NOTWRAP 0.0 0 // CAMERA_UP"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_Y -0.01 1 MAN_NOTWRAP 0.0 0 // CAMERA_UP"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_Y -0.7 1 MAN_NOTWRAP 0.05 2000 // CAMERA_UP")});
            break;
        case 0x21:
            by_device({T("1 CICLS_CAMERA MCMD_ANGLE_Y 0.15 1 MAN_NOTWRAP 0.0 0 // CAMERA_DOWN"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_Y -0.01 1 MAN_NOTWRAP 0.0 0 // CAMERA_DOWN"),
                       T("1 CICLS_CAMERA MCMD_ANGLE_Y 0.7 1 MAN_NOTWRAP 0.05 2000 // CAMERA_DOWN")});
            break;
        case 0x22: // CAMERA_CENTER; releasing the modifier recentres too
            if (axis(k)) {
                not_impl();
                break;
            }
            line1(T("1 CICLS_CAMERA MCMD_ANGLE_X 0.5 1 MAN_NOTWRAP 0.0 0 // CAMERA_CENTER(x)"));
            line1(T("1 CICLS_CAMERA MCMD_ANGLE_Y 0.5 1 MAN_NOTWRAP 0.0 0 // CAMERA_CENTER(y)"));
            {
                char buf[256];
                std::snprintf(buf, sizeof buf, "KEY   SCAN_NULL %s 0 CICLS_CAMERA MCMD_ANGLE_X 0.5 1 MAN_NOTWRAP 0.0 0 // CAMERA_CENTER(y)\n",
                              mod_name.c_str());
                out += buf;
                std::snprintf(buf, sizeof buf, "KEY   SCAN_NULL %s 0 CICLS_CAMERA MCMD_ANGLE_Y 0.5 1 MAN_NOTWRAP 0.0 0 // CAMERA_CENTER(y)\n",
                              mod_name.c_str());
                out += buf;
            }
            break;
        case 0x23: by_device({nullptr, nullptr, kInfrared}); break;
        case 0x28: by_device({nullptr, nullptr, kSelectAll}); break;
        case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31:
            if (axis(k)) not_impl();
            else {
                char buf[256];
                std::snprintf(buf, sizeof buf, T("1 CICLS_MULTIGUN MCMD_SELECT 0.0 %d 0 0.0 0 // SELECT_WEAPON_(n)"), d.c_str(),
                              mod_name.c_str(), key_name.c_str(), int(*manual) - 0x28);
                out += buf;
            }
            break;
        case 0x32: key_pair(kFireCont, kFireOff); break;
        case 0x33: by_device({nullptr, nullptr, kFireOnce}); break;
        case 0x3c: by_device({nullptr, nullptr, kCamouflage}); break;
        case 0x3d: by_device({nullptr, nullptr, kRepair}); break;
        case 0x3f: by_device({nullptr, nullptr, kChangeTable}); break;
        case 0x41: key_pair(kMissile, kMissileOff); break;
        case 0x42: key_pair(kFireAll, kFireAllOff); break;
        default: line1(kDummy); break; // CMD_SPOTLIGHT: every device
        }
#undef T
    }
    return out;
}

std::vector<Binding> compile_man(std::string_view man_text, bool hero, bool joystick) {
    return parse_translate_table(create_translate_table(write_man(parse_man(man_text, joystick)), hero));
}

std::array<ManTable, 4> build_game_tables(const std::array<ManTable, 4>& tabs) {
    struct Entry {
        int tab;
        std::uint32_t cmd;
    };
    // The "other" tab's vehicle commands, shared by the three vehicle lists.
    static constexpr std::uint32_t kOtherVehicle[] = {0x0b, 0x0c, 0x0d, 0x0e, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x28, 0x29, 0x2a,
                                                      0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x32, 0x3c, 0x3d, 0x23};
    std::array<std::vector<Entry>, 4> lists;
    // 10101568: addition.man
    for (std::uint32_t c : {0x2d3u, 0x2d4u, 0x2d7u, 0x2d8u, 0x2d5u, 0x2d6u, 0x2e5u, 0x2e6u}) lists[0].push_back({1, c});
    for (std::uint32_t c : {0x2dau, 0x2dfu, 0x2dcu, 0x2ddu, 0x2deu, 0x2eeu, 0x2e4u, 0x2e8u, 0x2e0u, 0x2e1u, 0x2e2u, 0x23u,
                            0x2e3u, 0x2edu, 0x2ecu, 0x2d9u, 0x2eau, 0x2ebu, 0x2dbu, 0x2efu, 0x2f0u, 0x2f1u, 0x2f2u})
        lists[0].push_back({0, c});
    // 10101668: table_2.man (warbots); 10101788: table_1.man, the same without 5/6 (move up/down)
    for (std::uint32_t c : {1u, 2u, 3u, 4u, 6u, 5u, 7u, 8u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}) {
        lists[1].push_back({2, c});
        if (c != 5 && c != 6) lists[2].push_back({2, c});
    }
    // 10101898: hero.man
    for (std::uint32_t c : {1u, 2u, 3u, 4u, 7u, 8u, 0x16u, 0x17u}) lists[3].push_back({3, c});
    for (int t = 1; t < 4; ++t)
        for (std::uint32_t c : kOtherVehicle) lists[std::size_t(t)].push_back({0, c});

    std::array<ManTable, 4> out;
    for (std::size_t t = 0; t < 4; ++t)
        for (const auto& e : lists[t])
            for (const auto& r : tabs[std::size_t(e.tab)])
                if (r.cmd == e.cmd) {
                    out[t].push_back(r);
                    break;
                }
    return out;
}

std::optional<std::uint32_t> ui_lookup(const ManTable& rows, std::uint16_t scan) {
    for (const auto& r : rows)
        if (r.modifier == 0 && r.key == scan) return r.cmd;
    return std::nullopt;
}

void apply_mac_defaults(ManTable& rows) {
    for (auto& r : rows) {
        r.modifier = mac_default_key(r.modifier);
        r.key = mac_default_key(r.key);
    }
}

} // namespace parkan::input
