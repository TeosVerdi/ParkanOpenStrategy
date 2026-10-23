// Game data from an installed folder or a retail disc image (src/install/install.hpp).
//   parkan-install detect <folder|image> [--full]   identify the edition, check its files (--full: hash all)
//   parkan-install verify <folder>                  detect --full; exit status 0 when complete
//   parkan-install import <image|folder> [<target>] [--no-music]
//                                                   unpack into <target> (default: the port's data folder,
//                                                   SDL_GetPrefPath("", "Parkan")/data/<edition>/)
//   parkan-install tracks <image>                   track table
//   parkan-install ls <image> [<dir>]               ISO 9660 / Joliet listing
//   parkan-install manifest <out.inc> <key>=<folder|image>...   regenerate src/install/manifests/manifest.inc
#include "install/disc.hpp"
#include "install/install.hpp"
#include "install/source.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
using namespace parkan::install;

namespace {

std::string editions_text(std::uint8_t mask) {
    std::string s;
    for (int i = 1; i <= kEditionCount; ++i)
        if (mask & edition_bit(Edition(i))) s += (s.empty() ? "" : ", ") + std::string(edition_info(Edition(i)).key);
    return s.empty() ? "no known edition" : s;
}

int print_report(const Report& r) {
    if (!r.readable()) {
        std::printf("error: %s\n", r.error.c_str());
        return 2;
    }
    std::printf("source:   %s%s%s\n", r.source_kind.c_str(), r.volume.empty() ? "" : ", volume ", r.volume.c_str());
    std::printf("edition:  %s (%s)\n", edition_info(r.edition).name, edition_info(r.edition).key);
    for (const auto& [file, mask] : r.fingerprints) std::printf("  %-18s %s\n", file.c_str(), editions_text(mask).c_str());
    if (r.edition != Edition::Unknown) {
        std::printf("files:    %zu of %zu present%s, %zu extra\n", r.present, r.expected,
                    r.hashed ? " and intact (SHA-256)" : " (sizes checked)", r.extra);
        std::size_t shown = 0;
        for (const auto& i : r.issues) {
            if (++shown > 40) {
                std::printf("  … %zu more\n", r.issues.size() - 40);
                break;
            }
            const char* kind = i.kind == FileIssue::Kind::Missing ? "missing" : i.kind == FileIssue::Kind::Damaged ? "damaged" : "modified";
            std::printf("  %-8s %s%s%s%s\n", kind, i.path.c_str(), i.user_file ? " (user file)" : "",
                        i.detail.empty() ? "" : ": ", i.detail.c_str());
        }
    }
    std::printf("music:    %d tracks%s%s\n", r.music_tracks, r.music_format.empty() ? "" : ", ", r.music_format.c_str());
    std::printf("result:   %s\n", r.complete() ? "complete" : r.edition == Edition::Unknown ? "unknown edition" : "incomplete");
    return r.complete() ? 0 : 1;
}

int cmd_import(const fs::path& source, fs::path target, bool music) {
    if (target.empty()) {
        // The app's default location: the SDL preferences folder.
        char* pref = SDL_GetPrefPath("", "Parkan");
        if (!pref) {
            std::fprintf(stderr, "no preferences folder; give a target directory\n");
            return 2;
        }
        const fs::path root = pref;
        SDL_free(pref);
        const Report r = detect(source);
        if (!r.readable()) return print_report(r);
        target = edition_dir(root, r.edition);
    }
    std::printf("importing %s\n       -> %s\n", source.string().c_str(), target.string().c_str());
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    ImportOptions opt;
    opt.music = music;
    const ImportResult res = import_game(source, target, [&](const ImportProgress& p) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::milliseconds(250)) {
            last = now;
            std::printf("\r  %5.1f%%  %d/%d  %-48.48s", p.bytes_total ? 100.0 * double(p.bytes_done) / double(p.bytes_total) : 0.0,
                        p.items_done, p.items_total, p.item.c_str());
            std::fflush(stdout);
        }
        return true;
    }, opt);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\r%-80s\r", "");
    std::printf("edition:  %s\n", edition_info(res.edition).name);
    std::printf("files:    %d written, %d already present; music: %d tracks ripped, %d already present\n",
                res.files_written, res.files_skipped, res.tracks_written, res.tracks_skipped);
    std::printf("written:  %.1f MB in %.1f s\n", double(res.bytes_written) / 1e6, secs);
    for (const auto& d : res.damaged) std::printf("  damaged %s\n", d.c_str());
    for (const auto& w : res.warnings) std::printf("warning:  %s\n", w.c_str());
    if (!res.ok) {
        std::printf("error:    %s\n", res.error.c_str());
        return 1;
    }
    return 0;
}

