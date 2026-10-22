#include "res/nres.hpp"

#include <algorithm>
#include <cctype>

namespace parkan {

namespace {
constexpr std::size_t kDirEntrySize = 64;
constexpr std::uint32_t kMagic = 0x7365524E; // "NRes"
} // namespace

NResArchive NResArchive::open(const std::filesystem::path& path) {
    return NResArchive(read_file(path));
}

NResArchive::NResArchive(Bytes bytes) : bytes_(std::move(bytes)) {
    std::span<const std::uint8_t> all(bytes_);
    Reader hdr(all);
    if (hdr.u32() != kMagic) throw FormatError("not an NRes archive");
    hdr.u32(); // version
    const std::uint32_t count = hdr.u32();
    if (std::size_t(count) * kDirEntrySize > all.size()) throw FormatError("bad NRes entry count");
    const std::size_t dir = all.size() - std::size_t(count) * kDirEntrySize;

    entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Reader r(all.subspan(dir + i * kDirEntrySize, kDirEntrySize));
        NResEntry e;
        e.type = r.u32();
        e.attr1 = r.u32();
        e.attr2 = r.u32();
        const std::uint32_t size = r.u32();
        e.attr3 = r.u32();
        auto raw = r.bytes(36);
        e.name.assign(reinterpret_cast<const char*>(raw.data()),
                      ::strnlen(reinterpret_cast<const char*>(raw.data()), raw.size()));
        const std::uint32_t offset = r.u32();
        if (offset > dir || size > dir - offset) throw FormatError("NRes entry outside data area");
        e.data = all.subspan(offset, size);
        entries_.push_back(std::move(e));
    }
}

const NResEntry* NResArchive::find_type(std::uint32_t type) const {
    auto it = std::ranges::find(entries_, type, &NResEntry::type);
    return it == entries_.end() ? nullptr : &*it;
}

const NResEntry* NResArchive::find_name(std::string_view name) const {
    auto eq = [&](const NResEntry& e) {
        return std::ranges::equal(e.name, name, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    };
    auto it = std::ranges::find_if(entries_, eq);
    return it == entries_.end() ? nullptr : &*it;
}

} // namespace parkan
