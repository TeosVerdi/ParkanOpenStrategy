// A game's files wherever they are: an installed folder, the loose files of the 2002 disc, or the
// InstallShield cabinet of the 2001 disc (in an image or on a mounted disc).
#pragma once

#include "install/disc.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace parkan::install {

struct SourceFile {
    std::string path; // '/'-separated, relative to the game root, names normalised (no ";1", no trailing '.')
    std::uint64_t size = 0;
};

class GameSource {
public:
    virtual ~GameSource() = default;
    const std::string& kind() const { return kind_; } // e.g. "disc image (MDS), InstallShield 5 cabinet"
    const std::vector<SourceFile>& files() const { return files_; }
    const SourceFile* find(std::string_view path) const; // case-insensitive
    // Streams the content of `f`, an element of files() (false from the sink stops; read then returns false).
    virtual bool read(const SourceFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const = 0;
    Bytes read_all(const SourceFile& f) const;
    // The disc image behind this source, for CD-DA tracks and the volume label (nullptr for folders).
    virtual const DiscImage* disc() const { return nullptr; }
    virtual std::string volume() const { return {}; }

protected:
    std::size_t index_of(const SourceFile& f) const; // position in files(); f must be one of them
    std::string kind_;
    std::vector<SourceFile> files_;
};

// Opens `path`: a game folder (iron_3d.exe inside), a mounted disc or copied disc folder (data1.cab, or
// "program files/Nikita/IS"), or a disc image (.mds/.mdf/.iso/.cue/.bin). Throws std::runtime_error
// (or FormatError) saying why when it is none of these.
std::unique_ptr<GameSource> open_source(const std::filesystem::path& path);

} // namespace parkan::install
