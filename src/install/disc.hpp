// CD images (Alcohol 120% MDF/MDS, plain or raw ISO, BIN/CUE) and the ISO 9660 / Joliet file system on
// their data track. Reference: ~/parkan-re/tools/discimage.py. Read-only; no platform dependencies.
#pragma once

#include "core/bytes.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parkan::install {

// Random access to a byte stream (a local file, a file inside a disc image).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    // Reads exactly `n` bytes at `offset`; throws FormatError past the end.
    virtual void read(std::uint64_t offset, void* dst, std::size_t n) const = 0;
};

class FileSource : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& path); // throws std::runtime_error
    std::uint64_t size() const override { return size_; }
    void read(std::uint64_t offset, void* dst, std::size_t n) const override;

private:
    mutable std::ifstream f_;
    std::uint64_t size_ = 0;
};

constexpr std::size_t kSector = 2048;      // ISO 9660 logical sector (mode 1 / mode 2 form 1 user data)
constexpr std::size_t kAudioSector = 2352; // one CD-DA frame: 588 stereo 16-bit samples, little-endian

struct Track {
    int number = 0;
    bool audio = false;
    int mode = 0;              // data tracks: 1 or 2
    std::uint32_t lba = 0;     // absolute start (index 01)
    std::uint32_t sectors = 0; // length from index 01 to the next track's stored data
    std::uint32_t pregap = 0;  // sectors before index 01 (informational)
    std::uint32_t sector_size = 0;  // bytes per stored sector: 2048, 2336, 2352 or 2448 (2352 + 96 subchannel)
    std::uint32_t data_offset = 0;  // user data within a stored sector (16 mode 1, 24 mode 2 form 1)
    std::uint64_t file_offset = 0;  // byte offset of index 01 in `file`
    std::filesystem::path file;
};

class DiscImage {
public:
    // .mds (reads the sibling .mdf), .mdf (uses a sibling .mds, else sniffs the sector layout), .cue,
    // .bin (uses a sibling .cue, else sniffs), .iso (2048-byte sectors, or raw 2352/2448 when it
    // starts with a sync pattern). Throws FormatError / std::runtime_error.
    static DiscImage open(const std::filesystem::path& path);

    const std::string& format() const { return format_; } // "MDS", "CUE", "ISO", "raw"
    const std::filesystem::path& path() const { return path_; }
    const std::vector<Track>& tracks() const { return tracks_; }
    const Track* track(int number) const;
    const Track* data_track() const; // the track holding the file system
    std::vector<const Track*> audio_tracks() const;

    // 2048-byte user data of `count` sectors from absolute `lba` on (any data track).
    void read_data(std::uint32_t lba, std::uint32_t count, std::uint8_t* dst) const;
    // `count` raw 2352-byte audio frames from sector `first` (relative to the track's index 01).
    void read_audio(const Track& t, std::uint32_t first, std::uint32_t count, std::uint8_t* dst) const;

    // Building blocks, public for the tests: parse an MDS / CUE description.
    static std::vector<Track> parse_mds(std::span<const std::uint8_t> mds, const std::filesystem::path& mdf,
                                        std::uint64_t mdf_size);
    static std::vector<Track> parse_cue(std::string_view cue, const std::filesystem::path& dir,
                                        const std::function<std::uint64_t(const std::filesystem::path&)>& file_size);

    DiscImage(DiscImage&&) noexcept;
    DiscImage& operator=(DiscImage&&) noexcept;
    ~DiscImage();

private:
    DiscImage() = default;
    std::ifstream& stream(const std::filesystem::path& file) const;
    void finish(); // pick the data track and its user-data offset

    std::string format_;
    std::filesystem::path path_;
    std::vector<Track> tracks_;
    int data_track_ = -1;
    mutable std::map<std::filesystem::path, std::unique_ptr<std::ifstream>> files_;
    mutable std::vector<std::uint8_t> raw_;
    // Cache: user data of a run of sectors (sequential readers fetch small pieces).
    mutable std::vector<std::uint8_t> cache_;
    mutable std::uint32_t cache_lba_ = 0, cache_count_ = 0;
};

// One directory record.
struct IsoEntry {
    std::string name; // UTF-8, ";1" version and a trailing "." removed
    bool dir = false;
    std::uint64_t size = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extents; // (lba, bytes); several for multi-extent files
};

// "DESCR.;1" -> "DESCR", "readme.htm;1" -> "readme.htm".
std::string normalise_iso_name(std::string_view name);

class IsoFs {
public:
    explicit IsoFs(const DiscImage& disc); // throws FormatError without an ISO 9660 volume
    bool joliet() const { return joliet_; }
    const std::string& volume_id() const { return volume_id_; }
    const IsoEntry& root() const { return root_; }

    std::vector<IsoEntry> list(const IsoEntry& dir) const;
    // Case-insensitive; '/' or '\' separators; ISO ";1" and trailing dots ignored.
    std::optional<IsoEntry> find(std::string_view path) const;
    // Every file and directory below `dir`, depth-first, with '/'-joined paths relative to it.
    void walk(const IsoEntry& dir, const std::function<void(const std::string& path, const IsoEntry&)>& fn) const;

    void read(const IsoEntry& file, std::uint64_t offset, void* dst, std::size_t n) const;
    Bytes read_all(const IsoEntry& file) const;

private:
    const DiscImage& disc_;
    bool joliet_ = false;
    std::string volume_id_;
    IsoEntry root_;
};

// A file inside an ISO file system as a ByteSource (the IsoFs must outlive it).
class IsoFileSource : public ByteSource {
public:
    IsoFileSource(const IsoFs& fs, IsoEntry e) : fs_(fs), e_(std::move(e)) {}
    std::uint64_t size() const override { return e_.size; }
    void read(std::uint64_t offset, void* dst, std::size_t n) const override { fs_.read(e_, offset, dst, n); }

private:
    const IsoFs& fs_;
    IsoEntry e_;
};

// ASCII case-insensitive comparison of '/'-separated paths after ISO name normalisation of each part.
bool path_iequals(std::string_view a, std::string_view b);
std::string ascii_lower(std::string_view s);

} // namespace parkan::install
