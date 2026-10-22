// The game's text: TextRes.dll's RT_STRING tables (UTF-16, Russian language 1049), which the
// original loads with LoadStringA (notes/PROGRESS.md "Text encoding"). The port reads them straight
// from the DLL's PE resource directory.
#pragma once

#include "core/bytes.hpp"

#include <map>

namespace parkan {

class TextRes {
public:
    // Parses a PE image (TextRes.dll). Strings of `language` win; other languages fill gaps.
    static TextRes from_pe(std::span<const std::uint8_t> image, std::uint16_t language = 1049);

    // UTF-8 text of string `id`, or empty when missing.
    const std::string& get(std::uint32_t id) const;
    // DATA/TextRes.cfg (cp1251 text): `NAME = id` lines map the names the game asks for
    // (GetText(name), e.g. "MIS_OBJ") to string ids.
    void load_names(std::string_view cfg);
    const std::string& get(std::string_view name) const;
    std::size_t size() const { return strings_.size(); }
    const std::map<std::uint32_t, std::string>& all() const { return strings_; }

private:
    std::map<std::uint32_t, std::string> strings_;
    std::map<std::string, std::uint32_t, std::less<>> names_;
};

std::string utf16le_to_utf8(std::span<const std::uint8_t> bytes);

} // namespace parkan
