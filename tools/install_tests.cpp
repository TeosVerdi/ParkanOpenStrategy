// Disc images and game data import (src/install): hashes, inflate, MDS / CUE / ISO 9660 + Joliet on
// synthetic images built in memory, an InstallShield 5 cabinet built in memory, and — when they are
// on this machine — edition detection on the GOG folders and the user's disc images. Linked into
// parkan-tests.
//
// End-to-end (slow: unpacks both discs, ~1.2 GB written): PARKAN_INSTALL_E2E=<scratch dir> makes
// install_unit_tests() import both disc images there and compare the 2001 import with
// ~/parkan-re/editions/original-2001/Program_and_data_files byte for byte.
// Overrides: PARKAN_DISC_2001, PARKAN_DISC_2002 (image paths), PARKAN_GOG_RU, PARKAN_GOG_EN (folders).
#include "core/bytes.hpp"
#include "core/paths.hpp"
#include "install/disc.hpp"
#include "install/edition.hpp"
#include "install/hash.hpp"
#include "install/inflate.hpp"
#include "install/install.hpp"
#include "install/iscab.hpp"
#include "install/source.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>

using namespace parkan;
using namespace parkan::install;
namespace fs = std::filesystem;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

Bytes text_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }

void put16(Bytes& b, std::size_t at, std::uint32_t v) { b[at] = std::uint8_t(v), b[at + 1] = std::uint8_t(v >> 8); }
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(v >> (8 * i));
}
void put32be(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(v >> (24 - 8 * i));
}
void put_both32(Bytes& b, std::size_t at, std::uint32_t v) { put32(b, at, v), put32be(b, at + 4, v); }

