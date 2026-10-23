#include "install/disc.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace parkan::install {

namespace fs = std::filesystem;

namespace {

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }
std::uint64_t le64(const std::uint8_t* p) { return le32(p) | std::uint64_t(le32(p + 4)) << 32; }

constexpr std::uint8_t kSync[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};

// User data offset inside a stored sector of a data track.
std::uint32_t user_offset(std::uint32_t sector_size, int mode) {
    if (sector_size == 2048) return 0;
    if (sector_size == 2336) return 8;            // mode 2 without sync/header: subheader first
    return mode == 2 ? 24 : 16;                    // 2352 / 2448: sync + header (+ XA subheader)
}

// A sibling of `p` with the same stem and extension `ext` (any case), if one exists.
std::optional<fs::path> sibling(const fs::path& p, std::string_view ext) {
    std::error_code ec;
    const std::string stem = ascii_lower(p.stem().string());
    for (const auto& e : fs::directory_iterator(p.parent_path().empty() ? fs::path(".") : p.parent_path(), ec)) {
        const fs::path& q = e.path();
        if (ascii_lower(q.stem().string()) == stem && ascii_lower(q.extension().string()) == ext) return q;
    }
    return std::nullopt;
}

std::uint64_t file_size_or_throw(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    if (ec) throw std::runtime_error("cannot open " + p.string());
    return n;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xc0 | cp >> 6), out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += char(0xe0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3f)), out += char(0x80 | (cp & 0x3f));
    } else {
        out += char(0xf0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3f)), out += char(0x80 | (cp >> 6 & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

std::string utf16be_to_utf8(const std::uint8_t* p, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        char32_t c = char32_t(p[i] << 8 | p[i + 1]);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
            const char32_t lo = char32_t(p[i + 2] << 8 | p[i + 3]);
            if (lo >= 0xdc00 && lo < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00), i += 2;
        }
        append_utf8(out, c);
    }
    return out;
}

std::vector<std::string> split_path(std::string_view p) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t end = p.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = p.size();
        if (end > start && p.substr(start, end - start) != ".") parts.emplace_back(p.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

} // namespace

std::string ascii_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

std::string normalise_iso_name(std::string_view name) {
    if (const auto semi = name.find(';'); semi != std::string_view::npos) name = name.substr(0, semi);
    while (!name.empty() && name.back() == '.') name.remove_suffix(1);
    return std::string(name);
}

bool path_iequals(std::string_view a, std::string_view b) {
    const auto pa = split_path(a), pb = split_path(b);
    if (pa.size() != pb.size()) return false;
    for (std::size_t i = 0; i < pa.size(); ++i)
        if (ascii_lower(normalise_iso_name(pa[i])) != ascii_lower(normalise_iso_name(pb[i]))) return false;
    return true;
}

// ---- FileSource ----

FileSource::FileSource(const fs::path& path) : f_(path, std::ios::binary) {
    if (!f_) throw std::runtime_error("cannot open " + path.string());
    size_ = file_size_or_throw(path);
}

void FileSource::read(std::uint64_t offset, void* dst, std::size_t n) const {
    if (offset > size_ || n > size_ - offset) throw FormatError("read past end of file");
    f_.clear();
    f_.seekg(std::streamoff(offset));
    f_.read(static_cast<char*>(dst), std::streamsize(n));
    if (std::size_t(f_.gcount()) != n) throw FormatError("short read");
}

// ---- DiscImage ----

DiscImage::DiscImage(DiscImage&&) noexcept = default;
DiscImage& DiscImage::operator=(DiscImage&&) noexcept = default;
DiscImage::~DiscImage() = default;

