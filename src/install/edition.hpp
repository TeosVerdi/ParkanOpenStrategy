// The releases the port knows (notes/releases.md) and their file manifests, generated from the
// releases themselves by `parkan-install manifest` into install/manifests/manifest.inc.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace parkan::install {

enum class Edition : std::uint8_t {
    Unknown = 0,
    GogRu = 1,    // GOG 2.0.0.4, Russian: the 2002 re-edition with the older Terrain.dll (2001-02-27)
    GogEng = 2,   // GOG 2.0.0.3, English
    Disc2002 = 3, // retail CD «Часть 1» (2002 re-edition), MSI with the files stored loose
    Disc2001 = 4, // retail CD, the 2001 original (InstallShield 5 cabinet), older binaries throughout
};
constexpr int kEditionCount = 4; // known editions, 1..4

struct EditionInfo {
    Edition id;
    const char* key;      // folder / manifest name: "gog-ru", "gog-en", "disc-2002", "disc-2001", "unknown"
    const char* name;     // human-readable
    const char* language; // "ru", "en"
};

const EditionInfo& edition_info(Edition e);
Edition edition_from_key(std::string_view key); // Unknown when not recognised
constexpr std::uint8_t edition_bit(Edition e) { return e == Edition::Unknown ? 0 : std::uint8_t(1u << (int(e) - 1)); }

// One file of one or more editions: '/'-separated path as shipped, size, the first 8 bytes of its
// SHA-256 (big-endian), and the editions (edition_bit mask) that ship exactly this content.
struct ManifestEntry {
    const char* path;
    std::uint32_t size;
    std::uint64_t hash;
    std::uint8_t editions;
};
std::span<const ManifestEntry> manifest();

// Files the game or the player rewrites (settings, key bindings, save slots): only their presence is
// checked, and a difference is reported as "modified", never as damage.
bool is_user_file(std::string_view path);
// Installer bookkeeping and port-generated files that are not part of any edition's content
// (GOG uninstaller, shortcuts, .DS_Store, import.json, ripped MUSIC/*.wav).
bool is_ignored_file(std::string_view path);

// Fingerprint files: their content identifies the edition (code builds and the string table differ
// between all four releases).
inline constexpr std::string_view kFingerprintFiles[] = {"iron3d.dll", "Terrain.dll", "DATA/TextRes.dll", "ai.dll"};

} // namespace parkan::install
