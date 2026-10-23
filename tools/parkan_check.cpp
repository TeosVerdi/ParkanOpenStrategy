// Parses every supported data file in a game install and prints corpus totals, so each loader can
// be checked against the reference Python tools in ~/parkan-re/tools (same totals expected).
//
// Usage: parkan-check <game dir>
#include "ai/script_program.hpp"
#include "core/paths.hpp"
#include "input/translate_table.hpp"
#include "mission/research_tree.hpp"
#include "mission/scheme.hpp"
#include "mission/ske.hpp"
#include "mission/tma.hpp"
#include "scene/mission_scene.hpp"
#include "res/ctld.hpp"
#include "res/fx.hpp"
#include "res/library.hpp"
#include "res/material.hpp"
#include "res/mesh.hpp"
#include "res/nres.hpp"
#include "res/swav.hpp"
#include "res/terrain.hpp"
#include "res/textres.hpp"
#include "res/landmap.hpp"
#include "ai/pathfind.hpp"
#include "world/ground.hpp"
#include "res/texm.hpp"
#include "res/weapons.hpp"

#include <fstream>
#include <map>
#include <set>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <strings.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace fs = std::filesystem;
using namespace parkan;

namespace {

int check_scripts(const fs::path& game) {
    auto dir = resolve_path(game, "MISSIONS/SCRIPTS");
    if (!dir) {
        std::printf("scripts: MISSIONS/SCRIPTS not found\n");
        return 1;
    }
    int files = 0, failed = 0;
    std::size_t records = 0, calls = 0, vars = 0, formulas = 0;
    for (const auto& e : fs::directory_iterator(*dir)) {
        auto ext = e.path().extension().string();
        if (ext != ".scr" && ext != ".SCR") continue;
        try {
            auto p = ai::load_script_bundle(*dir, e.path().stem().string());
            ++files;
            vars = p.vars.size();
            formulas += p.formulas.size();
            for (const auto& ev : p.events) {
                records += ev.records.size();
                for (const auto& r : ev.records) calls += r.handler != ai::kNone;
            }
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  FAIL %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("scripts: %d parsed, %d failed; %zu records, %zu handler calls, %zu formulas; varset %zu vars\n",
                files, failed, records, calls, formulas, vars);
    return failed;
}

int check_research(const fs::path& game) {
    auto dir = resolve_path(game, "MISSIONS/SCRIPTS");
    if (!dir) return 1;
    int files = 0, failed = 0;
    std::size_t items = 0, available = 0, edges = 0;
    for (const auto& e : fs::directory_iterator(*dir)) {
        auto ext = e.path().extension().string();
        if (ext != ".trf" && ext != ".TRF") continue;
        try {
            auto t = mission::load_research_tree(e.path());
            ++files;
            items += t.items.size();
            for (const auto& it : t.items) {
                available += it.initial_state != 0;
                edges += it.prerequisites.size();
            }
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  FAIL %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("research: %d parsed, %d failed; %zu items, %zu available at start, %zu prerequisite edges\n",
                files, failed, items, available, edges);
    return failed;
}

struct Tally {
    int ok = 0, failed = 0;
    void fail(const std::string& where, const std::exception& ex) {
        if (failed++ < 5) std::printf("  FAIL %s: %s\n", where.c_str(), ex.what());
    }
};

bool is_nres(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char magic[4] = {};
    return f.read(magic, 4) && std::string_view(magic, 4) == "NRes";
}

int check_assets(const fs::path& game) {
    std::map<std::string, Tally> t;
    std::size_t vertices = 0, batches = 0, texels = 0, terrain_vertices = 0, terrain_faces = 0, motion_states = 0;
    for (const auto& e : fs::recursive_directory_iterator(game)) {
        if (!e.is_regular_file() || e.file_size() < 16) continue;
        const auto rel = fs::relative(e.path(), game).string();
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".wea") { // loose landscape/sky material lists
            try {
                auto b = read_file(e.path());
                parse_wear({reinterpret_cast<const char*>(b.data()), b.size()});
                ++t["WEAR (loose)"].ok;
            } catch (const std::exception& ex) { t["WEAR (loose)"].fail(rel, ex); }
            continue;
        }
        if (!is_nres(e.path())) continue;
        std::optional<NResArchive> arc;
        try { arc.emplace(NResArchive::open(e.path())); } catch (const std::exception& ex) { t["NRes"].fail(rel, ex); continue; }
        ++t["NRes"].ok;
        if (ext == ".map") continue; // Land.map (ArealMap): loader still to write
        if (ext == ".msh") { // Land.msh landscapes are whole files
            try {
                auto tr = decode_terrain(read_file(e.path()));
                terrain_vertices += tr.positions.size();
                terrain_faces += tr.faces.size();
                ++t["Land.msh"].ok;
            } catch (const std::exception& ex) { t["Land.msh"].fail(rel, ex); }
            continue;
        }
        for (const auto& en : arc->entries()) {
            const std::string where = rel + ":" + en.name;
            try {
                if (en.type == fourcc("MESH")) {
                    auto m = decode_mesh(en.data);
                    vertices += m.positions.size(); batches += m.batches.size();
                    ++t["MESH"].ok;
                } else if (en.type == fourcc("Texm")) {
                    auto x = decode_texm(en.data);
                    texels += x.mips_rgba.front().size() / 4;
                    ++t["Texm"].ok;
                } else if (en.type == fourcc("MAT0")) {
                    decode_mat0(en.data, en.attr1, en.attr2);
                    ++t["MAT0"].ok;
                } else if (en.type == fourcc("WEAR")) {
                    parse_wear({reinterpret_cast<const char*>(en.data.data()), en.data.size()});
                    ++t["WEAR"].ok;
                } else if (en.type == fourcc("CTLD")) {
                    auto c = decode_ctld(en.data);
                    motion_states += c.states.size();
                    ++t["CTLD"].ok;
                }
            } catch (const std::exception& ex) {
                const char* kind = en.type == fourcc("MESH") ? "MESH" : en.type == fourcc("Texm") ? "Texm"
                                 : en.type == fourcc("MAT0") ? "MAT0" : en.type == fourcc("CTLD") ? "CTLD" : "WEAR";
                t[kind].fail(where, ex);
            }
        }
    }
    int bad = 0;
    for (auto& [k, v] : t) {
        std::printf("%-13s %5d ok, %d failed\n", k.c_str(), v.ok, v.failed);
        bad += v.failed;
    }
    std::printf("assets: %zu mesh vertices, %zu batches, %zu level-0 texels; terrain %zu vertices, %zu faces; "
                "%zu motion states\n",
                vertices, batches, texels, terrain_vertices, terrain_faces, motion_states);
    return bad;
}

// notes/audio.md §1: every SWAV decodes, every FX sound command resolves (except the one entry the
// data itself lacks), every MUSIC/Track*.ogg opens (or Track*.wav, ripped from a CD by parkan-install).
int check_audio(const fs::path& game) {
    AssetLibrary lib(game);
    int bad = 0, swav = 0;
    double seconds = 0;
    for (const char* name : {"sounds.lib", "voices.lib"}) {
        try {
            for (const auto& e : lib.archive(name).entries()) {
                try {
                    if (e.type != fourcc("SWAV")) throw FormatError("entry type is not SWAV");
                    const Pcm p = decode_swav(e.data);
                    if (p.rate != 22050) throw FormatError("rate " + std::to_string(p.rate));
                    seconds += p.seconds();
                    ++swav;
                } catch (const std::exception& ex) {
                    if (bad++ < 5) std::printf("  FAIL %s:%s: %s\n", name, e.name.c_str(), ex.what());
                }
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::printf("  FAIL %s: %s\n", name, ex.what());
        }
    }
    // fortif_door_move.wav (FX b_turret) is absent from sounds.lib in the shipped data (audio.md §1.3).
    const std::set<std::string> known_missing = {"fortif_door_move.wav"};
    int effects = 0, fx_sounds = 0, resolved = 0, missing_known = 0;
    std::set<std::string> names;
    for (const auto& e : lib.archive("effects.rlb").entries()) {
        try {
            const auto fx = decode_fx(e.data);
            ++effects;
            for (const auto& s : fx.sounds) {
                ++fx_sounds;
                names.insert(s.entry);
                if (lib.find(s.archive, s.entry)) {
                    ++resolved;
                } else if (known_missing.count(s.entry)) {
                    ++missing_known;
                } else {
                    ++bad;
                    std::printf("  FAIL %s: sound %s/%s not found\n", e.name.c_str(), s.archive.c_str(), s.entry.c_str());
                }
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::printf("  FAIL effects.rlb:%s: %s\n", e.name.c_str(), ex.what());
        }
    }
    int tracks = 0;
    double music = 0;
    if (auto dir = resolve_path(game, "MUSIC"))
        for (const auto& e : fs::directory_iterator(*dir)) {
            const auto f = e.path().filename().string();
            if (f.size() != 11 || strncasecmp(f.c_str(), "track", 5) != 0) continue;
            if (strcasecmp(f.c_str() + 7, ".wav") == 0) {
                // Ripped from a CD by parkan-install: 44-byte header, 44.1 kHz stereo 16-bit PCM.
                std::uint8_t h[44] = {};
                std::ifstream(e.path(), std::ios::binary).read(reinterpret_cast<char*>(h), 44);
                std::uint32_t data = 0;
                std::memcpy(&data, h + 40, 4);
                if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVEfmt ", 8) != 0 || h[20] != 1 || h[22] != 2 ||
                    std::memcmp(h + 36, "data", 4) != 0 || fs::file_size(e.path()) != 44 + std::uint64_t(data)) {
                    ++bad;
                    std::printf("  FAIL MUSIC/%s: not a 44-byte-header PCM WAV\n", f.c_str());
                    continue;
                }
                ++tracks;
                music += data / 176400.0;
                continue;
            }
            if (strcasecmp(f.c_str() + 7, ".ogg") != 0) continue;
            int err = 0;
            stb_vorbis* v = stb_vorbis_open_filename(e.path().string().c_str(), &err, nullptr);
            if (!v) {
                ++bad;
                std::printf("  FAIL MUSIC/%s: stb_vorbis error %d\n", f.c_str(), err);
                continue;
            }
            ++tracks;
            music += stb_vorbis_stream_length_in_seconds(v);
            stb_vorbis_close(v);
        }
    std::printf("audio: %d SWAV decoded (%.1f s), %d failed; effects %d, %d sound commands "
                "(%zu names): %d resolved, %d known-missing; music %d tracks, %.1f s\n",
                swav, seconds, bad, effects, fx_sounds, names.size(), resolved, missing_known, tracks, music);
    return bad + (tracks ? 0 : 1);
}

// notes/effects.md: every FXID effect and EXPL explosion definition decodes; every effect material
// and EXPL effect resolves.
int check_effects(const fs::path& game) {
    AssetLibrary lib(game);
    int bad = 0, effects = 0, materials = 0, expl = 0, surfaces = 0;
    std::map<std::uint32_t, int> ops;
    for (const auto& e : lib.archive("effects.rlb").entries()) {
        try {
            const FxEffect fx = decode_fx(e.data);
            ++effects;
            for (const auto& c : fx.list) {
                ++ops[c.opcode];
                const FxResourceRef* m = nullptr;
                if (auto* x = std::get_if<FxSpriteCmd>(&c.data)) m = &x->material;
                if (auto* x = std::get_if<FxBeamCmd>(&c.data)) m = &x->material;
                if (auto* x = std::get_if<FxBurstCmd>(&c.data)) m = &x->material;
                if (auto* x = std::get_if<FxTrailCmd>(&c.data)) m = &x->material;
                if (!m || m->name.empty()) continue;
                ++materials;
                if (!lib.find("Material.lib", m->name)) {
                    ++bad;
                    std::printf("  FAIL effects.rlb:%s: material %s not found\n", e.name.c_str(), m->name.c_str());
                }
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::printf("  FAIL effects.rlb:%s: %s\n", e.name.c_str(), ex.what());
        }
    }
    for (const char* a : {"animals.rlb", "static.rlb", "system.rlb", "turrets.rlb", "weapon.rlb", "bases.rlb",
                          "guns.rlb", "fortif.rlb", "parts.rlb", "objects.rlb", "intsys.rlb"}) {
        try {
            for (const auto& e : lib.archive(a).entries()) {
                if (e.type != fourcc("EXPL")) continue;
                try {
                    const ExplosionDef x = decode_expl(e.data);
                    ++expl;
                    if (!x.fx[0].second.empty() && !lib.find(x.fx[0].first, x.fx[0].second)) {
                        ++bad;
                        std::printf("  FAIL %s:%s: effect %s not found\n", a, e.name.c_str(), x.fx[0].second.c_str());
                    }
                    for (std::size_t k = 1; k < x.fx.size(); ++k)
                        surfaces += x.fx_valid[k] && lib.find(x.fx[k].first, x.fx[k].second);
                } catch (const std::exception& ex) {
                    ++bad;
                    std::printf("  FAIL %s:%s: %s\n", a, e.name.c_str(), ex.what());
                }
            }
        } catch (const std::exception&) {
            // archive absent in this release
        }
    }
    std::printf("effects: %d FXID decoded (op1 %d, op2 %d, op3 %d, op4 %d, op5 %d, op7 %d, op8 %d, op9 %d, op10 %d), "
                "%d material refs; %d EXPL decoded, %d surface variants; %d failed\n",
                effects, ops[1], ops[2], ops[3], ops[4], ops[5], ops[7], ops[8], ops[9], ops[10], materials, expl,
                surfaces, bad);
    return bad;
}

int check_missions(const fs::path& game) {
    auto dir = resolve_path(game, "MISSIONS");
    if (!dir) return 1;
    int files = 0, failed = 0;
    std::size_t paths = 0, clans = 0, objects = 0, extras = 0;
    for (const auto& e : fs::recursive_directory_iterator(*dir)) {
        auto name = e.path().filename().string();
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != "data.tma") continue;
        try {
            auto t = mission::decode_tma(read_file(e.path()));
            ++files;
            paths += t.paths.size(), clans += t.clans.size(), objects += t.objects.size(), extras += t.extras.size();
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  FAIL %s: %s\n", fs::relative(e.path(), game).string().c_str(), ex.what());
        }
    }
    // Every mission's sky.ske parses and evaluates.
    int skes = 0, ske_fail = 0;
    std::size_t ske_layers = 0;
    for (const auto& e : fs::recursive_directory_iterator(*dir)) {
        auto name = e.path().filename().string();
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != "sky.ske") continue;
        try {
            const auto s = mission::decode_ske(read_file(e.path()));
            for (const auto& k : s.keys) ske_layers += k.layers.size();
            for (double t = 0; t < 3600; t += 37) mission::evaluate_ske(s, t);
            ++skes;
        } catch (const std::exception& ex) {
            ++ske_fail;
            std::printf("  FAIL %s: %s\n", fs::relative(e.path(), game).string().c_str(), ex.what());
        }
    }
    std::printf("skies: %d sky.ske parsed, %d failed, %zu layers\n", skes, ske_fail, ske_layers);
    failed += ske_fail;
    // Build every mission scene: terrain + every placed object resolved to a model.
    AssetLibrary lib(game);
    std::size_t placed = 0, unresolved = 0;
    int scene_fail = 0;
    for (const auto& e : fs::recursive_directory_iterator(*dir)) {
        auto name = e.path().filename().string();
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != "data.tma") continue;
        const auto rel = fs::relative(e.path().parent_path(), game).string();
        try {
            MissionSceneStats st;
            load_mission_render_mesh(lib, rel, &st);
            placed += st.placed, unresolved += st.unresolved;
            if (st.unresolved) std::printf("  %s: %zu unresolved objects\n", rel.c_str(), st.unresolved);
        } catch (const std::exception& ex) {
            ++scene_fail;
            std::printf("  SCENE FAIL %s: %s\n", rel.c_str(), ex.what());
        }
    }
    std::printf("scenes: %zu objects placed, %zu unresolved, %d missions failed\n", placed, unresolved, scene_fail);
    failed += scene_fail;
    std::printf("missions: %d data.tma parsed, %d failed; %zu paths, %zu clans, %zu objects, %zu extras\n", files,
                failed, paths, clans, objects, extras);
    return failed;
}

// Every unit design under UNITS: the scheme tree parses, and every design assembles with all of its
// external components on valid mount nodes.
int check_schemes(const fs::path& game) {
    auto dir = resolve_path(game, "UNITS");
    if (!dir) return 1;
    AssetLibrary lib(game);
    int files = 0, failed = 0, dangling = 0;
    std::size_t nodes = 0;
    MissionSceneStats st;
    for (const auto& e : fs::recursive_directory_iterator(*dir)) {
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".dat") continue;
        const auto rel = fs::relative(e.path(), game).string();
        try {
            const auto s = mission::decode_scheme(read_file(e.path()));
            ++files;
            auto count = [&](auto&& self, const mission::SchemeNode& n) -> void {
                ++nodes;
                for (const auto& c : n.children) self(self, c);
            };
            count(count, s.root);
            // Two shipped designs (mas_l_n1, tel_l_n1) name prototypes missing from objects.rlb.
            if (!load_scheme_render_mesh(lib, rel, &st)) {
                ++dangling;
                std::printf("  %s: base prototype %s is not in objects.rlb\n", rel.c_str(), s.root.entry.c_str());
            }
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  FAIL %s: %s\n", rel.c_str(), ex.what());
        }
    }
    std::printf("schemes: %d .dat parsed, %d failed, %d dangling; %zu nodes, %zu external components mounted, "
                "%zu bad mounts\n",
                files, failed, dangling, nodes, st.components, st.bad_components);
    return failed + int(st.bad_components);
}

// Ground probes: on every map, a probe just above the centroid of every 7th LOD0 face lands on that
// face's plane height (or on a higher face stacked over it).
// Land.map on every map: areals, walkable ones, vertices, links, grid cells and entries, and the
// areal graph's edges (expected totals from the reference parser, tools/landmap.py's layout).
int check_landmaps(const fs::path& game) {
    auto maps = resolve_path(game, "DATA/MAPS");
    if (!maps) return 1;
    int map_count = 0, bad_maps = 0;
    std::size_t areals = 0, walkable = 0, verts = 0, links = 0, cells = 0, entries = 0, edges = 0;
    for (const auto& e : fs::directory_iterator(*maps)) {
        auto file = resolve_path(e.path(), "Land.map");
        if (!file) continue;
        ++map_count;
        try {
            const LandMap m = decode_landmap(read_file(*file));
            areals += m.areals.size();
            for (const auto& a : m.areals) {
                walkable += a.walkable != 0;
                verts += a.verts.size();
                for (const auto& l : a.links) links += l[0] != -1;
            }
            cells += std::size_t(m.cells_x) * m.cells_y;
            entries += m.cell_ids.size();
            for (const auto& n : ai::ArealGraph::build(m).nodes) edges += n.edges.size();
        } catch (const std::exception& ex) {
            ++bad_maps;
            std::printf("  FAIL %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("landmaps: %d maps, %d failed; %zu areals (%zu walkable), %zu vertices, %zu links, %zu grid cells, "
                "%zu grid entries, %zu graph edges\n",
                map_count, bad_maps, areals, walkable, verts, links, cells, entries, edges);
    return bad_maps;
}

int check_ground(const fs::path& game) {
    auto maps = resolve_path(game, "DATA/MAPS");
    if (!maps) return 1;
    int map_count = 0, bad_maps = 0;
    std::size_t probes = 0, misses = 0, below = 0;
    for (const auto& e : fs::directory_iterator(*maps)) {
        auto land = resolve_path(e.path(), "Land.msh");
        if (!land) continue;
        ++map_count;
        try {
            const Terrain t = decode_terrain(read_file(*land));
            const TerrainGround g(t);
            std::set<std::uint32_t> lod0;
            for (const auto& n : t.nodes)
                if (n.slots[0] != 0xFFFF)
                    for (std::size_t i = 0; i < t.slots[n.slots[0]].tri_count; ++i)
                        lod0.insert(t.draw_list[t.slots[n.slots[0]].tri_start + i].face);
            std::size_t k = 0;
            for (std::uint32_t f : lod0) {
                if (k++ % 7) continue;
                const auto& face = t.faces[f];
                if (face.flags & 0x20000) continue;
                Vec3 c{};
                for (auto v : face.v) c = c + Vec3{t.positions[v][0], t.positions[v][1], t.positions[v][2]} * (1.f / 3);
                const auto hit = g.probe(c + Vec3{0, 0, 0.5f});
                ++probes;
                if (!hit) ++misses;
                else if (hit->z < c.z - 0.01f) ++below;
            }
        } catch (const std::exception& ex) {
            ++bad_maps;
            std::printf("  FAIL %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("ground: %d maps, %d failed; %zu probes, %zu missed, %zu below their face\n", map_count, bad_maps,
                probes, misses, below);
    return bad_maps;
}

// TextRes.dll's string tables (the game's Russian text).
int check_textres(const fs::path& game) {
    auto p = resolve_path(game, "DATA/TextRes.dll");
    if (!p) {
        std::printf("textres: TextRes.dll missing\n");
        return 1;
    }
    try {
        TextRes t = TextRes::from_pe(read_file(*p));
        if (auto cfg = resolve_path(game, "DATA/TextRes.cfg")) {
            const Bytes b = read_file(*cfg);
            t.load_names({reinterpret_cast<const char*>(b.data()), b.size()});
        }
        if (t.get(std::string_view("MIS_OBJ")).empty()) throw std::runtime_error("MIS_OBJ not resolved through TextRes.cfg");
        std::size_t bytes = 0;
        for (const auto& [id, s] : t.all()) bytes += s.size();
        std::printf("textres: %zu strings (ids %u..%u), %zu UTF-8 bytes\n", t.size(), t.all().begin()->first,
                    t.all().rbegin()->first, bytes);
        return t.size() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::printf("textres: FAIL %s\n", ex.what());
        return 1;
    }
}

// The vehicle-control binding tables parse with every name known.
int check_bindings(const fs::path& game) {
    int failed = 0;
    std::size_t bindings = 0;
    for (const char* name : {"m1.tbl", "m2.tbl", "hero.tbl"}) {
        auto path = resolve_path(game, name);
        if (!path) {
            ++failed;
            std::printf("  missing %s\n", name);
            continue;
        }
        try {
            const Bytes b = read_file(*path);
            bindings += input::parse_translate_table({reinterpret_cast<const char*>(b.data()), b.size()}).size();
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  FAIL %s: %s\n", name, ex.what());
        }
    }
    std::printf("bindings: 3 tables, %d failed, %zu binding lines\n", failed, bindings);
    return failed;
}

// notes/weapons-damage.md §1: every weapon device's bullet resolves (BULL → CTLD, NDPR, EXPL); the one
// BULL whose data is missing (bm_b_02, unused) is reported, not fatal. Same totals as
// tools/weapons_dump.py --check.
int check_weapons(const fs::path& game) {
    AssetLibrary lib(game);
    try {
        const WeaponCorpus w = scan_weapon_corpus(lib);
        std::printf("weapons: %zu weapon devices, %zu bullets (%zu BULL records), %zu EXPL entries; %zu NDPR (%zu parts)\n",
                    w.weapon_devices, w.bullets, w.bull_records, w.expl_entries, w.ndpr_entries, w.ndpr_parts);
        for (const auto& u : w.unresolved) std::printf("  unused BULL with missing data: %s\n", u.c_str());
        return 0;
    } catch (const std::exception& ex) {
        std::printf("weapons: FAIL %s\n", ex.what());
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <game dir>\n", argv[0]);
        return 2;
    }
    fs::path game = argv[1];
    int bad = check_scripts(game) + check_research(game) + check_missions(game) + check_schemes(game) + check_bindings(game) + check_ground(game) + check_textres(game) + check_landmaps(game) +
              check_assets(game) + check_audio(game) + check_effects(game) + check_weapons(game);
    return bad ? 1 : 0;
}
