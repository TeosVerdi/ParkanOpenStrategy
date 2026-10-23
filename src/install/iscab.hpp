// InstallShield 5 cabinets ("ISc(", data1.cab with its header embedded or in data1.hdr), as written by
// the 2001 disc's installer. Port of the parts of unshield (libunshield: header, file group, file
// descriptor, reader) that a single-volume v5 cabinet needs. Each file's data is either stored or a
// series of chunks, each a u16 length followed by an independent raw-deflate stream. Version 5 keeps
// no checksum of its own: callers verify against the edition manifests (install/edition.hpp).
#pragma once

#include "install/disc.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace parkan::install {

struct CabFile {
    int index = 0;           // file descriptor index
    std::string group;       // file group ("Program and data files")
    std::string dir;         // '/'-separated, as stored ("MISSIONS/Tutorial.01"), UTF-8
    std::string name;        // UTF-8
    std::uint16_t flags = 0; // 1 split, 2 obfuscated, 4 compressed, 8 invalid
    std::uint32_t expanded = 0, compressed = 0, data_offset = 0;

    std::string path() const { return dir.empty() ? name : dir + "/" + name; }
};

class IsCabinet {
public:
    // `header`: data1.hdr, or data1.cab itself when the header is embedded; `data`: data1.cab.
    // Throws FormatError for anything but a single-volume version 5 (or 0) cabinet.
    IsCabinet(std::shared_ptr<const ByteSource> header, std::shared_ptr<const ByteSource> data);
    explicit IsCabinet(std::shared_ptr<const ByteSource> cab) : IsCabinet(cab, cab) {}

    int major_version() const { return major_; }
    const std::vector<std::string>& groups() const { return groups_; }
    // The valid files of every file group, in group order.
    const std::vector<CabFile>& files() const { return files_; }
    const CabFile* find(std::string_view path) const; // case-insensitive

    // Streams the expanded file in pieces to `sink` (false = stop; extract then returns false).
    // Throws FormatError on corrupt data or a size mismatch.
    bool extract(const CabFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const;
    Bytes extract(const CabFile& f) const;

private:
    const std::uint8_t* at(std::uint64_t offset, std::size_t n) const; // header bytes, grown on demand
    std::string string_at(std::uint64_t offset) const;                 // NUL-terminated, cp1251

    std::shared_ptr<const ByteSource> header_, data_;
    mutable Bytes head_;
    int major_ = 0;
    std::uint32_t cab_desc_ = 0, file_table_ = 0, dir_count_ = 0, file_count_ = 0;
    std::vector<std::uint32_t> table_;
    std::vector<std::string> groups_;
    std::vector<CabFile> files_;
};

// InstallShield's obfuscation (flag 2), applied to the stored bytes before decompression; `seed`
// counts the bytes of the file processed so far.
void is_deobfuscate(std::span<std::uint8_t> data, std::uint32_t& seed);

} // namespace parkan::install