std::vector<Track> DiscImage::parse_mds(std::span<const std::uint8_t> d, const fs::path& mdf, std::uint64_t mdf_size) {
    // Alcohol 120% descriptor: header "MEDIA DESCRIPTOR", session count at 0x14, session blocks
    // (24 bytes) at the u32 at 0x50, track blocks of 80 bytes, per-track extra block (pregap, length).
    auto need = [&](std::size_t off, std::size_t n) {
        if (off > d.size() || n > d.size() - off) throw FormatError("MDS: truncated");
    };
    need(0, 0x58);
    if (std::memcmp(d.data(), "MEDIA DESCRIPTOR", 16) != 0) throw FormatError("not an MDS file");
    const int sessions = le16(&d[0x14]);
    const std::uint32_t sess = le32(&d[0x50]);
    std::vector<Track> out;
    for (int s = 0; s < sessions; ++s) {
        need(sess + s * 24, 24);
        const std::uint8_t* b = &d[sess + s * 24];
        const int blocks = b[10];
        const std::uint32_t toff = le32(b + 20);
        for (int i = 0; i < blocks; ++i) {
            need(toff + i * 80, 80);
            const std::uint8_t* t = &d[toff + i * 80];
            const int point = t[4];
            if (point < 1 || point > 99) continue;
            Track tr;
            tr.number = point;
            const int m = t[0] & 0x0f;
            tr.audio = m == 9;
            tr.mode = m >= 0xb && m <= 0xd ? 2 : 1;
            tr.sector_size = le16(t + 0x10);
            tr.lba = le32(t + 0x24);
            tr.file_offset = le64(t + 0x28);
            tr.file = mdf;
            if (const std::uint32_t extra = le32(t + 0x0c); extra) {
                need(extra, 8);
                tr.pregap = le32(&d[extra]);
                tr.sectors = le32(&d[extra + 4]);
            }
            if (tr.sector_size == 0) throw FormatError("MDS: zero sector size");
            tr.data_offset = tr.audio ? 0 : user_offset(tr.sector_size, tr.mode);
            out.push_back(tr);
        }
    }
    std::sort(out.begin(), out.end(), [](const Track& a, const Track& b) { return a.file_offset < b.file_offset; });
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint64_t end = i + 1 < out.size() ? out[i + 1].file_offset : mdf_size;
        const std::uint64_t stored = end > out[i].file_offset ? (end - out[i].file_offset) / out[i].sector_size : 0;
        if (out[i].sectors == 0 || out[i].sectors > stored) out[i].sectors = std::uint32_t(stored);
    }
    std::sort(out.begin(), out.end(), [](const Track& a, const Track& b) { return a.number < b.number; });
    return out;
}

std::vector<Track> DiscImage::parse_cue(std::string_view cue, const fs::path& dir,
                                        const std::function<std::uint64_t(const fs::path&)>& file_size) {
    struct Raw {
        Track t;
        std::int64_t index0 = -1, index1 = -1; // frames within the file
        std::uint32_t pregap = 0;              // PREGAP (not stored)
    };
    std::vector<Raw> raw;
    fs::path file;
    auto msf = [](std::string_view s) -> std::int64_t {
        int m = 0, sec = 0, f = 0;
        if (std::sscanf(std::string(s).c_str(), "%d:%d:%d", &m, &sec, &f) != 3) throw FormatError("CUE: bad time");
        return (std::int64_t(m) * 60 + sec) * 75 + f;
    };
    std::istringstream in{std::string(cue)};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;
        cmd = ascii_lower(cmd);
        if (cmd == "file") {
            std::string rest;
            std::getline(ls, rest);
            const auto q1 = rest.find('"'), q2 = rest.rfind('"');
            std::string name;
            if (q1 != std::string::npos && q2 > q1) {
                name = rest.substr(q1 + 1, q2 - q1 - 1);
            } else {
                std::istringstream rs(rest);
                rs >> name;
            }
            file = dir / name;
            if (!fs::exists(file)) // CUE files written on Windows often disagree in case
                if (auto s = sibling(file, ascii_lower(file.extension().string()))) file = *s;
        } else if (cmd == "track") {
            int n = 0;
            std::string type;
            ls >> n >> type;
            type = ascii_lower(type);
            Raw r;
            r.t.number = n;
            r.t.file = file;
            if (type == "audio") r.t.audio = true, r.t.sector_size = 2352;
            else if (type == "mode1/2048") r.t.mode = 1, r.t.sector_size = 2048;
            else if (type == "mode1/2352") r.t.mode = 1, r.t.sector_size = 2352;
            else if (type == "mode2/2336") r.t.mode = 2, r.t.sector_size = 2336;
            else if (type == "mode2/2352") r.t.mode = 2, r.t.sector_size = 2352;
            else throw FormatError("CUE: unsupported track type " + type);
            r.t.data_offset = r.t.audio ? 0 : user_offset(r.t.sector_size, r.t.mode);
            raw.push_back(r);
        } else if (cmd == "index" && !raw.empty()) {
            int n = 0;
            std::string t;
            ls >> n >> t;
            if (n == 0) raw.back().index0 = msf(t);
            if (n == 1) raw.back().index1 = msf(t);
        } else if (cmd == "pregap" && !raw.empty()) {
            std::string t;
            ls >> t;
            raw.back().pregap = std::uint32_t(msf(t));
        }
    }
    std::vector<Track> out;
    std::uint32_t file_base = 0, gaps = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        Raw& r = raw[i];
        if (r.index1 < 0) throw FormatError("CUE: track without INDEX 01");
        if (i > 0 && r.t.file != raw[i - 1].t.file) {
            file_base += std::uint32_t(file_size(raw[i - 1].t.file) / raw[i - 1].t.sector_size);
        }
        gaps += r.pregap;
        r.t.file_offset = std::uint64_t(r.index1) * r.t.sector_size;
        r.t.lba = file_base + gaps + std::uint32_t(r.index1);
        r.t.pregap = r.pregap + std::uint32_t(r.index0 >= 0 ? r.index1 - r.index0 : 0);
        std::uint64_t end_frame;
        if (i + 1 < raw.size() && raw[i + 1].t.file == r.t.file)
            end_frame = std::uint64_t(raw[i + 1].index0 >= 0 ? raw[i + 1].index0 : raw[i + 1].index1);
        else
            end_frame = file_size(r.t.file) / r.t.sector_size;
        r.t.sectors = end_frame > std::uint64_t(r.index1) ? std::uint32_t(end_frame - r.index1) : 0;
        out.push_back(r.t);
    }
    return out;
}

