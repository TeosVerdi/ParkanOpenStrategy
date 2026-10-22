// Developer dumps of single resources.
//   parkan-dump <game dir> nodes <archive> <mesh>         MSH node table (index, parent, flags, name, static pose)
//   parkan-dump <game dir> anim <archive> <mesh> <node>   per-frame key index, key time and position of one node
//   parkan-dump <game dir> tma <mission dir>               placed objects of a mission's data.tma
#include "core/paths.hpp"
#include "mission/tma.hpp"
#include "res/library.hpp"
#include "res/mesh.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <game dir> nodes <archive> <mesh>\n", argv[0]);
        return 2;
    }
    try {
        parkan::AssetLibrary lib(argv[1]);
        const std::string what = argv[2];
        if (what == "nodes" && argc == 5) {
            const parkan::NResEntry* e = lib.find(argv[3], argv[4]);
            if (!e) throw std::runtime_error("entry not found");
            const parkan::Mesh m = parkan::decode_mesh(e->data);
            for (std::size_t i = 0; i < m.nodes.size(); ++i) {
                const auto& n = m.nodes[i];
                std::printf("%3zu parent=%5d flags=%04x slot0=%5d key=%4u %-20s", i, n.parent == 0xFFFF ? -1 : int(n.parent),
                            n.flags, n.slots[0] == 0xFFFF ? -1 : int(n.slots[0]), n.fallback_key, n.name.c_str());
                if (n.fallback_key < m.keys.size()) {
                    const auto& k = m.keys[n.fallback_key];
                    std::printf(" pos=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f)", k.pos[0], k.pos[1], k.pos[2], k.quat[0],
                                k.quat[1], k.quat[2], k.quat[3]);
                }
                std::printf("\n");
            }
            return 0;
        }
        if (what == "anim" && argc == 6) {
            const parkan::NResEntry* e = lib.find(argv[3], argv[4]);
            if (!e) throw std::runtime_error("entry not found");
            const parkan::Mesh m = parkan::decode_mesh(e->data);
            const std::size_t node = std::size_t(std::atoi(argv[5]));
            if (node >= m.nodes.size()) throw std::runtime_error("no such node");
            const auto& n = m.nodes[node];
            std::printf("frames=%u keys=%zu map=%zu anim_map_start=%u fallback_key=%u\n", m.frame_count, m.keys.size(),
                        m.frame_map.size(), n.anim_map_start, n.fallback_key);
            for (std::uint32_t f = 0; n.anim_map_start != 0xFFFF && f < m.frame_count; ++f) {
                const std::size_t mi = std::size_t(n.anim_map_start) + f;
                if (mi >= m.frame_map.size()) break;
                const unsigned k = m.frame_map[mi];
                std::printf("  frame %4u key %5u", f, k);
                if (k < m.keys.size())
                    std::printf(" t=%7.2f pos=(%.3f %.3f %.3f)", m.keys[k].time, m.keys[k].pos[0], m.keys[k].pos[1],
                                m.keys[k].pos[2]);
                std::printf("\n");
            }
            return 0;
        }
        if (what == "tma" && argc == 4) {
            auto dir = parkan::resolve_path(argv[1], argv[3]);
            auto path = dir ? parkan::resolve_path(*dir, "data.tma") : std::nullopt;
            if (!path) throw std::runtime_error("no data.tma");
            const auto t = parkan::mission::decode_tma(parkan::read_file(*path));
            for (std::size_t i = 0; i < t.clans.size(); ++i)
                std::printf("clan %zu %s id=%d mode=%u script=%s\n", i, t.clans[i].name.c_str(), t.clans[i].raw_id,
                            t.clans[i].mode, t.clans[i].script.c_str());
            for (std::size_t i = 0; i < t.objects.size(); ++i) {
                const auto& o = t.objects[i];
                std::printf("%3zu kind=%u flags=%08x ai=%u id=%08x pos=(%.0f %.0f %.0f) %-28s %s\n", i, o.kind, o.class_flags,
                            o.ai_index, o.identity, o.position[0], o.position[1], o.position[2], o.resource.c_str(),
                            o.instance_name.c_str());
            }
            return 0;
        }
        std::fprintf(stderr, "unknown command\n");
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
