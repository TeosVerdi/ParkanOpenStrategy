#include "install/source.hpp"

#include "core/paths.hpp"
#include "install/iscab.hpp"

#include <algorithm>

namespace parkan::install {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kChunk = 1 << 20;
constexpr std::string_view kDiscGameDir = "program files/Nikita/IS"; // the 2002 disc's loose files

class FolderGame : public GameSource {
public:
    FolderGame(const fs::path& root, std::string kind) : root_(root) {
        kind_ = std::move(kind);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            files_.push_back({fs::relative(it->path(), root, ec).generic_string(), it->file_size(ec)});
        }
        std::sort(files_.begin(), files_.end(), [](const SourceFile& a, const SourceFile& b) { return a.path < b.path; });
    }
    bool read(const SourceFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const override {
        FileSource src(root_ / f.path);
        Bytes buf;
        for (std::uint64_t pos = 0; pos < src.size();) {
            buf.resize(std::size_t(std::min<std::uint64_t>(kChunk, src.size() - pos)));
            src.read(pos, buf.data(), buf.size());
            pos += buf.size();
            if (!sink(buf)) return false;
        }
        return true;
    }

private:
    fs::path root_;
};

// The disc image and its file system, shared by the two disc layouts.
struct Disc {
    explicit Disc(const fs::path& p) : image(DiscImage::open(p)), iso(image) {}
    DiscImage image;
    IsoFs iso;
};

class IsoGame : public GameSource {
public:
    IsoGame(std::unique_ptr<Disc> disc, const IsoEntry& root) : disc_(std::move(disc)) {
        kind_ = "disc image (" + disc_->image.format() + "), loose files";
        disc_->iso.walk(root, [&](const std::string& path, const IsoEntry& e) {
            if (e.dir) return;
            files_.push_back({path, e.size});
            entries_.push_back(e);
        });
    }
    bool read(const SourceFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const override {
        const IsoEntry& e = entries_[index_of(f)];
        Bytes buf;
        for (std::uint64_t pos = 0; pos < e.size;) {
            buf.resize(std::size_t(std::min<std::uint64_t>(kChunk, e.size - pos)));
            disc_->iso.read(e, pos, buf.data(), buf.size());
            pos += buf.size();
            if (!sink(buf)) return false;
        }
        return true;
    }
    const DiscImage* disc() const override { return &disc_->image; }
    std::string volume() const override { return disc_->iso.volume_id(); }

private:
    std::unique_ptr<Disc> disc_;
    std::vector<IsoEntry> entries_;
};

class CabGame : public GameSource {
public:
    CabGame(std::unique_ptr<Disc> disc, std::shared_ptr<const ByteSource> header, std::shared_ptr<const ByteSource> data,
            std::string kind)
        : disc_(std::move(disc)), cab_(std::move(header), std::move(data)) {
        kind_ = std::move(kind);
        for (const CabFile& f : cab_.files()) files_.push_back({f.path(), f.expanded});
    }
    bool read(const SourceFile& f, const std::function<bool(std::span<const std::uint8_t>)>& sink) const override {
        return cab_.extract(cab_.files()[index_of(f)], sink);
    }
    const DiscImage* disc() const override { return disc_ ? &disc_->image : nullptr; }
    std::string volume() const override { return disc_ ? disc_->iso.volume_id() : std::string(); }

private:
    std::unique_ptr<Disc> disc_; // declared first: the cabinet reads through it
    IsCabinet cab_;
};

std::unique_ptr<GameSource> open_disc(const fs::path& path) {
    auto disc = std::make_unique<Disc>(path);
    const std::string fmt = disc->image.format();
    if (auto dir = disc->iso.find(kDiscGameDir); dir && dir->dir && disc->iso.find(std::string(kDiscGameDir) + "/iron_3d.exe"))
        return std::make_unique<IsoGame>(std::move(disc), *dir);
    if (auto cab = disc->iso.find("data1.cab")) {
        auto data = std::make_shared<IsoFileSource>(disc->iso, *cab);
        std::shared_ptr<const ByteSource> header = data;
        if (auto hdr = disc->iso.find("data1.hdr")) header = std::make_shared<IsoFileSource>(disc->iso, *hdr);
        return std::make_unique<CabGame>(std::move(disc), header, data, "disc image (" + fmt + "), InstallShield 5 cabinet");
    }
    // Any other layout: the directory holding iron_3d.exe.
    std::optional<std::string> found;
    disc->iso.walk(disc->iso.root(), [&](const std::string& p, const IsoEntry& e) {
        if (!found && !e.dir && ascii_lower(e.name) == "iron_3d.exe") found = p;
    });
    if (found) {
        const auto slash = found->rfind('/');
        const auto dir = slash == std::string::npos ? std::optional(disc->iso.root()) : disc->iso.find(found->substr(0, slash));
        if (dir) return std::make_unique<IsoGame>(std::move(disc), *dir);
    }
    throw std::runtime_error("no Parkan files on the disc image " + path.string());
}

} // namespace

std::size_t GameSource::index_of(const SourceFile& f) const {
    if (&f < files_.data() || &f >= files_.data() + files_.size()) throw std::logic_error("not a file of this source: " + f.path);
    return std::size_t(&f - files_.data());
}

const SourceFile* GameSource::find(std::string_view path) const {
    for (const auto& f : files_)
        if (path_iequals(f.path, path)) return &f;
    return nullptr;
}

Bytes GameSource::read_all(const SourceFile& f) const {
    Bytes b;
    b.reserve(std::size_t(f.size));
    read(f, [&](std::span<const std::uint8_t> s) {
        b.insert(b.end(), s.begin(), s.end());
        return true;
    });
    return b;
}

std::unique_ptr<GameSource> open_source(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) throw std::runtime_error(path.string() + ": not found");
    if (!fs::is_directory(path, ec)) return open_disc(path);

    if (resolve_path(path, "iron_3d.exe")) return std::make_unique<FolderGame>(path, "folder");
    if (auto loose = resolve_path(path, kDiscGameDir); loose && resolve_path(*loose, "iron_3d.exe"))
        return std::make_unique<FolderGame>(*loose, "folder, 2002 disc layout");
    if (auto cab = resolve_path(path, "data1.cab")) {
        auto data = std::make_shared<FileSource>(*cab);
        std::shared_ptr<const ByteSource> header = data;
        if (auto hdr = resolve_path(path, "data1.hdr")) header = std::make_shared<FileSource>(*hdr);
        return std::make_unique<CabGame>(nullptr, header, data, "folder, InstallShield 5 cabinet");
    }
    // A folder holding one disc image (the user picked the image's folder).
    std::vector<fs::path> images;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        const std::string ext = ascii_lower(e.path().extension().string());
        if (ext == ".mds" || ext == ".cue" || ext == ".iso") images.push_back(e.path());
    }
    if (images.size() == 1) return open_disc(images[0]);
    if (images.size() > 1) throw std::runtime_error(path.string() + ": several disc images; choose one");
    throw std::runtime_error(path.string() + ": no Parkan installation or disc image here");
}

} // namespace parkan::install