DiscImage DiscImage::open(const fs::path& path) {
    DiscImage img;
    img.path_ = path;
    const std::string ext = ascii_lower(path.extension().string());
    std::optional<fs::path> mds, cue;
    if (ext == ".mds") mds = path;
    else if (ext == ".mdf") mds = sibling(path, ".mds");
    else if (ext == ".cue") cue = path;
    else if (ext == ".bin" || ext == ".img") cue = sibling(path, ".cue");

    if (mds) {
        const auto mdf = ext == ".mdf" ? std::optional(path) : sibling(*mds, ".mdf");
        if (!mdf) throw std::runtime_error("no .mdf next to " + mds->string());
        img.tracks_ = parse_mds(read_file(*mds), *mdf, file_size_or_throw(*mdf));
        img.format_ = "MDS";
    } else if (cue) {
        const Bytes text = read_file(*cue);
        img.tracks_ = parse_cue({reinterpret_cast<const char*>(text.data()), text.size()}, cue->parent_path(),
                                file_size_or_throw);
        img.format_ = "CUE";
    } else {
        // A single data track: 2048-byte sectors, or raw sectors when it starts with a sync pattern.
        const std::uint64_t size = file_size_or_throw(path);
        Track t;
        t.number = 1, t.mode = 1, t.file = path, t.sector_size = 2048;
        std::uint8_t head[2448 + 16] = {};
        std::ifstream f(path, std::ios::binary);
        f.read(reinterpret_cast<char*>(head), sizeof head);
        if (std::memcmp(head, kSync, 12) == 0) {
            t.sector_size = std::memcmp(head + 2352, kSync, 12) == 0 ? 2352
                            : std::memcmp(head + 2448, kSync, 12) == 0 ? 2448
                                                                       : 2352;
            t.mode = head[15] == 2 ? 2 : 1;
            img.format_ = "raw";
        } else {
            img.format_ = "ISO";
        }
        t.data_offset = user_offset(t.sector_size, t.mode);
        t.sectors = std::uint32_t(size / t.sector_size);
        img.tracks_.push_back(t);
    }
    if (img.tracks_.empty()) throw FormatError("no tracks in " + path.string());
    img.finish();
    return img;
}

void DiscImage::finish() {
    // The file system lives on the first data track with an ISO 9660 descriptor at sector 16; probe
    // the user-data offset (mode 2 images are sometimes mislabelled).
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        Track& t = tracks_[i];
        if (t.audio || t.sectors <= 16) continue;
        const std::uint32_t candidates[] = {t.data_offset, 16, 24, 0, 8};
        std::vector<std::uint8_t> s(t.sector_size);
        std::ifstream& f = stream(t.file);
        f.clear();
        f.seekg(std::streamoff(t.file_offset + 16ull * t.sector_size));
        f.read(reinterpret_cast<char*>(s.data()), std::streamsize(s.size()));
        if (std::size_t(f.gcount()) != s.size()) continue;
        for (std::uint32_t off : candidates) {
            if (off + kSector > t.sector_size) continue;
            if (std::memcmp(&s[off + 1], "CD001", 5) == 0) {
                t.data_offset = off;
                data_track_ = int(i);
                return;
            }
        }
    }
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        if (!tracks_[i].audio) {
            data_track_ = int(i);
            return;
        }
}

const Track* DiscImage::track(int number) const {
    for (const auto& t : tracks_)
        if (t.number == number) return &t;
    return nullptr;
}

const Track* DiscImage::data_track() const { return data_track_ >= 0 ? &tracks_[std::size_t(data_track_)] : nullptr; }

