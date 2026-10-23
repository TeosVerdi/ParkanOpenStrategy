#include "install/iscab.hpp"

#include "install/inflate.hpp"

#include <algorithm>
#include <cstring>

namespace parkan::install {

namespace {

constexpr std::uint32_t kSignature = 0x28635349; // "ISc("
constexpr std::uint16_t kSplit = 1, kObfuscated = 2, kCompressed = 4, kInvalid = 8;
constexpr int kMaxFileGroups = 71, kMaxComponents = 71;

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }

} // namespace

void is_deobfuscate(std::span<std::uint8_t> data, std::uint32_t& seed) {
    for (std::uint8_t& b : data) {
        const std::uint8_t x = b ^ 0xd5;
        b = std::uint8_t(std::uint8_t(x >> 2 | x << 6) - seed % 0x47);
        ++seed;
    }
}

const std::uint8_t* IsCabinet::at(std::uint64_t offset, std::size_t n) const {
    const std::uint64_t end = offset + n;
    if (end > header_->size()) throw FormatError("cabinet header: offset out of range");
    if (end > head_.size()) {
        const std::uint64_t want = std::min<std::uint64_t>(header_->size(), std::max<std::uint64_t>(end, head_.size() * 2 + 65536));
        const std::size_t old = head_.size();
        head_.resize(std::size_t(want));
        header_->read(old, head_.data() + old, head_.size() - old);
    }
    return head_.data() + offset;
}

std::string IsCabinet::string_at(std::uint64_t offset) const {
    std::string s;
    for (;;) {
        const char c = char(*at(offset + s.size(), 1));
        if (!c) break;
        s += c;
        if (s.size() > 4096) throw FormatError("cabinet header: unterminated string");
    }
    return cp1251_to_utf8(s);
}

IsCabinet::IsCabinet(std::shared_ptr<const ByteSource> header, std::shared_ptr<const ByteSource> data)
    : header_(std::move(header)), data_(std::move(data)) {
    // Common header: signature, version, volume info, cabinet descriptor offset and size.
    const std::uint8_t* c = at(0, 20);
    if (le32(c) != kSignature) throw FormatError("not an InstallShield cabinet");
    const std::uint32_t version = le32(c + 4);
    if (version >> 24 == 1) {
        major_ = int(version >> 12 & 0xf);
    } else if (version >> 24 == 2 || version >> 24 == 4) {
        major_ = int(version & 0xffff);
        if (major_) major_ /= 100;
    }
    if (major_ != 5 && major_ != 0)
        throw FormatError("InstallShield cabinet version " + std::to_string(major_) + " is not supported (only 5)");
    cab_desc_ = le32(c + 12);
    if (!le32(c + 16)) throw FormatError("cabinet has no descriptor (not the header volume)");

    // Cabinet descriptor.
    const std::uint8_t* d = at(cab_desc_, 0x30 + 0xe + 4 * (kMaxFileGroups + kMaxComponents));
    file_table_ = le32(d + 0x0c);
    dir_count_ = le32(d + 0x1c);
    file_count_ = le32(d + 0x28);
    std::uint32_t group_offsets[kMaxFileGroups];
    for (int i = 0; i < kMaxFileGroups; ++i) group_offsets[i] = le32(d + 0x3e + 4 * i);

    const std::uint8_t* t = at(std::uint64_t(cab_desc_) + file_table_, 4 * std::size_t(dir_count_ + file_count_));
    table_.resize(dir_count_ + file_count_);
    for (std::size_t i = 0; i < table_.size(); ++i) table_[i] = le32(t + 4 * i);
    const std::uint64_t base = std::uint64_t(cab_desc_) + file_table_;

    auto descriptor = [&](std::uint32_t index) {
        // v5 descriptor (0x2a bytes): name, directory, flags, sizes, attributes, DOS date and time at
        // 0x12/0x16/0x1a, data offset at 0x26. (unshield reads 0x3a with an MD5 that only v6+ check.)
        const std::uint8_t* p = at(base + table_[dir_count_ + index], 0x2a);
        CabFile f;
        f.index = int(index);
        const std::uint32_t name_offset = le32(p), dir_index = le32(p + 4);
        f.flags = le16(p + 8);
        f.expanded = le32(p + 10);
        f.compressed = le32(p + 14);
        f.data_offset = le32(p + 0x26);
        if ((f.flags & kInvalid) || !name_offset || !f.data_offset) f.flags |= kInvalid;
        else {
            if (dir_index >= dir_count_) throw FormatError("cabinet: bad directory index");
            f.name = string_at(base + name_offset);
            f.dir = string_at(base + table_[dir_index]);
            std::replace(f.dir.begin(), f.dir.end(), '\\', '/');
            while (!f.dir.empty() && f.dir.back() == '/') f.dir.pop_back();
        }
        return f;
    };

    // File groups: linked lists of (name, descriptor, next) per hash bucket.
    for (std::uint32_t offset : group_offsets) {
        for (int guard = 0; offset && guard < 4096; ++guard) {
            const std::uint8_t* node = at(std::uint64_t(cab_desc_) + offset, 12);
            const std::uint32_t desc = le32(node + 4), next = le32(node + 8);
            const std::uint8_t* g = at(std::uint64_t(cab_desc_) + desc, 4 + 0x48 + 8);
            const std::string name = string_at(std::uint64_t(cab_desc_) + le32(g));
            const std::uint32_t first = le32(g + 4 + 0x48), last = le32(g + 4 + 0x48 + 4);
            groups_.push_back(name);
            for (std::uint32_t i = first; i <= last && i < file_count_; ++i) {
                CabFile f = descriptor(i);
                if (f.flags & kInvalid) continue;
                f.group = name;
                files_.push_back(std::move(f));
            }
            offset = next;
        }
    }
}

