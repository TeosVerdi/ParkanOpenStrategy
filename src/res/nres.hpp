// NRes archives (.lib/.rlb/.res/.trf/...): a header, the entry payloads, then a directory of
// 64-byte records at the end of the file. Original reader: Ngi32 INgiResFile (ngi32-platform.md).
#pragma once

#include "core/bytes.hpp"

#include <optional>

namespace parkan {

constexpr std::uint32_t fourcc(std::string_view s) {
    return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 |
           std::uint32_t(std::uint8_t(s[2])) << 16 | std::uint32_t(std::uint8_t(s[3])) << 24;
}

struct NResEntry {
    std::uint32_t type = 0;  // four-character code, e.g. 'TRF0'
    std::uint32_t attr1 = 0; // per-type meaning (often a record count)
    std::uint32_t attr2 = 0; // per-type meaning (often a version)
    std::uint32_t attr3 = 0; // per-type meaning (often a record size)
    std::string name;        // up to 36 bytes, CP1251
    std::span<const std::uint8_t> data;
};

class NResArchive {
public:
    static NResArchive open(const std::filesystem::path& path);
    explicit NResArchive(Bytes bytes);

    const std::vector<NResEntry>& entries() const { return entries_; }
    // Lookups mirror the original: by type, or by name ignoring ASCII case.
    const NResEntry* find_type(std::uint32_t type) const;
    const NResEntry* find_name(std::string_view name) const;

private:
    Bytes bytes_;
    std::vector<NResEntry> entries_;
};

} // namespace parkan
