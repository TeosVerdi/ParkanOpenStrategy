#include "install/install.hpp"

#include "core/paths.hpp"
#include "install/hash.hpp"
#include "install/source.hpp"

#include <bit>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <unordered_map>

namespace parkan::install {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kWavHeader = 44;

std::uint64_t hash_source(const GameSource& src, const SourceFile& f) {
    Sha256 h;
    src.read(f, [&](std::span<const std::uint8_t> s) {
        h.update(s);
        return true;
    });
    const auto d = h.final();
    return digest_prefix(d);
}

std::uint64_t hash_file(const fs::path& p) {
    FileSource src(p);
    Sha256 h;
    Bytes buf(1 << 20);
    for (std::uint64_t pos = 0; pos < src.size();) {
        const auto n = std::size_t(std::min<std::uint64_t>(buf.size(), src.size() - pos));
        src.read(pos, buf.data(), n);
        h.update({buf.data(), n});
        pos += n;
    }
    const auto d = h.final();
    return digest_prefix(d);
}

// The manifest entries of one edition by lower-case path.
std::unordered_map<std::string, const ManifestEntry*> edition_manifest(Edition e) {
    std::unordered_map<std::string, const ManifestEntry*> m;
    const std::uint8_t bit = edition_bit(e);
    for (const auto& entry : manifest())
        if (entry.editions & bit) m.emplace(ascii_lower(entry.path), &entry);
    return m;
}

int music_number(std::string_view path, std::string_view& ext) {
    // MUSIC/TrackNN.ogg|wav, NN = 02..10 (the CD's audio tracks).
    const std::string p = ascii_lower(path);
    if (p.size() != 17 || !p.starts_with("music/track")) return 0;
    ext = std::string_view(path).substr(13);
    const std::string e = ascii_lower(ext);
    if (e != ".ogg" && e != ".wav") return 0;
    const int n = (p[11] - '0') * 10 + (p[12] - '0');
    return p[11] >= '0' && p[11] <= '9' && p[12] >= '0' && p[12] <= '9' && n >= 2 && n <= 10 ? n : 0;
}

std::string json_escape(std::string_view s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\', r += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
            r += buf;
        } else r += c;
    }
    return r;
}

// The string value of "key" in a flat JSON object (import.json), or "".
std::string json_field(std::string_view json, std::string_view key) {
    const std::string k = "\"" + std::string(key) + "\"";
    auto pos = json.find(k);
    if (pos == std::string_view::npos) return {};
    pos = json.find('"', json.find(':', pos + k.size()));
    if (pos == std::string_view::npos) return {};
    std::string r;
    for (std::size_t i = pos + 1; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char c = json[++i];
            if (c == 'n') r += '\n';
            else if (c == 't') r += '\t';
            else if (c == 'u' && i + 4 < json.size()) {
                r += char(std::stoi(std::string(json.substr(i + 1, 4)), nullptr, 16));
                i += 4;
            } else r += c;
        } else {
            r += json[i];
        }
    }
    return r;
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// target/rel, reusing existing directories (and an existing file) of the same name in any case.
fs::path place(const fs::path& target, std::string_view rel, bool create_dirs) {
    fs::path cur = target;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = rel.find('/', start);
        const std::string part(rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (auto existing = resolve_path(cur, part)) cur = *existing;
        else cur /= part;
        if (slash == std::string_view::npos) return cur;
        if (create_dirs) fs::create_directories(cur);
        start = slash + 1;
    }
}

void write_wav_header(std::ofstream& out, std::uint32_t pcm_bytes) {
    auto u32 = [&](std::uint32_t v) {
        const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        out.write(b, 4);
    };
    auto u16 = [&](std::uint16_t v) {
        const char b[2] = {char(v), char(v >> 8)};
        out.write(b, 2);
    };
    out.write("RIFF", 4), u32(36 + pcm_bytes), out.write("WAVEfmt ", 8);
    u32(16), u16(1), u16(2), u32(44100), u32(44100 * 4), u16(4), u16(16);
    out.write("data", 4), u32(pcm_bytes);
}