const CabFile* IsCabinet::find(std::string_view path) const {
    for (const auto& f : files_)
        if (path_iequals(f.path(), path)) return &f;
    return nullptr;
}

bool IsCabinet::extract(const CabFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const {
    if (f.flags & kSplit) {
        // A split file continues in the next volume (data2.cab); the 2001 disc has a single volume.
        const std::uint64_t need = (f.flags & kCompressed) ? f.compressed : f.expanded;
        if (std::uint64_t(f.data_offset) + need > data_->size())
            throw FormatError(f.path() + ": split across cabinet volumes (not supported)");
    }
    std::uint32_t seed = 0;
    std::uint64_t pos = f.data_offset, total = 0;
    Bytes in, out;
    auto read = [&](std::size_t n) {
        in.resize(n);
        data_->read(pos, in.data(), n);
        pos += n;
        if (f.flags & kObfuscated) is_deobfuscate(in, seed);
    };
    auto emit = [&](std::span<const std::uint8_t> s) {
        total += s.size();
        return sink(s);
    };
    if (f.flags & kCompressed) {
        std::uint64_t left = f.compressed;
        while (left) {
            if (left < 2) throw FormatError(f.path() + ": truncated chunk header");
            read(2);
            const std::size_t n = le16(in.data());
            if (n + 2 > left) throw FormatError(f.path() + ": chunk past the compressed size");
            read(n);
            left -= n + 2;
            out.clear();
            inflate_raw(in, out, f.expanded - total);
            if (!emit(out)) return false;
        }
    } else {
        std::uint64_t left = f.expanded;
        while (left) {
            read(std::size_t(std::min<std::uint64_t>(left, 1 << 16)));
            left -= in.size();
            if (!emit(in)) return false;
        }
    }
    if (total != f.expanded) throw FormatError(f.path() + ": expanded to " + std::to_string(total) + " bytes, expected " +
                                               std::to_string(f.expanded));
    return true;
}

Bytes IsCabinet::extract(const CabFile& f) const {
    Bytes b;
    b.reserve(f.expanded);
    extract(f, [&](std::span<const std::uint8_t> s) {
        b.insert(b.end(), s.begin(), s.end());
        return true;
    });
    return b;
}

} // namespace parkan::install