void write_bytes(const fs::path& p, const Bytes& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

fs::path temp_dir() {
    const fs::path d = fs::temp_directory_path() / ("parkan-install-tests-" + std::to_string(::getpid()));
    fs::create_directories(d);
    return d;
}

fs::path env_or(const char* var, const fs::path& fallback) {
    const char* v = std::getenv(var);
    return v && *v ? fs::path(v) : fallback;
}

fs::path home() { return env_or("HOME", "/"); }
const fs::path kCollection = "Downloads/Собрание Parkan/2. Parkan - Iron Strategy Original + 1";
fs::path disc_2001() {
    return env_or("PARKAN_DISC_2001", home() / kCollection / "Parkan. Железная стратегия" / "PARKAN_Iron_Strategy_(Original).mds");
}
fs::path disc_2002() {
    return env_or("PARKAN_DISC_2002", home() / kCollection / "Parkan. Железная стратегия 1" / "PARKAN_Iron_Strategy_(Part1).mds");
}
const fs::path kGog = "Library/Application Support/CrossOver/Bottles/ParkanIS/drive_c/GOG Games";
fs::path gog_ru() { return env_or("PARKAN_GOG_RU", home() / kGog / "Parkan - Iron Strategy"); }
fs::path gog_en() { return env_or("PARKAN_GOG_EN", home() / kGog / "Parkan - Iron Strategy - ENG"); }

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---- hashes, inflate ----

void test_hashes() {
    std::printf("install: hashes\n");
    const Bytes abc = text_bytes("abc");
    CHECK(to_hex(sha256(abc)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Md5 m;
    m.update(abc);
    CHECK(to_hex(m.final()) == "900150983cd24fb0d6963f7d28e17f72");
    // Streaming in odd pieces equals one shot.
    Bytes big(100'000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::uint8_t(i * 31 + (i >> 7));
    Sha256 s;
    for (std::size_t i = 0; i < big.size(); i += 777) s.update({big.data() + i, std::min<std::size_t>(777, big.size() - i)});
    CHECK(s.final() == sha256(big));
    CHECK(digest_prefix(sha256(abc)) == 0xba7816bf8f01cfeaull);
}

// zlib raw deflate (wbits -15): a fixed-Huffman stream ending in a sync flush (no final block, as in
// InstallShield chunks), a stored block, and a dynamic-Huffman block.
constexpr std::uint8_t kDeflateFixedSync[] = {0x0a, 0x48, 0x2c, 0xca, 0x4e, 0xcc, 0xb3, 0x52, 0xf0, 0x2c, 0xca, 0xcf,
                                              0x53, 0x08, 0x2e, 0x29, 0x4a, 0x2c, 0x49, 0x4d, 0xaf, 0xd4, 0x53, 0x08,
                                              0x18, 0x15, 0x1e, 0x7a, 0xc2, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
constexpr std::uint8_t kDeflateStored[] = {0x01, 0x0c, 0x00, 0xf3, 0xff, 0x73, 0x74, 0x6f, 0x72,
                                           0x65, 0x64, 0x20, 0x62, 0x6c, 0x6f, 0x63, 0x6b};
constexpr std::uint8_t kDeflateDynamic[] = {
    0x75, 0xd1, 0x6b, 0x0a, 0x83, 0x40, 0x0c, 0x04, 0xe0, 0xab, 0xe4, 0x6a, 0x3e, 0x62, 0x5d, 0xab, 0xa6, 0x75, 0x7d, 0xb5,
    0xa7, 0x6f, 0xdc, 0x90, 0x66, 0xa0, 0xf4, 0x87, 0x03, 0x42, 0x16, 0x3e, 0x66, 0xd6, 0x9e, 0xe9, 0xb9, 0xa5, 0xe6, 0x4e,
    0xf5, 0x22, 0x96, 0xc7, 0x4c, 0x9d, 0x9c, 0x64, 0x39, 0x6c, 0xd3, 0x23, 0x93, 0x58, 0xee, 0xbc, 0xd0, 0xda, 0xb3, 0x25,
    0x8d, 0xd5, 0xfb, 0x45, 0xad, 0xa5, 0xdc, 0xe8, 0xe8, 0xd3, 0x68, 0xc9, 0xb4, 0x48, 0x2d, 0xab, 0xa5, 0x3e, 0xeb, 0xf4,
    0xdc, 0x92, 0xa9, 0x19, 0xab, 0x99, 0x2c, 0xeb, 0x2d, 0x8d, 0xad, 0x67, 0x95, 0x59, 0x2f, 0x67, 0xcb, 0x72, 0x39, 0x89,
    0xa5, 0xfe, 0xce, 0xe4, 0xc2, 0x62, 0x73, 0xa1, 0xd9, 0x06, 0xb0, 0xb9, 0xb0, 0xd8, 0x5c, 0x58, 0x6c, 0x2e, 0x2c, 0x36,
    0x17, 0x9a, 0x2d, 0x83, 0xcd, 0x85, 0xbf, 0x36, 0x17, 0x96, 0x4b, 0x17, 0xea, 0xef, 0xf5, 0x45, 0x83, 0x2a, 0x8b, 0x06,
    0x55, 0x16, 0x0d, 0xaa, 0x2c, 0x1a, 0x54, 0x59, 0x34, 0xa8, 0xb2, 0x68, 0x50, 0x65, 0xd1, 0xa0, 0x3e, 0x88, 0x06, 0x55,
    0x16, 0x0d, 0xaa, 0x2c, 0x94, 0x2a, 0x6b, 0xd1, 0x17, 0x0d, 0x5e, 0xb6, 0x92, 0xb8, 0xf2, 0x81, 0x2b, 0x0f, 0xb8, 0xf2,
    0x8e, 0x2b, 0x13, 0xae, 0x2c, 0xb8, 0x32, 0xe3, 0xca, 0x19, 0x57, 0x66, 0x5c, 0xb9, 0xc6, 0x26, 0x2b, 0x5c, 0x99, 0x70,
    0x65, 0xf9, 0x4a, 0x71, 0xe3, 0xf3, 0xef, 0xc6, 0x3d, 0x6e, 0x4c, 0xb8, 0x71, 0xc2, 0x8d, 0xe5, 0x03};

std::string fixed_plain() {
    std::string s;
    for (int i = 0; i < 20; ++i) s += "Parkan: Iron Strategy. ";
    return s;
}
std::string dynamic_plain() {
    const std::string words = "the quick brown fox jumps over the lazy dog while robots of the clan build bases on the moon ";
    std::string s;
    for (std::size_t i = 0; i < 60; ++i) s += words.substr(i * 7 % words.size(), 13);
    return s;
}

std::string inflate_text(std::span<const std::uint8_t> in) {
    Bytes out;
    inflate_raw(in, out);
    return {out.begin(), out.end()};
}

void test_inflate() {
    std::printf("install: inflate\n");
    CHECK(inflate_text(kDeflateFixedSync) == fixed_plain());
    CHECK(inflate_text(kDeflateStored) == "stored block");
    CHECK(inflate_text(kDeflateDynamic) == dynamic_plain());
    // Each call is its own stream; appended output.
    Bytes out = text_bytes(">");
    inflate_raw(kDeflateStored, out);
    CHECK(std::string(out.begin(), out.end()) == ">stored block");
    // Corruption and limits are errors.
    bool threw = false;
    try {
        Bytes o;
        inflate_raw(std::span(kDeflateDynamic).first(100), o);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Bytes o;
        inflate_raw(kDeflateFixedSync, o, 100);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- synthetic CD images ----

// A record of an ISO 9660 directory.
void dir_record(Bytes& d, const Bytes& name, std::uint32_t lba, std::uint32_t size, bool dir) {
    const std::size_t n = 33 + name.size() + (name.size() % 2 == 0);
    const std::size_t at = d.size();
    d.resize(at + n);
    d[at] = std::uint8_t(n);
    put_both32(d, at + 2, lba);
    put_both32(d, at + 10, size);
    d[at + 25] = dir ? 2 : 0;
    d[at + 32] = std::uint8_t(name.size());
    std::memcpy(&d[at + 33], name.data(), name.size());
}

Bytes joliet_name(std::string_view s) {
    Bytes b;
    for (char c : s) b.push_back(0), b.push_back(std::uint8_t(c));
    return b;
}

constexpr std::uint32_t kIsoSectors = 26;
const std::string kSmall = "6 bytes";

// 2048-byte sectors: PVD (16), Joliet SVD (17), terminator (18), root / SUBDIR for both trees
// (19-22), DESCR.;1 (23), SUBDIR/BIG.BIN;1 = 3000 bytes (24-25). Joliet: "descr.", "SubDir/Big.bin".
// `primary` / `joliet` rename the small file.
Bytes make_iso(std::string_view primary = "DESCR.;1", std::string_view joliet_small = "descr.;1") {
    Bytes img(kIsoSectors * kSector);
    auto sector = [&](std::uint32_t s) { return s * kSector; };
    Bytes big(3000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::uint8_t(i % 251);
    auto tree = [&](std::uint32_t root, std::uint32_t sub, bool joliet) {
        Bytes r, s;
        dir_record(r, {0}, root, kSector, true);
        dir_record(r, {1}, root, kSector, true);
        dir_record(r, joliet ? joliet_name(joliet_small) : text_bytes(primary), 23, std::uint32_t(kSmall.size()), false);
        dir_record(r, joliet ? joliet_name("SubDir") : text_bytes("SUBDIR"), sub, kSector, true);
        dir_record(s, {0}, sub, kSector, true);
        dir_record(s, {1}, root, kSector, true);
        dir_record(s, joliet ? joliet_name("Big.bin") : text_bytes("BIG.BIN;1"), 24, 3000, false);
        std::memcpy(&img[sector(root)], r.data(), r.size());
        std::memcpy(&img[sector(sub)], s.data(), s.size());
    };
    tree(19, 20, false);
    tree(21, 22, true);
    auto vd = [&](std::uint32_t s, std::uint8_t type, std::uint32_t root) {
        std::uint8_t* v = &img[sector(s)];
        v[0] = type;
        std::memcpy(v + 1, "CD001", 5);
        v[6] = 1;
        if (type == 255) return;
        std::memset(v + 40, ' ', 32);
        std::memcpy(v + 40, "PARKAN_TEST", 11);
        Bytes rr;
        dir_record(rr, {0}, root, kSector, true);
        std::memcpy(v + 156, rr.data(), 34);
        if (type == 2) std::memcpy(v + 88, "%/E", 3);
    };
    vd(16, 1, 19);
    vd(17, 2, 21);
    vd(18, 255, 0);
    std::memcpy(&img[sector(23)], kSmall.data(), kSmall.size());
    std::memcpy(&img[sector(24)], big.data(), big.size());
    return img;
}

// The same sectors as raw mode-1 sectors (sync, header, data, zero EDC/ECC), `stride` 2352 or 2448
// (+96 bytes of subchannel), followed by `audio` sectors of CD-DA whose bytes encode their position.
Bytes make_raw(const Bytes& iso, std::uint32_t stride, std::uint32_t audio) {
    const std::uint32_t data = std::uint32_t(iso.size() / kSector);
    Bytes raw(std::size_t(data + audio) * stride, 0);
    for (std::uint32_t s = 0; s < data; ++s) {
        std::uint8_t* p = &raw[std::size_t(s) * stride];
        p[0] = 0;
        std::memset(p + 1, 0xff, 10);
        p[11] = 0;
        p[15] = 1; // mode 1
        std::memcpy(p + 16, &iso[std::size_t(s) * kSector], kSector);
        if (stride == 2448) std::memset(p + 2352, 0xaa, 96);
    }
    for (std::uint32_t s = 0; s < audio; ++s) {
        std::uint8_t* p = &raw[std::size_t(data + s) * stride];
        for (std::size_t i = 0; i < kAudioSector; ++i) p[i] = std::uint8_t(s * 7 + i);
        if (stride == 2448) std::memset(p + 2352, 0x55, 96);
    }
    return raw;
}

// An Alcohol MDS: one session, track 1 mode 1 at LBA 0, track 2 audio after a 150-sector pregap
// (not stored), both with extra blocks.
Bytes make_mds(std::uint32_t stride, std::uint32_t data, std::uint32_t audio) {
    Bytes m(0x58 + 24 + 2 * 80 + 16, 0);
    std::memcpy(m.data(), "MEDIA DESCRIPTOR", 16);
    m[16] = 1, m[17] = 5;
    put16(m, 0x14, 1);
    put32(m, 0x50, 0x58);
    const std::size_t s = 0x58, t = s + 24, extra = t + 160;
    put32(m, s, std::uint32_t(-150));
    put32(m, s + 4, data + 150 + audio);
    put16(m, s + 8, 1);
    m[s + 10] = 2;
    put16(m, s + 12, 1), put16(m, s + 14, 2);
    put32(m, s + 20, std::uint32_t(t));
    for (int i = 0; i < 2; ++i) {
        const std::size_t o = t + std::size_t(i) * 80;
        m[o] = i == 0 ? 0xaa : 0xa9;
        m[o + 1] = stride == 2448 ? 8 : 0;
        m[o + 4] = std::uint8_t(i + 1);
        put32(m, o + 0x0c, std::uint32_t(extra + std::size_t(i) * 8));
        put16(m, o + 0x10, stride);
        put32(m, o + 0x24, i == 0 ? 0 : data + 150);
        put32(m, o + 0x28, i == 0 ? 0 : data * stride);
        put32(m, extra + std::size_t(i) * 8, 150);
        put32(m, extra + std::size_t(i) * 8 + 4, i == 0 ? data : audio);
    }
    return m;
}

void check_iso(const DiscImage& img, const char* what) {
    std::printf("install: ISO 9660 on %s\n", what);
    const IsoFs fs(img);
    CHECK(fs.joliet());
    CHECK(fs.volume_id() == "PARKAN_TEST");
    const auto root = fs.list(fs.root());
    CHECK(root.size() == 2 && root[0].name == "descr" && root[1].name == "SubDir" && root[1].dir);
    const auto small = fs.find("DESCR");
    CHECK(small && fs.read_all(*small) == text_bytes(kSmall));
    CHECK(fs.find("descr.;1").has_value());
    const auto big = fs.find("subdir\\BIG.BIN");
    CHECK(big && big->size == 3000);
    if (big) {
        const Bytes b = fs.read_all(*big);
        bool ok = b.size() == 3000;
        for (std::size_t i = 0; ok && i < b.size(); ++i) ok = b[i] == std::uint8_t(i % 251);
        CHECK(ok);
        std::uint8_t piece[10];
        fs.read(*big, 2045, piece, 10); // across the sector boundary
        CHECK(piece[0] == 2045 % 251 && piece[9] == 2054 % 251);
    }
    CHECK(!fs.find("subdir/missing").has_value());
    int files = 0;
    fs.walk(fs.root(), [&](const std::string& p, const IsoEntry& e) { files += !e.dir && (p == "descr" || p == "SubDir/Big.bin"); });
    CHECK(files == 2);
}

void test_disc_images() {
    const fs::path dir = temp_dir();
    const Bytes iso = make_iso();
    write_bytes(dir / "plain.iso", iso);
    {
        const DiscImage img = DiscImage::open(dir / "plain.iso");
        CHECK(img.format() == "ISO" && img.tracks().size() == 1 && img.tracks()[0].sectors == kIsoSectors);
        check_iso(img, "a plain .iso");
    }
    for (std::uint32_t stride : {2352u, 2448u}) {
        const std::uint32_t audio = 100;
        const std::string stem = "disc" + std::to_string(stride);
        write_bytes(dir / (stem + ".mdf"), make_raw(iso, stride, audio));
        write_bytes(dir / (stem + ".MDS"), make_mds(stride, kIsoSectors, audio)); // sibling found in any case
        const DiscImage img = DiscImage::open(dir / (stem + ".mdf"));
        CHECK(img.format() == "MDS" && img.tracks().size() == 2);
        const Track* t1 = img.track(1);
        const Track* t2 = img.track(2);
        CHECK(t1 && !t1->audio && t1->sector_size == stride && t1->data_offset == 16 && t1->sectors == kIsoSectors);
        CHECK(t2 && t2->audio && t2->lba == kIsoSectors + 150 && t2->sectors == audio && t2->pregap == 150);
        CHECK(img.audio_tracks().size() == 1);
        check_iso(img, stride == 2352 ? "an MDF/MDS, 2352-byte sectors" : "an MDF/MDS, 2448-byte sectors");
        if (t2) {
            Bytes a(2 * kAudioSector);
            img.read_audio(*t2, 5, 2, a.data());
            CHECK(a[0] == std::uint8_t(5 * 7) && a[kAudioSector] == std::uint8_t(6 * 7) && a[kAudioSector - 1] == std::uint8_t(5 * 7 + 2351));
        }
    }
    // BIN/CUE with the audio track's 2-sector pregap stored (INDEX 00), and a raw .iso sniffed by its sync.
    write_bytes(dir / "game.bin", make_raw(iso, 2352, 50));
    const std::string cue = "FILE \"GAME.BIN\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n"
                            "  TRACK 02 AUDIO\r\n    INDEX 00 00:00:26\r\n    INDEX 01 00:00:28\r\n";
    write_bytes(dir / "game.cue", text_bytes(cue));
    {
        const DiscImage img = DiscImage::open(dir / "game.cue");
        CHECK(img.format() == "CUE" && img.tracks().size() == 2);
        const Track* t2 = img.track(2);
        CHECK(img.track(1) && img.track(1)->sectors == 26);
        CHECK(t2 && t2->audio && t2->lba == 28 && t2->sectors == 48 && t2->pregap == 2 && t2->file_offset == 28u * 2352);
        check_iso(img, "a BIN/CUE");
    }
    write_bytes(dir / "raw.iso", make_raw(iso, 2352, 0));
    {
        const DiscImage img = DiscImage::open(dir / "raw.iso");
        CHECK(img.format() == "raw" && img.tracks()[0].sector_size == 2352);
        check_iso(img, "a raw .iso");
    }
    CHECK(normalise_iso_name("DESCR.;1") == "DESCR" && normalise_iso_name("readme.htm;1") == "readme.htm");
    CHECK(path_iequals("MISSIONS\\Autodemo.00\\DESCR.", "missions/autodemo.00/descr"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- synthetic InstallShield 5 cabinet ----

class MemSource : public ByteSource {
public:
    explicit MemSource(Bytes b) : b_(std::move(b)) {}
    std::uint64_t size() const override { return b_.size(); }
    void read(std::uint64_t off, void* dst, std::size_t n) const override {
        if (off + n > b_.size()) throw FormatError("past end");
        std::memcpy(dst, b_.data() + off, n);
    }

private:
    Bytes b_;
};

// Inverse of is_deobfuscate.
void obfuscate(std::span<std::uint8_t> d) {
    std::uint32_t seed = 0;
    for (std::uint8_t& b : d) {
        const std::uint8_t x = std::uint8_t(b + seed++ % 0x47);
        b = std::uint8_t(std::uint8_t(x << 2 | x >> 6) ^ 0xd5);
    }
}

// Common header at 0, cabinet descriptor at 0x200 (group and component offset tables up to +0x276),
// file group list at +0x300, group descriptor at +0x320 (name at +0x380), file table at +0x400
// (directory "MISSIONS\Sub", three descriptors, the last invalid), file data from 0x1000:
// "readme.txt" stored + obfuscated, "data.bin" two deflate chunks.
Bytes make_cab() {
    Bytes c(0x2000, 0);
    put32(c, 0, 0x28635349);
    put32(c, 4, 0x01005100);
    put32(c, 12, 0x200);
    put32(c, 16, 0x7d3);
    const std::size_t cd = 0x200, ft = 0x400;
    put32(c, cd + 0x0c, ft);
    put32(c, cd + 0x1c, 1); // directories
    put32(c, cd + 0x28, 3); // files
    put32(c, cd + 0x3e, 0x300);
    put32(c, cd + 0x300 + 4, 0x320);
    put32(c, cd + 0x320, 0x380);
    put32(c, cd + 0x320 + 0x4c, 0);
    put32(c, cd + 0x320 + 0x50, 2);
    std::memcpy(&c[cd + 0x380], "Program and data files", 22);
    const std::size_t t = cd + ft;
    const std::uint32_t entries[] = {0x40, 0x80, 0xb0, 0xe0};
    for (int i = 0; i < 4; ++i) put32(c, t + 4 * std::size_t(i), entries[i]);
    std::memcpy(&c[t + 0x40], "MISSIONS\\Sub", 12);
    std::memcpy(&c[t + 0x110], "readme.txt", 10);
    std::memcpy(&c[t + 0x120], "data.bin", 8);
    std::memcpy(&c[t + 0x130], "gone.bin", 8);

    const std::string readme = "Parkan, 2001";
    Bytes stored(readme.begin(), readme.end());
    obfuscate(stored);
    std::memcpy(&c[0x1000], stored.data(), stored.size());
    Bytes chunks;
    for (std::span<const std::uint8_t> chunk : {std::span<const std::uint8_t>(kDeflateFixedSync), std::span<const std::uint8_t>(kDeflateDynamic)}) {
        chunks.push_back(std::uint8_t(chunk.size())), chunks.push_back(std::uint8_t(chunk.size() >> 8));
        chunks.insert(chunks.end(), chunk.begin(), chunk.end());
    }
    std::memcpy(&c[0x1100], chunks.data(), chunks.size());
    auto desc = [&](std::uint32_t at, std::uint32_t name, std::uint16_t flags, std::uint32_t expanded, std::uint32_t compressed,
                    std::uint32_t data) {
        put32(c, t + at, name);
        put32(c, t + at + 4, 0);
        put16(c, t + at + 8, flags);
        put32(c, t + at + 10, expanded);
        put32(c, t + at + 14, compressed);
        put32(c, t + at + 0x26, data);
    };
    desc(0x80, 0x110, 2, std::uint32_t(readme.size()), std::uint32_t(readme.size()), 0x1000);
    desc(0xb0, 0x120, 4, std::uint32_t(fixed_plain().size() + dynamic_plain().size()), std::uint32_t(chunks.size()), 0x1100);
    desc(0xe0, 0x130, 8, 10, 10, 0x1200);
    return c;
}

void test_cabinet() {
    std::printf("install: InstallShield 5 cabinet\n");
    const IsCabinet cab(std::make_shared<MemSource>(make_cab()));
    CHECK(cab.major_version() == 5);
    CHECK(cab.groups().size() == 1 && cab.groups()[0] == "Program and data files");
    CHECK(cab.files().size() == 2);
    const CabFile* readme = cab.find("missions/sub/README.TXT");
    CHECK(readme && readme->path() == "MISSIONS/Sub/readme.txt" && readme->group == "Program and data files");
    if (readme) CHECK(cab.extract(*readme) == text_bytes("Parkan, 2001"));
    if (const CabFile* data = cab.find("MISSIONS/Sub/data.bin")) {
        CHECK(cab.extract(*data) == text_bytes(fixed_plain() + dynamic_plain()));
        int pieces = 0;
        CHECK(!cab.extract(*data, [&](std::span<const std::uint8_t>) { return ++pieces < 1; })); // cancel
    } else {
        CHECK(!"data.bin missing");
    }
    CHECK(!cab.find("MISSIONS/Sub/gone.bin"));
}

void test_editions_table() {
    std::printf("install: editions\n");
    CHECK(edition_from_key("disc-2001") == Edition::Disc2001 && edition_from_key("x") == Edition::Unknown);
    CHECK(std::string(edition_info(Edition::GogEng).key) == "gog-en");
    CHECK(is_user_file("Iron_3D.ini") && is_user_file("ui_hq.man") && is_user_file("SAVE/saveslots.cfg") && !is_user_file("objects.dlb"));
    CHECK(is_ignored_file("unins000.dat") && is_ignored_file("MUSIC/Track02.wav") && !is_ignored_file("MUSIC/Track02.ogg"));
    CHECK(default_data_root("/p") == fs::path("/p/data") && edition_dir("/p", Edition::Disc2002) == fs::path("/p/data/disc-2002"));
    // Every edition has a manifest with its fingerprint files.
    for (int e = 1; e <= kEditionCount; ++e) {
        int files = 0, prints = 0;
        for (const auto& m : manifest()) {
            if (!(m.editions & edition_bit(Edition(e)))) continue;
            ++files;
            for (std::string_view fp : kFingerprintFiles) prints += path_iequals(m.path, fp);
        }
        CHECK(files > 900 && prints == 4);
    }
}

// ---- import: resumable, cancellable, keeps the player's settings; CD audio to WAV ----

void test_import() {
    std::printf("install: import\n");
    const fs::path dir = temp_dir();
    const fs::path src = dir / "src", dst = dir / "pref" / "data" / "unknown";
    fs::create_directories(src / "DATA");
    write_bytes(src / "iron_3d.exe", text_bytes("MZ"));
    write_bytes(src / "Iron_3D.ini", text_bytes("[x]"));
    Bytes big(3 << 20);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::uint8_t(i * 13);
    write_bytes(src / "DATA" / "big.bin", big);
    write_bytes(src / "unins000.dat", text_bytes("ignored"));

    // Cancelled at the first progress report: nothing final is left behind.
    ImportResult r = import_game(src, dst, [](const ImportProgress&) { return false; });
    CHECK(!r.ok && r.cancelled && !fs::exists(dst / "import.json"));
    // Cancelled halfway through big.bin: no .part remains.
    r = import_game(src, dst, [](const ImportProgress& p) { return p.bytes_done < (1u << 20); });
    CHECK(r.cancelled && !fs::exists(dst / "DATA" / "big.bin.part") && !fs::exists(dst / "DATA" / "big.bin"));
    int calls = 0;
    r = import_game(src, dst, [&](const ImportProgress& p) {
        ++calls;
        return p.bytes_done <= p.bytes_total;
    });
    CHECK(r.ok && r.edition == Edition::Unknown && r.files_written + r.files_skipped == 3 && !r.warnings.empty());
    CHECK(calls >= 4 && read_file(dst / "DATA" / "big.bin") == big && !fs::exists(dst / "unins000.dat"));
    CHECK(fs::exists(dst / "import.json"));
    // Re-run: everything is skipped; a damaged game file is rewritten; the player's ini is kept.
    write_bytes(dst / "Iron_3D.ini", text_bytes("[mine]"));
    Bytes broken = big;
    broken[12345] ^= 1;
    write_bytes(dst / "DATA" / "big.bin", broken);
    r = import_game(src, dst);
    CHECK(r.ok && r.files_written == 1 && r.files_skipped == 2);
    CHECK(read_file(dst / "DATA" / "big.bin") == big && read_file(dst / "Iron_3D.ini") == text_bytes("[mine]"));
    const auto imported = list_imported(dir / "pref");
    CHECK(imported.size() == 1 && imported[0].edition == Edition::Unknown && imported[0].source == fs::absolute(src).string());

    // A disc image (2448-byte sectors, one audio track) with the game at its root.
    const std::uint32_t audio = 80;
    write_bytes(dir / "game.mdf", make_raw(make_iso("IRON_3D.EXE;1", "iron_3d.exe"), 2448, audio));
    write_bytes(dir / "game.mds", make_mds(2448, kIsoSectors, audio));
    const fs::path disc_dst = dir / "disc";
    r = import_game(dir / "game.mds", disc_dst);
    CHECK(r.ok && r.files_written == 2 && r.tracks_written == 1);
    const fs::path wav = disc_dst / "MUSIC" / "Track02.wav";
    CHECK(fs::exists(disc_dst / "iron_3d.exe") && fs::exists(disc_dst / "SubDir" / "Big.bin"));
    if (fs::exists(wav)) {
        const Bytes w = read_file(wav);
        CHECK(w.size() == 44 + std::size_t(audio) * kAudioSector);
        CHECK(std::memcmp(w.data(), "RIFF", 4) == 0 && std::memcmp(w.data() + 8, "WAVEfmt ", 8) == 0 && w[22] == 2 && w[34] == 16);
        CHECK(w[24] == 0x44 && w[25] == 0xac && w[44] == 0 && w[45] == 1 && w[44 + kAudioSector] == 7); // 44100 Hz; sector pattern
    } else {
        CHECK(!"Track02.wav missing");
    }
    r = import_game(dir / "game.mds", disc_dst);
    CHECK(r.ok && r.files_skipped == 2 && r.tracks_skipped == 1 && r.bytes_written == 0);
    const Report rep = detect(disc_dst);
    CHECK(rep.readable() && rep.music_tracks == 1 && rep.music_format == "wav" && rep.edition == Edition::Unknown);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- the real releases, when present ----

void test_real_sources() {
    struct Case {
        fs::path path;
        Edition edition;
        std::size_t files;
    };
    const Case cases[] = {{gog_ru(), Edition::GogRu, 1039}, {gog_en(), Edition::GogEng, 1043},
                          {disc_2002(), Edition::Disc2002, 1017}, {disc_2001(), Edition::Disc2001, 932}};
    for (const Case& c : cases) {
        std::error_code ec;
        if (!fs::exists(c.path, ec)) {
            std::printf("install: %s absent, skipped\n", c.path.string().c_str());
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Report r = detect(c.path);
        std::printf("install: detect %s -> %s, %zu/%zu files, %d music tracks (%.2f s)\n", c.path.filename().string().c_str(),
                    edition_info(r.edition).key, r.present, r.expected, r.music_tracks, seconds_since(t0));
        CHECK(r.readable());
        CHECK(r.edition == c.edition);
        CHECK(r.complete());
        CHECK(r.music_tracks == 9);
        if (c.edition == Edition::Disc2001 || c.edition == Edition::Disc2002) {
            const auto src = open_source(c.path);
            CHECK(src->files().size() == c.files);
            CHECK(src->disc() && src->disc()->tracks().size() == 10 && src->volume() == "PARKAN_IS");
        }
    }
}

// PARKAN_INSTALL_E2E=<dir>: import both discs, compare the 2001 import with the unshield extraction.
void test_end_to_end() {
    const char* e2e = std::getenv("PARKAN_INSTALL_E2E");
    if (!e2e || !*e2e) return;
    const fs::path out = e2e;
    for (const auto& [image, key] : {std::pair{disc_2002(), "disc-2002"}, std::pair{disc_2001(), "disc-2001"}}) {
        std::error_code ec;
        if (!fs::exists(image, ec)) continue;
        const fs::path target = out / key;
        for (int pass = 0; pass < 2; ++pass) { // the second pass must skip everything
            const auto t0 = std::chrono::steady_clock::now();
            const ImportResult r = import_game(image, target);
            std::printf("install: import %s pass %d: %s, %d files written, %d skipped, %d tracks written, %d skipped, %.0f MB (%.1f s)\n",
                        key, pass + 1, r.ok ? "ok" : r.error.c_str(), r.files_written, r.files_skipped, r.tracks_written,
                        r.tracks_skipped, double(r.bytes_written) / 1e6, seconds_since(t0));
            CHECK(r.ok && r.damaged.empty() && r.tracks_written + r.tracks_skipped == 9);
            if (pass == 1) CHECK(r.files_written == 0 && r.tracks_written == 0);
        }
        DetectOptions full;
        full.full = true;
        const Report v = detect(target, full);
        std::printf("install: verify %s: %s, %zu/%zu intact, %zu extra, music %d (%s)\n", key, edition_info(v.edition).key, v.present,
                    v.expected, v.extra, v.music_tracks, v.music_format.c_str());
        CHECK(v.complete() && v.music_tracks == 9 && v.music_format == "wav");
    }
    // The 2001 import against unshield's extraction, byte for byte.
    const fs::path truth = home() / "parkan-re/editions/original-2001/Program_and_data_files";
    std::error_code ec;
    if (!fs::exists(truth, ec) || !fs::exists(out / "disc-2001", ec)) return;
    int same = 0, total = 0;
    for (const auto& e : fs::recursive_directory_iterator(truth)) {
        if (!e.is_regular_file()) continue;
        ++total;
        const auto mine = resolve_path(out / "disc-2001", fs::relative(e.path(), truth).generic_string());
        if (mine && read_file(*mine) == read_file(e.path())) ++same;
        else std::printf("  differs: %s\n", fs::relative(e.path(), truth).generic_string().c_str());
    }
    std::printf("install: 2001 import vs unshield: %d of %d files byte-identical\n", same, total);
    CHECK(total == 932 && same == total);
}

} // namespace

int install_unit_tests() {
    g_fail = 0;
    test_hashes();
    test_inflate();
    test_disc_images();
    test_cabinet();
    test_editions_table();
    test_import();
    test_real_sources();
    test_end_to_end();
    return g_fail;
}