std::vector<const Track*> DiscImage::audio_tracks() const {
    std::vector<const Track*> r;
    for (const auto& t : tracks_)
        if (t.audio) r.push_back(&t);
    return r;
}

std::ifstream& DiscImage::stream(const fs::path& file) const {
    auto& s = files_[file];
    if (!s) {
        s = std::make_unique<std::ifstream>(file, std::ios::binary);
        if (!*s) throw std::runtime_error("cannot open " + file.string());
    }
    return *s;
}

void DiscImage::read_data(std::uint32_t lba, std::uint32_t count, std::uint8_t* dst) const {
    constexpr std::uint32_t kRun = 32;
    while (count) {
        if (lba >= cache_lba_ && lba < cache_lba_ + cache_count_) {
            const std::uint32_t n = std::min(count, cache_lba_ + cache_count_ - lba);
            std::memcpy(dst, &cache_[std::size_t(lba - cache_lba_) * kSector], n * kSector);
            dst += n * kSector, lba += n, count -= n;
            continue;
        }
        const Track* t = nullptr;
        for (const auto& c : tracks_)
            if (!c.audio && lba >= c.lba && lba < c.lba + c.sectors) t = &c;
        if (!t) throw FormatError("sector " + std::to_string(lba) + " is outside the data tracks");
        const std::uint32_t rel = lba - t->lba, start = rel - rel % kRun;
        const std::uint32_t n = std::min(kRun, t->sectors - start);
        std::ifstream& f = stream(t->file);
        raw_.resize(std::size_t(n) * t->sector_size);
        f.clear();
        f.seekg(std::streamoff(t->file_offset + std::uint64_t(start) * t->sector_size));
        f.read(reinterpret_cast<char*>(raw_.data()), std::streamsize(raw_.size()));
        if (std::size_t(f.gcount()) != raw_.size()) throw FormatError("image truncated at sector " + std::to_string(lba));
        cache_.resize(std::size_t(kRun) * kSector);
        for (std::uint32_t i = 0; i < n; ++i)
            std::memcpy(&cache_[std::size_t(i) * kSector], &raw_[std::size_t(i) * t->sector_size + t->data_offset], kSector);
        cache_lba_ = t->lba + start, cache_count_ = n;
    }
}

void DiscImage::read_audio(const Track& t, std::uint32_t first, std::uint32_t count, std::uint8_t* dst) const {
    if (first > t.sectors || count > t.sectors - first) throw FormatError("audio read past the end of the track");
    std::ifstream& f = stream(t.file);
    f.clear();
    f.seekg(std::streamoff(t.file_offset + std::uint64_t(first) * t.sector_size));
    if (t.sector_size == kAudioSector) {
        f.read(reinterpret_cast<char*>(dst), std::streamsize(std::size_t(count) * kAudioSector));
        if (std::size_t(f.gcount()) != std::size_t(count) * kAudioSector) throw FormatError("image truncated");
        return;
    }
    raw_.resize(std::size_t(count) * t.sector_size);
    f.read(reinterpret_cast<char*>(raw_.data()), std::streamsize(raw_.size()));
    if (std::size_t(f.gcount()) != raw_.size()) throw FormatError("image truncated");
    for (std::uint32_t i = 0; i < count; ++i)
        std::memcpy(dst + std::size_t(i) * kAudioSector, &raw_[std::size_t(i) * t.sector_size], kAudioSector);
}

// ---- IsoFs ----

namespace {

IsoEntry record_entry(const std::uint8_t* r, bool joliet) {
    IsoEntry e;
    const std::uint32_t lba = le32(r + 2), size = le32(r + 10);
    e.dir = r[25] & 2;
    e.size = size;
    e.extents.emplace_back(lba, size);
    const int nlen = r[32];
    e.name = joliet ? utf16be_to_utf8(r + 33, std::size_t(nlen))
                    : cp1251_to_utf8({reinterpret_cast<const char*>(r + 33), std::size_t(nlen)});
    e.name = normalise_iso_name(e.name);
    return e;
}

} // namespace

