#include "input/translate_table.hpp"

#include "core/bytes.hpp"
#include "input/scancodes.hpp"

#include <cstdlib>
#include <sstream>
#include <utility>

namespace parkan::input {

namespace {

struct Pair {
    const char* name;
    std::uint32_t value;
};

constexpr Pair kClasses[] = {
    {"CICLS_UNKNOWN", 0},      {"CICLS_TURRET", 1},        {"CICLS_MULTIGUN", 2}, {"CICLS_SIMPLE", 3},
    {"CICLS_CAMERA", 4},       {"CICLS_ENGINE", 5},        {"CICLS_RADAR", 8},    {"CICLS_FIGHTSHIELD", 9},
    {"CICLS_DETECTSHIELD", 10}, {"CICLS_ELEVATOR", 11},    {"CICLS_DOOR", 12},    {"CICLS_COMPUTER", 13},
    {"CICLS_REPAIRSYS", 15},   {"CICLS_POWERSTOR", 19},
};

constexpr Pair kCommands[] = {
    {"MCMD_DUMMY", kMcmdDummy},     {"MCMD_STATE", kMcmdState},       {"MCMD_ROTATE_X", kMcmdRotateX},
    {"MCMD_ROTATE_Y", kMcmdRotateY}, {"MCMD_ROTATE_Z", kMcmdRotateZ}, {"MCMD_ANGLE_X", kMcmdAngleX},
    {"MCMD_ANGLE_Y", kMcmdAngleY},  {"MCMD_FORWARD", kMcmdForward},   {"MCMD_BACK", kMcmdBack},
    {"MCMD_LEFT", kMcmdLeft},       {"MCMD_RIGHT", kMcmdRight},       {"MCMD_UP", kMcmdUp},
    {"MCMD_DOWN", kMcmdDown},       {"MCMD_SELECT", kMcmdSelect},     {"MCMD_SELECT_NEXT", kMcmdSelectNext},
    {"MCMD_TABLE", kMcmdTable},     {"MCMD_ANGLE_Z", kMcmdAngleZ},    {"MCMD_MISSILE", kMcmdMissile},
    {"MCMD_FIRE_ALL", kMcmdFireAll}, {"MCMD_WALK_F", kMcmdWalkF},     {"MCMD_WALK_B", kMcmdWalkB},
    {"MCMD_LOCK", kMcmdLock},
};

constexpr Pair kStates[] = {
    {"MAN_WRAP", 0},
    {"MAN_NOTWRAP", 1},
    {"CIS_SWITCHOFF", 0},
    {"CIS_SWITCHON", 0x20},
    {"CIS_SWITCH_INV", 0x40},
    {"CIS_CONTINUEFIGHT", 0x100},
    {"CIS_TURRETCONTROL", 0x100},
    {"CIS_ANGLETRACE", 0x100},
    {"CIS_SINGLEFIGHT", 0x200},
    {"CIS_MANUALCONTROL", 0x200},
    {"CIS_MANUALTRACE", 0x200},
    {"CIS_GROUPFIGHT", 0x400},
    {"CIS_POINTTRACE", 0x400},
    {"CIS_INFRARED_ON", 0x1000},
    {"CIS_INFRARED_OFF", 0x2000},
    {"CIS_INFRARED_INV", 0x4000},
    {"CIS_CHAMELEON_INV", 0x4000},
};

template <std::size_t N>
std::uint32_t lookup(const Pair (&table)[N], const std::string& name, const char* what) {
    for (const auto& p : table)
        if (name == p.name) return p.value;
    throw FormatError(std::string("unknown ") + what + " " + name);
}

std::uint16_t scan(const std::string& name) {
    if (auto c = scan_from_name(name)) return *c;
    throw FormatError("unknown scan code " + name);
}

} // namespace

std::vector<Binding> parse_translate_table(std::string_view text) {
    std::vector<Binding> out;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::string comment;
        if (auto c = line.find("//"); c != std::string::npos) {
            comment = line.substr(c + 2);
            line.resize(c);
            const auto first = comment.find_first_not_of(" \t"), last = comment.find_last_not_of(" \t\r");
            comment = first == std::string::npos ? std::string() : comment.substr(first, last - first + 1);
        }
        std::istringstream w(line);
        std::string device, modifier, key, press, cls, cmd, value, index, state, accel, ramp;
        if (!(w >> device)) continue;
        Binding b;
        if (device == "KEY") b.device = Device::Key;
        else if (device == "MOUSE") b.device = Device::Mouse;
        else if (device == "JOY") b.device = Device::Joy;
        else continue;
        if (!(w >> modifier >> key >> press >> cls >> cmd >> value >> index >> state >> accel >> ramp))
            throw FormatError("short binding line: " + line);
        b.modifier = scan(modifier);
        b.key = scan(key);
        b.on_press = std::atoi(press.c_str()) != 0;
        b.cls = lookup(kClasses, cls, "class");
        b.cmd = lookup(kCommands, cmd, "command");
        b.value = std::strtof(value.c_str(), nullptr);
        b.index = std::atoi(index.c_str());
        bool named = false;
        for (const auto& p : kStates)
            if (state == p.name) b.state = std::int32_t(p.value), named = true;
        if (!named) b.state = std::atoi(state.c_str());
        b.accel = std::strtof(accel.c_str(), nullptr);
        b.ramp_ms = std::atoi(ramp.c_str());
        b.comment = std::move(comment);
        out.push_back(std::move(b));
    }
    return out;
}

std::uint16_t mac_default_key(std::uint16_t key) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"SCAN_G_5", "SCAN_L"},         {"SCAN_G_PLUS", "SCAN_W_PLUS"}, {"SCAN_G_SUB", "SCAN_W_SUB"},
        {"SCAN_G_ASTERISK", "SCAN_BSLASH"}, {"SCAN_G_SLASH", "SCAN_BS"}, {"SCAN_G_PGUP", "SCAN_QUOTE"},
        {"SCAN_G_PGDN", "SCAN_SLASH"},
    };
    for (const auto& [from, to] : kMap)
        if (key == scan(from)) return scan(to);
    return key;
}

void apply_mac_defaults(std::vector<Binding>& table) {
    for (auto& b : table) {
        b.key = mac_default_key(b.key);
        b.modifier = mac_default_key(b.modifier);
    }
}

} // namespace parkan::input