int cmd_tracks(const fs::path& image) {
    const DiscImage img = DiscImage::open(image);
    std::printf("%s image, %zu tracks\n", img.format().c_str(), img.tracks().size());
    for (const Track& t : img.tracks())
        std::printf("track %2d: %s sector %u lba %u, %u sectors (%.1f s), pregap %u\n", t.number,
                    t.audio ? "audio " : t.mode == 2 ? "mode 2" : "mode 1", t.sector_size, t.lba, t.sectors, t.sectors / 75.0, t.pregap);
    return 0;
}

int cmd_ls(const fs::path& image, std::string_view dir) {
    const DiscImage img = DiscImage::open(image);
    const IsoFs fs(img);
    std::printf("volume %s%s\n", fs.volume_id().c_str(), fs.joliet() ? " (Joliet)" : "");
    const auto d = dir.empty() ? std::optional(fs.root()) : fs.find(dir);
    if (!d || !d->dir) {
        std::fprintf(stderr, "%s: no such directory\n", std::string(dir).c_str());
        return 2;
    }
    for (const IsoEntry& e : fs.list(*d))
        std::printf("%12s %s\n", e.dir ? "<dir>" : std::to_string(e.size).c_str(), e.name.c_str());
    return 0;
}

int cmd_manifest(const fs::path& out, int argc, char** argv) {
    std::vector<std::unique_ptr<GameSource>> keep;
    std::vector<std::pair<Edition, const GameSource*>> sources;
    for (int i = 0; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        const Edition e = eq ? edition_from_key(std::string_view(argv[i], std::size_t(eq - argv[i]))) : Edition::Unknown;
        if (e == Edition::Unknown) {
            std::fprintf(stderr, "expected <edition key>=<path>, got %s\n", argv[i]);
            return 2;
        }
        keep.push_back(open_source(eq + 1));
        std::printf("%s: %s, %zu files\n", edition_info(e).key, keep.back()->kind().c_str(), keep.back()->files().size());
        sources.emplace_back(e, keep.back().get());
    }
    const std::string text = generate_manifest(sources);
    std::ofstream(out, std::ios::binary) << text;
    std::printf("wrote %s\n", out.string().c_str());
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: parkan-install detect <folder|image> [--full]\n"
                 "       parkan-install verify <folder>\n"
                 "       parkan-install import <image|folder> [<target dir>] [--no-music]\n"
                 "       parkan-install tracks <image>\n"
                 "       parkan-install ls <image> [<dir>]\n"
                 "       parkan-install manifest <out.inc> <edition>=<folder|image>...\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string_view cmd = argv[1];
    std::vector<std::string_view> args;
    bool full = false, music = true;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full") == 0) full = true;
        else if (std::strcmp(argv[i], "--no-music") == 0) music = false;
        else args.push_back(argv[i]);
    }
    try {
        if (cmd == "detect" && args.size() == 1) {
            DetectOptions o;
            o.full = full;
            return print_report(detect(fs::path(args[0]), o));
        }
        if (cmd == "verify" && args.size() == 1) {
            DetectOptions o;
            o.full = true;
            return print_report(detect(fs::path(args[0]), o));
        }
        if (cmd == "import" && (args.size() == 1 || args.size() == 2))
            return cmd_import(fs::path(args[0]), args.size() == 2 ? fs::path(args[1]) : fs::path(), music);
        if (cmd == "tracks" && args.size() == 1) return cmd_tracks(fs::path(args[0]));
        if (cmd == "ls" && (args.size() == 1 || args.size() == 2)) return cmd_ls(fs::path(args[0]), args.size() == 2 ? args[1] : "");
        if (cmd == "manifest" && argc >= 4) return cmd_manifest(argv[2], argc - 3, argv + 3);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 2;
    }
    return usage();
}
