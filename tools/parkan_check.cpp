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
#include "res/material.hpp"
#include "res/mesh.hpp"
#include "res/nres.hpp"
#include "res/terrain.hpp"
#include "world/ground.hpp"
#include "res/texm.hpp"

#include <fstream>
#include <map>
#include <set>

#include <cstdio>
#include <filesystem>

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

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <game dir>\n", argv[0]);
        return 2;
    }
    fs::path game = argv[1];
    int bad = check_scripts(game) + check_research(game) + check_missions(game) + check_schemes(game) + check_bindings(game) + check_ground(game) +
              check_assets(game);
    return bad ? 1 : 0;
}