struct Cancelled {};

} // namespace

bool Report::complete() const {
    if (!readable() || edition == Edition::Unknown) return false;
    for (const auto& i : issues)
        if (!i.user_file && i.kind != FileIssue::Kind::Modified) return false;
    return true;
}

Report detect(const GameSource& src, const DetectOptions& options) {
    Report r;
    r.source_kind = src.kind();
    r.volume = src.volume();
    try {
        // Edition: the editions every present fingerprint file agrees on.
        std::uint8_t agree = 0;
        bool any = false;
        for (std::string_view fp : kFingerprintFiles) {
            std::uint8_t mask = 0;
            if (const SourceFile* f = src.find(fp)) {
                const auto d = sha256(src.read_all(*f));
                const std::uint64_t h = digest_prefix(d);
                for (const auto& e : manifest())
                    if (e.size == f->size && e.hash == h && path_iequals(e.path, fp)) mask |= e.editions;
                agree = any ? std::uint8_t(agree & mask) : mask;
                any = true;
            }
            r.fingerprints.emplace_back(std::string(fp), mask);
        }
        if (std::popcount(agree) == 1) r.edition = Edition(std::countr_zero(agree) + 1);

        // Music.
        if (const DiscImage* disc = src.disc()) {
            r.music_tracks = int(disc->audio_tracks().size());
            if (r.music_tracks) r.music_format = "cd-da";
        } else {
            bool tracks[11] = {}, ogg = false, wav = false;
            for (const auto& f : src.files()) {
                std::string_view ext;
                if (const int n = music_number(f.path, ext)) {
                    tracks[n] = true;
                    (ascii_lower(ext) == ".ogg" ? ogg : wav) = true;
                }
            }
            for (bool t : tracks) r.music_tracks += t;
            r.music_format = ogg && wav ? "ogg+wav" : ogg ? "ogg" : wav ? "wav" : "";
        }

        if (r.edition == Edition::Unknown) return r;
        // Completeness against the edition's manifest.
        auto expected = edition_manifest(r.edition);
        r.expected = expected.size();
        r.hashed = options.full;
        for (const auto& f : src.files()) {
            if (is_ignored_file(f.path)) continue;
            const auto it = expected.find(ascii_lower(f.path));
            if (it == expected.end()) {
                ++r.extra;
                continue;
            }
            const ManifestEntry& e = *it->second;
            expected.erase(it);
            const bool user = is_user_file(e.path);
            bool same = f.size == e.size;
            if (same && options.full) same = hash_source(src, f) == e.hash;
            if (same || user) ++r.present;
            if (!same)
                r.issues.push_back({user ? FileIssue::Kind::Modified : FileIssue::Kind::Damaged, e.path, user,
                                    f.size != e.size ? "size " + std::to_string(f.size) + ", expected " + std::to_string(e.size)
                                                     : "content differs"});
        }
        for (const auto& [key, e] : expected)
            r.issues.push_back({FileIssue::Kind::Missing, e->path, is_user_file(e->path), {}});
        std::sort(r.issues.begin(), r.issues.end(), [](const FileIssue& a, const FileIssue& b) { return a.path < b.path; });
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

Report detect(const fs::path& source, const DetectOptions& options) {
    try {
        const auto src = open_source(source);
        return detect(*src, options);
    } catch (const std::exception& ex) {
        Report r;
        r.error = ex.what();
        return r;
    }
}

ImportResult import_game(const fs::path& source, const fs::path& target, const ProgressFn& progress,
                         const ImportOptions& options) {
    ImportResult res;
    ImportProgress p;
    auto report = [&] {
        if (progress && !progress(p)) throw Cancelled{};
    };
    fs::path part; // the file being written, removed on failure
    try {
        const auto src = open_source(source);
        const Report det = detect(*src);
        if (!det.readable()) throw std::runtime_error(det.error);
        res.edition = det.edition;
        if (det.edition == Edition::Unknown)
            res.warnings.push_back("unknown edition: the files are copied but cannot be verified");
        const auto expected = edition_manifest(det.edition);

        std::vector<const SourceFile*> files;
        for (const auto& f : src->files())
            if (!is_ignored_file(f.path)) files.push_back(&f), p.bytes_total += f.size;
        std::vector<const Track*> tracks;
        if (options.music && src->disc())
            for (const Track* t : src->disc()->audio_tracks()) tracks.push_back(t), p.bytes_total += std::uint64_t(t->sectors) * kAudioSector;
        p.items_total = int(files.size() + tracks.size());
        fs::create_directories(target);

        Bytes buf;
        for (const SourceFile* f : files) {
            p.phase = ImportProgress::Phase::Files;
            p.item = f->path;
            report();
            const auto it = expected.find(ascii_lower(f->path));
            const ManifestEntry* want = it == expected.end() ? nullptr : it->second;
            const fs::path dst = place(target, f->path, true);
            std::error_code ec;
            if (fs::is_regular_file(dst, ec)) {
                // Keep the player's settings; keep game files that are already right.
                bool keep = is_user_file(f->path);
                if (!keep && fs::file_size(dst, ec) == f->size) {
                    const std::uint64_t have = hash_file(dst);
                    keep = want ? have == want->hash : have == hash_source(*src, *f);
                }
                if (keep) {
                    ++res.files_skipped, ++p.items_done, p.bytes_done += f->size;
                    continue;
                }
            }
            part = dst;
            part += ".part";
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("cannot write " + part.string());
                Sha256 h;
                src->read(*f, [&](std::span<const std::uint8_t> s) {
                    out.write(reinterpret_cast<const char*>(s.data()), std::streamsize(s.size()));
                    h.update(s);
                    p.bytes_done += s.size();
                    report();
                    return true;
                });
                out.close();
                if (!out) throw std::runtime_error("write failed: " + part.string());
                const auto d = h.final();
                if (want && !is_user_file(f->path) && (want->size != f->size || digest_prefix(d) != want->hash))
                    res.damaged.push_back(f->path);
            }
            fs::rename(part, dst);
            part.clear();
            ++res.files_written, ++p.items_done;
            res.bytes_written += f->size;
        }

        // CD audio -> MUSIC/TrackNN.wav (the numbering GOG's MUSIC/TrackNN.ogg uses).
        std::vector<int> ripped;
        for (const Track* t : tracks) {
            p.phase = ImportProgress::Phase::Music;
            char name[32];
            std::snprintf(name, sizeof name, "MUSIC/Track%02d.wav", t->number);
            p.item = name;
            report();
            const std::uint64_t pcm = std::uint64_t(t->sectors) * kAudioSector;
            if (pcm > 0xffffffffu - kWavHeader) throw std::runtime_error(std::string(name) + ": track too long for WAV");
            const fs::path dst = place(target, name, true);
            std::error_code ec;
            ripped.push_back(t->number);
            if (fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) == kWavHeader + pcm) {
                ++res.tracks_skipped, ++p.items_done, p.bytes_done += pcm;
                continue;
            }
            part = dst;
            part += ".part";
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("cannot write " + part.string());
                write_wav_header(out, std::uint32_t(pcm));
                constexpr std::uint32_t kRun = 75 * 4; // 4 seconds
                buf.resize(std::size_t(kRun) * kAudioSector);
                for (std::uint32_t s = 0; s < t->sectors; s += kRun) {
                    const std::uint32_t n = std::min(kRun, t->sectors - s);
                    src->disc()->read_audio(*t, s, n, buf.data());
                    out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(std::size_t(n) * kAudioSector));
                    p.bytes_done += std::uint64_t(n) * kAudioSector;
                    report();
                }
                out.close();
                if (!out) throw std::runtime_error("write failed: " + part.string());
            }
            fs::rename(part, dst);
            part.clear();
            ++res.tracks_written, ++p.items_done;
            res.bytes_written += kWavHeader + pcm;
        }
        if (options.music && !src->disc())
            res.warnings.push_back("the source has no CD audio tracks: no music was ripped");

        // import.json
        std::string tracks_json;
        for (int n : ripped) tracks_json += (tracks_json.empty() ? "" : ", ") + std::to_string(n);
        const std::string json =
            "{\n"
            "  \"format\": 1,\n"
            "  \"edition\": \"" + std::string(edition_info(res.edition).key) + "\",\n"
            "  \"edition_name\": \"" + json_escape(edition_info(res.edition).name) + "\",\n"
            "  \"source\": \"" + json_escape(fs::absolute(source).string()) + "\",\n"
            "  \"source_kind\": \"" + json_escape(src->kind()) + "\",\n"
            "  \"volume\": \"" + json_escape(src->volume()) + "\",\n"
            "  \"imported\": \"" + utc_now() + "\",\n"
            "  \"files\": " + std::to_string(files.size()) + ",\n"
            "  \"damaged\": " + std::to_string(res.damaged.size()) + ",\n"
            "  \"music\": {\"format\": \"wav\", \"tracks\": [" + tracks_json + "]}\n"
            "}\n";
        part = target / "import.json.part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out << json;
            if (!out) throw std::runtime_error("cannot write " + part.string());
        }
        fs::rename(part, target / "import.json");
        part.clear();
        res.ok = res.damaged.empty();
        if (!res.ok)
            res.error = std::to_string(res.damaged.size()) + " files differ from the " + edition_info(res.edition).name +
                        " manifest (damaged image?)";
    } catch (const Cancelled&) {
        res.cancelled = true;
        res.error = "cancelled";
    } catch (const std::exception& ex) {
        res.error = ex.what();
    }
    if (!part.empty()) {
        std::error_code ec;
        fs::remove(part, ec);
    }
    return res;
}

