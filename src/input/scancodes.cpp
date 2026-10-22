#include "input/scancodes.hpp"

namespace parkan::input {

namespace {

struct Named {
    std::uint16_t code;
    const char* name;
};

// World3D ConvertPredicScanCode names (notes/scancodes.tsv; generated). The original ScanCode2Str
// prints SCAN_JOY_R_X for 0x252/0x253; the parser names used here are the correct ones.
constexpr Named kNames[] = {
    // Port additions: the original ignores WM_SYSKEYDOWN, so Alt (Option) had no name; these are its
    // set-1 codes (left 0x38, right extended 0x138), unused by the original tables.
    {0x038, "SCAN_LALT"},
    {0x138, "SCAN_RALT"},
    {0x000, "SCAN_NULL"},
    {0x001, "SCAN_ESC"},
    {0x002, "SCAN_W_1"},
    {0x003, "SCAN_W_2"},
    {0x004, "SCAN_W_3"},
    {0x005, "SCAN_W_4"},
    {0x006, "SCAN_W_5"},
    {0x007, "SCAN_W_6"},
    {0x008, "SCAN_W_7"},
    {0x009, "SCAN_W_8"},
    {0x00a, "SCAN_W_9"},
    {0x00b, "SCAN_W_0"},
    {0x00c, "SCAN_W_SUB"},
    {0x00d, "SCAN_W_PLUS"},
    {0x00e, "SCAN_BS"},
    {0x00f, "SCAN_TAB"},
    {0x010, "SCAN_Q"},
    {0x011, "SCAN_W"},
    {0x012, "SCAN_E"},
    {0x013, "SCAN_R"},
    {0x014, "SCAN_T"},
    {0x015, "SCAN_Y"},
    {0x016, "SCAN_U"},
    {0x017, "SCAN_I"},
    {0x018, "SCAN_O"},
    {0x019, "SCAN_P"},
    {0x01a, "SCAN_LBRACKET"},
    {0x01b, "SCAN_RBRACKET"},
    {0x01c, "SCAN_W_ENTER"},
    {0x01d, "SCAN_LCTRL"},
    {0x01e, "SCAN_A"},
    {0x01f, "SCAN_S"},
    {0x020, "SCAN_D"},
    {0x021, "SCAN_F"},
    {0x022, "SCAN_G"},
    {0x023, "SCAN_H"},
    {0x024, "SCAN_J"},
    {0x025, "SCAN_K"},
    {0x026, "SCAN_L"},
    {0x027, "SCAN_SEMIDOT"},
    {0x028, "SCAN_QUOTE"},
    {0x029, "SCAN_TILDA"},
    {0x02a, "SCAN_LSHIFT"},
    {0x02b, "SCAN_BSLASH"},
    {0x02c, "SCAN_Z"},
    {0x02d, "SCAN_X"},
    {0x02e, "SCAN_C"},
    {0x02f, "SCAN_V"},
    {0x030, "SCAN_B"},
    {0x031, "SCAN_N"},
    {0x032, "SCAN_M"},
    {0x033, "SCAN_COMMA"},
    {0x034, "SCAN_DOT"},
    {0x035, "SCAN_SLASH"},
    {0x036, "SCAN_RSHIFT"},
    {0x037, "SCAN_G_ASTERISK"},
    {0x039, "SCAN_BLANK"},
    {0x03a, "SCAN_CAPS"},
    {0x03b, "SCAN_F1"},
    {0x03c, "SCAN_F2"},
    {0x03d, "SCAN_F3"},
    {0x03e, "SCAN_F4"},
    {0x03f, "SCAN_F5"},
    {0x040, "SCAN_F6"},
    {0x041, "SCAN_F7"},
    {0x042, "SCAN_F8"},
    {0x043, "SCAN_F9"},
    {0x044, "SCAN_F10"},
    {0x045, "SCAN_PAUSE"},
    {0x046, "SCAN_SLOCK"},
    {0x047, "SCAN_G_7"},
    {0x048, "SCAN_G_8"},
    {0x049, "SCAN_G_9"},
    {0x04a, "SCAN_G_SUB"},
    {0x04b, "SCAN_G_4"},
    {0x04c, "SCAN_G_5"},
    {0x04d, "SCAN_G_6"},
    {0x04e, "SCAN_G_PLUS"},
    {0x04f, "SCAN_G_1"},
    {0x050, "SCAN_G_2"},
    {0x051, "SCAN_G_3"},
    {0x052, "SCAN_G_0"},
    {0x053, "SCAN_W_DEL"},
    {0x057, "SCAN_F11"},
    {0x058, "SCAN_F12"},
    {0x11c, "SCAN_G_ENTER"},
    {0x11d, "SCAN_RCTRL"},
    {0x135, "SCAN_G_SLASH"},
    {0x145, "SCAN_NUMLOCK"},
    {0x147, "SCAN_G_HOME"},
    {0x148, "SCAN_G_UP"},
    {0x149, "SCAN_G_PGUP"},
    {0x14b, "SCAN_G_LEFT"},
    {0x14d, "SCAN_G_RIGHT"},
    {0x14f, "SCAN_G_END"},
    {0x150, "SCAN_G_DOWN"},
    {0x151, "SCAN_G_PGDN"},
    {0x152, "SCAN_G_INS"},
    {0x153, "SCAN_G_DEL"},
    {0x1fe, "SCAN_MOUSE_X"},
    {0x1ff, "SCAN_MOUSE_Y"},
    {0x200, "SCAN_LMOUSE"},
    {0x201, "SCAN_RMOUSE"},
    {0x202, "SCAN_MMOUSE"},
    {0x204, "SCAN_JOY1"},
    {0x205, "SCAN_JOY2"},
    {0x206, "SCAN_JOY3"},
    {0x207, "SCAN_JOY4"},
    {0x208, "SCAN_JOY5"},
    {0x209, "SCAN_JOY6"},
    {0x20a, "SCAN_JOY7"},
    {0x20b, "SCAN_JOY8"},
    {0x20c, "SCAN_JOY9"},
    {0x20d, "SCAN_JOY10"},
    {0x20e, "SCAN_JOY11"},
    {0x20f, "SCAN_JOY12"},
    {0x210, "SCAN_JOY13"},
    {0x211, "SCAN_JOY14"},
    {0x212, "SCAN_JOY15"},
    {0x213, "SCAN_JOY16"},
    {0x214, "SCAN_JOY17"},
    {0x215, "SCAN_JOY18"},
    {0x216, "SCAN_JOY19"},
    {0x217, "SCAN_JOY20"},
    {0x218, "SCAN_JOY21"},
    {0x219, "SCAN_JOY22"},
    {0x21a, "SCAN_JOY23"},
    {0x21b, "SCAN_JOY24"},
    {0x21c, "SCAN_JOY25"},
    {0x21d, "SCAN_JOY26"},
    {0x21e, "SCAN_JOY27"},
    {0x21f, "SCAN_JOY28"},
    {0x220, "SCAN_JOY29"},
    {0x221, "SCAN_JOY30"},
    {0x222, "SCAN_JOY31"},
    {0x223, "SCAN_JOY32"},
    {0x226, "SCAN_JPOV1_0"},
    {0x227, "SCAN_JPOV1_45"},
    {0x228, "SCAN_JPOV1_90"},
    {0x229, "SCAN_JPOV1_135"},
    {0x22a, "SCAN_JPOV1_180"},
    {0x22b, "SCAN_JPOV1_225"},
    {0x22c, "SCAN_JPOV1_270"},
    {0x22d, "SCAN_JPOV1_315"},
    {0x230, "SCAN_JPOV2_0"},
    {0x231, "SCAN_JPOV2_45"},
    {0x232, "SCAN_JPOV2_90"},
    {0x233, "SCAN_JPOV2_135"},
    {0x234, "SCAN_JPOV2_180"},
    {0x235, "SCAN_JPOV2_225"},
    {0x236, "SCAN_JPOV2_270"},
    {0x237, "SCAN_JPOV2_315"},
    {0x23a, "SCAN_JPOV3_0"},
    {0x23b, "SCAN_JPOV3_45"},
    {0x23c, "SCAN_JPOV3_90"},
    {0x23d, "SCAN_JPOV3_135"},
    {0x23e, "SCAN_JPOV3_180"},
    {0x23f, "SCAN_JPOV3_225"},
    {0x240, "SCAN_JPOV3_270"},
    {0x241, "SCAN_JPOV3_315"},
    {0x244, "SCAN_JPOV4_0"},
    {0x245, "SCAN_JPOV4_45"},
    {0x246, "SCAN_JPOV4_90"},
    {0x247, "SCAN_JPOV4_135"},
    {0x248, "SCAN_JPOV4_180"},
    {0x249, "SCAN_JPOV4_225"},
    {0x24a, "SCAN_JPOV4_270"},
    {0x24b, "SCAN_JPOV4_315"},
    {0x24e, "SCAN_JOY_X"},
    {0x24f, "SCAN_JOY_Y"},
    {0x250, "SCAN_JOY_Z"},
    {0x251, "SCAN_JOY_R_X"},
    {0x252, "SCAN_JOY_R_Y"},
    {0x253, "SCAN_JOY_R_Z"},
};

} // namespace

std::optional<std::uint16_t> scan_from_name(std::string_view name) {
    for (const auto& n : kNames)
        if (name == n.name) return n.code;
    return std::nullopt;
}

std::string_view scan_name(std::uint16_t code) {
    for (const auto& n : kNames)
        if (n.code == code) return n.name;
    return {};
}

} // namespace parkan::input

