// Game data from an installed folder or a retail disc image — the API the app uses.
//
// The player either points the port at an installed game (a GOG folder, or a folder installed from
// the CD under Windows) — detect() checks it and the game runs from it in place — or at a disc image
// (Alcohol MDF/MDS, ISO, BIN/CUE), which import_game() unpacks once into the port's own data folder:
//
//   <pref>/data/<edition key>/        e.g. ~/Library/Application Support/Parkan/data/disc-2002/
//
// where <pref> is SDL_GetPrefPath("", "Parkan") (the app supplies it: this library is SDL-free). The
// imported folder then looks exactly like an installed one: the game files as on the disc, plus
// MUSIC/TrackNN.wav ripped from the CD audio tracks 2–10 (GOG ships the same tracks as .ogg; the
// original played them from the CD through MCI), plus import.json describing the import.
//
// Typical app flow:
//   1. at start, list_imported(pref) and any configured game folder; detect() the chosen one;
//   2. if none is usable, ask for a folder or an image (file dialog), detect() it, show the report;
//   3. for an image: import_game(image, edition_dir(pref, report.edition), progress) on a worker
//      thread, with a progress bar and a Cancel button (the callback returns false to cancel); a
//      cancelled or failed import can be re-run — files already in place are kept;
//   4. run the game from the folder.
#pragma once

#include "install/edition.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace parkan::install {

class GameSource;

struct FileIssue {
    enum class Kind { Missing, Damaged, Modified } kind;
    std::string path;
    bool user_file = false; // settings / bindings / saves (is_user_file): informational only
    std::string detail;
};

struct Report {
    std::string error;       // non-empty: the source could not be read at all (other fields unset)
    std::string source_kind; // "folder", "disc image (MDS), loose files", "disc image (MDS), InstallShield 5 cabinet", …
    std::string volume;      // disc volume label
    Edition edition = Edition::Unknown;
    // Each fingerprint file: the editions (edition_bit mask) whose content it matches; 0 = absent or unknown content.
    std::vector<std::pair<std::string, std::uint8_t>> fingerprints;
    bool hashed = false;            // every file's content was checked (DetectOptions::full)
    std::size_t expected = 0;       // files of the edition's manifest
    std::size_t present = 0;        // of those, found (with the right size, and content when hashed)
    std::size_t extra = 0;          // files not in the manifest (ignored files excluded)
    std::vector<FileIssue> issues;  // missing / damaged / modified files
    int music_tracks = 0;           // MUSIC/Track02..10 (.ogg or .wav) in a folder, CD-DA tracks on a disc
    std::string music_format;       // "ogg", "wav", "ogg+wav", "cd-da" or ""

    bool readable() const { return error.empty(); }
    // A known edition with every game file present and intact (user files aside).
    bool complete() const;
};

struct DetectOptions {
    bool full = false; // hash every file (folders: ~1 s per 300 MB; the 2001 disc: decompresses the cabinet)
};

// Identifies `source` (folder or disc image, see open_source in install/source.hpp) and checks it
// against the manifest of the edition found. Never throws: failures go to Report::error.
Report detect(const std::filesystem::path& source, const DetectOptions& options = {});
Report detect(const GameSource& source, const DetectOptions& options = {});

struct ImportProgress {
    enum class Phase { Files, Music } phase = Phase::Files;
    std::string item;               // the file or track being written
    std::uint64_t bytes_done = 0;   // across both phases
    std::uint64_t bytes_total = 0;
    int items_done = 0, items_total = 0; // files + tracks
};
// Called often (at least once per MB); return false to cancel. Runs on the importing thread.
using ProgressFn = std::function<bool(const ImportProgress&)>;

struct ImportOptions {
    bool music = true; // rip the CD-DA tracks
};

struct ImportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    Edition edition = Edition::Unknown;
    int files_written = 0, files_skipped = 0; // skipped: already present with the right content
    int tracks_written = 0, tracks_skipped = 0;
    std::uint64_t bytes_written = 0;
    std::vector<std::string> damaged;  // files whose content does not match the edition manifest
    std::vector<std::string> warnings;
};

// Copies the game files of `source` (a disc image, or any source open_source accepts) into `target`
// (created if needed) and rips its CD audio tracks to MUSIC/TrackNN.wav (44.1 kHz stereo 16-bit
// PCM). Idempotent and resumable: files already present with the right content are skipped, and
// every file is written to "<name>.part" and renamed when complete. Directory names reuse an
// existing directory of the same name in any case. Writes target/import.json on success. Never
// throws.
ImportResult import_game(const std::filesystem::path& source, const std::filesystem::path& target,
                         const ProgressFn& progress = {}, const ImportOptions& options = {});

// <pref>/data — where imports live. `pref` is SDL_GetPrefPath("", "Parkan").
std::filesystem::path default_data_root(const std::filesystem::path& pref);
// <pref>/data/<edition key>
std::filesystem::path edition_dir(const std::filesystem::path& pref, Edition e);

struct ImportedGame {
    std::filesystem::path dir;
    Edition edition = Edition::Unknown;
    std::string source, date; // from import.json
};
// The imports under default_data_root(pref) (folders with an import.json).
std::vector<ImportedGame> list_imported(const std::filesystem::path& pref);

// The text of install/manifests/manifest.inc for the given editions' sources (parkan-install manifest).
std::string generate_manifest(const std::vector<std::pair<Edition, const GameSource*>>& sources);

} // namespace parkan::install
