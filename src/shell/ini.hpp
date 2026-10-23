// Win32 private-profile files (GetPrivateProfileString / WritePrivateProfileString), as iron3d reads and
// writes iron_3d.ini: `[Section]` headers, `KEY=value` lines, case-insensitive names. Edits keep every
// other line as it was. Spec: notes/shell.md "Options".
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace parkan::shell {

// The value of `key` in `section` (whitespace around it trimmed, surrounding quotes kept), or nullopt.
std::optional<std::string> ini_get(std::string_view text, std::string_view section, std::string_view key);
int ini_get_int(std::string_view text, std::string_view section, std::string_view key, int fallback);
// WritePrivateProfileString: replaces the key's line, or appends it at the end of its section (the section
// is created at the end of the file when missing). Line ends follow the file's (CRLF when it has any).
std::string ini_set(std::string text, std::string_view section, std::string_view key, std::string_view value);

} // namespace parkan::shell