namespace parkan::input {

std::uint16_t scan_from_hid(int usage) {
    static const char* const kByUsage[] = {
        // 0x00..0x03: no key / errors
        nullptr, nullptr, nullptr, nullptr,
        // 0x04..0x1d: A..Z
        "SCAN_A", "SCAN_B", "SCAN_C", "SCAN_D", "SCAN_E", "SCAN_F", "SCAN_G", "SCAN_H", "SCAN_I", "SCAN_J",
        "SCAN_K", "SCAN_L", "SCAN_M", "SCAN_N", "SCAN_O", "SCAN_P", "SCAN_Q", "SCAN_R", "SCAN_S", "SCAN_T",
        "SCAN_U", "SCAN_V", "SCAN_W", "SCAN_X", "SCAN_Y", "SCAN_Z",
        // 0x1e..0x27: 1..9, 0
        "SCAN_W_1", "SCAN_W_2", "SCAN_W_3", "SCAN_W_4", "SCAN_W_5", "SCAN_W_6", "SCAN_W_7", "SCAN_W_8",
        "SCAN_W_9", "SCAN_W_0",
        // 0x28..0x38: Return, Escape, Backspace, Tab, Space, - = [ ] \ (non-US #) ; ' ` , . /
        "SCAN_W_ENTER", "SCAN_ESC", "SCAN_BS", "SCAN_TAB", "SCAN_BLANK", "SCAN_W_SUB", "SCAN_W_PLUS",
        "SCAN_LBRACKET", "SCAN_RBRACKET", "SCAN_BSLASH", "SCAN_BSLASH", "SCAN_SEMIDOT", "SCAN_QUOTE",
        "SCAN_TILDA", "SCAN_COMMA", "SCAN_DOT", "SCAN_SLASH",
        // 0x39..0x45: Caps Lock, F1..F12
        "SCAN_CAPS", "SCAN_F1", "SCAN_F2", "SCAN_F3", "SCAN_F4", "SCAN_F5", "SCAN_F6", "SCAN_F7", "SCAN_F8",
        "SCAN_F9", "SCAN_F10", "SCAN_F11", "SCAN_F12",
        // 0x46..0x52: Print Screen, Scroll Lock, Pause, Insert, Home, Page Up, Delete, End, Page Down,
        // Right, Left, Down, Up (the gray block; on a Mac, Fn+arrows give Home/End/Page Up/Page Down)
        nullptr, "SCAN_SLOCK", "SCAN_PAUSE", "SCAN_G_INS", "SCAN_G_HOME", "SCAN_G_PGUP", "SCAN_G_DEL",
        "SCAN_G_END", "SCAN_G_PGDN", "SCAN_G_RIGHT", "SCAN_G_LEFT", "SCAN_G_DOWN", "SCAN_G_UP",
        // 0x53..0x63: Num Lock, keypad / * - + Enter 1..9 0 .
        "SCAN_NUMLOCK", "SCAN_G_SLASH", "SCAN_G_ASTERISK", "SCAN_G_SUB", "SCAN_G_PLUS", "SCAN_G_ENTER",
        "SCAN_G_1", "SCAN_G_2", "SCAN_G_3", "SCAN_G_4", "SCAN_G_5", "SCAN_G_6", "SCAN_G_7", "SCAN_G_8",
        "SCAN_G_9", "SCAN_G_0", "SCAN_W_DEL",
        // 0x64: non-US backslash (ISO key next to left Shift; ` on a Mac ISO keyboard)
        "SCAN_TILDA",
    };
    const char* name = nullptr;
    if (usage >= 0 && std::size_t(usage) < std::size(kByUsage)) name = kByUsage[usage];
    if (usage == 0xe0) name = "SCAN_LCTRL";
    if (usage == 0xe1) name = "SCAN_LSHIFT";
    if (usage == 0xe2) name = "SCAN_LALT"; // Option (port addition)
    if (usage == 0xe6) name = "SCAN_RALT";
    if (usage == 0xe4) name = "SCAN_RCTRL";
    if (usage == 0xe5) name = "SCAN_RSHIFT";
    if (!name) return kScanNull;
    return scan_from_name(name).value_or(kScanNull);
}

} // namespace parkan::input