IsoFs::IsoFs(const DiscImage& disc) : disc_(disc) {
    const Track* t = disc.data_track();
    if (!t) throw FormatError("no data track");
    std::uint8_t v[kSector];
    std::optional<IsoEntry> pvd_root, joliet_root;
    for (std::uint32_t lba = t->lba + 16; lba < t->lba + 64; ++lba) {
        disc.read_data(lba, 1, v);
        if (std::memcmp(v + 1, "CD001", 5) != 0) break;
        if (v[0] == 255) break;
        if (v[0] == 1 && !pvd_root) {
            pvd_root = record_entry(v + 156, false);
            volume_id_.assign(reinterpret_cast<const char*>(v + 40), 32);
            while (!volume_id_.empty() && volume_id_.back() == ' ') volume_id_.pop_back();
        }
        if (v[0] == 2 && v[88] == '%' && v[89] == '/' && (v[90] == '@' || v[90] == 'C' || v[90] == 'E'))
            joliet_root = record_entry(v + 156, true);
    }
    if (!pvd_root) throw FormatError("no ISO 9660 volume");
    joliet_ = joliet_root.has_value();
    root_ = joliet_ ? *joliet_root : *pvd_root;
    root_.name.clear();
}

std::vector<IsoEntry> IsoFs::list(const IsoEntry& dir) const {
    std::vector<IsoEntry> out;
    const Bytes data = read_all(dir);
    bool continues = false; // the previous record was a non-final extent of a multi-extent file
    for (std::size_t i = 0; i < data.size();) {
        const std::size_t n = data[i];
        if (n == 0) {
            i = (i / kSector + 1) * kSector;
            continue;
        }
        if (n < 34 || i + n > data.size()) throw FormatError("ISO: bad directory record");
        const std::uint8_t* r = &data[i];
        i += n;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue; // "." and ".."
        IsoEntry e = record_entry(r, joliet_);
        if (continues && !out.empty() && out.back().name == e.name) {
            out.back().size += e.size;
            out.back().extents.push_back(e.extents[0]);
        } else {
            out.push_back(std::move(e));
        }
        continues = r[25] & 0x80;
    }
    return out;
}

std::optional<IsoEntry> IsoFs::find(std::string_view path) const {
    IsoEntry cur = root_;
    for (const std::string& part : split_path(path)) {
        if (!cur.dir) return std::nullopt;
        const std::string want = ascii_lower(normalise_iso_name(part));
        bool found = false;
        for (IsoEntry& e : list(cur))
            if (ascii_lower(e.name) == want) {
                cur = std::move(e);
                found = true;
                break;
            }
        if (!found) return std::nullopt;
    }
    return cur;
}

void IsoFs::walk(const IsoEntry& dir, const std::function<void(const std::string&, const IsoEntry&)>& fn) const {
    struct Item {
        std::string path;
        IsoEntry e;
    };
    std::vector<Item> stack{{"", dir}};
    while (!stack.empty()) {
        Item it = std::move(stack.back());
        stack.pop_back();
        auto entries = list(it.e);
        for (auto e = entries.rbegin(); e != entries.rend(); ++e) {
            std::string p = it.path.empty() ? e->name : it.path + "/" + e->name;
            if (e->dir) stack.push_back({p, *e});
        }
        for (const IsoEntry& e : entries) fn(it.path.empty() ? e.name : it.path + "/" + e.name, e);
    }
}

void IsoFs::read(const IsoEntry& file, std::uint64_t offset, void* dst, std::size_t n) const {
    if (offset > file.size || n > file.size - offset) throw FormatError("ISO: read past end of " + file.name);
    auto* out = static_cast<std::uint8_t*>(dst);
    std::uint8_t sector[kSector];
    std::uint64_t ext_start = 0;
    for (const auto& [lba, bytes] : file.extents) {
        const std::uint64_t ext_end = ext_start + bytes;
        while (n && offset < ext_end) {
            const std::uint64_t in_ext = offset - ext_start;
            const auto s = std::uint32_t(lba + in_ext / kSector);
            const std::size_t inner = std::size_t(in_ext % kSector);
            const std::size_t take = std::size_t(std::min<std::uint64_t>({n, kSector - inner, ext_end - offset}));
            if (inner == 0 && n >= kSector && ext_end - offset >= kSector) {
                // Whole sectors straight into the destination.
                const auto whole = std::uint32_t(std::min<std::uint64_t>(n, ext_end - offset) / kSector);
                disc_.read_data(s, whole, out);
                out += std::size_t(whole) * kSector, offset += std::uint64_t(whole) * kSector, n -= std::size_t(whole) * kSector;
                continue;
            }
            disc_.read_data(s, 1, sector);
            std::memcpy(out, sector + inner, take);
            out += take, offset += take, n -= take;
        }
        ext_start = ext_end;
    }
    if (n) throw FormatError("ISO: extents shorter than " + file.name);
}

Bytes IsoFs::read_all(const IsoEntry& file) const {
    Bytes b(file.size);
    if (!b.empty()) read(file, 0, b.data(), b.size());
    return b;
}

} // namespace parkan::install