fs::path default_data_root(const fs::path& pref) { return pref / "data"; }

fs::path edition_dir(const fs::path& pref, Edition e) { return default_data_root(pref) / edition_info(e).key; }

std::vector<ImportedGame> list_imported(const fs::path& pref) {
    std::vector<ImportedGame> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(default_data_root(pref), ec)) {
        const fs::path j = e.path() / "import.json";
        if (!fs::is_regular_file(j, ec)) continue;
        try {
            const Bytes b = read_file(j);
            const std::string_view s(reinterpret_cast<const char*>(b.data()), b.size());
            out.push_back({e.path(), edition_from_key(json_field(s, "edition")), json_field(s, "source"), json_field(s, "imported")});
        } catch (const std::exception&) {
        }
    }
    std::sort(out.begin(), out.end(), [](const ImportedGame& a, const ImportedGame& b) { return a.dir < b.dir; });
    return out;
}

std::string generate_manifest(const std::vector<std::pair<Edition, const GameSource*>>& sources) {
    struct Item {
        std::string path;
        std::uint64_t size, hash;
        std::uint8_t mask;
    };
    std::map<std::tuple<std::string, std::uint64_t, std::uint64_t>, Item> items;
    for (const auto& [edition, src] : sources)
        for (const auto& f : src->files()) {
            if (is_ignored_file(f.path)) continue;
            const std::uint64_t h = hash_source(*src, f);
            auto [it, fresh] = items.try_emplace({ascii_lower(f.path), f.size, h}, Item{f.path, f.size, h, 0});
            it->second.mask |= edition_bit(edition);
        }
    std::string out = "// Generated by `parkan-install manifest` from:";
    for (const auto& [edition, src] : sources) out += std::string(" ") + edition_info(edition).key;
    out += ". {path, size, SHA-256 prefix, editions}\n";
    for (const auto& [key, it] : items) {
        std::string path;
        for (char c : it.path) {
            if (c == '"' || c == '\\') path += '\\';
            path += c;
        }
        char line[64];
        std::snprintf(line, sizeof line, ", %llu, 0x%016llxull, 0x%x},\n", static_cast<unsigned long long>(it.size),
                      static_cast<unsigned long long>(it.hash), unsigned(it.mask));
        out += "{\"" + path + "\"" + line;
    }
    return out;
}

} // namespace parkan::install
