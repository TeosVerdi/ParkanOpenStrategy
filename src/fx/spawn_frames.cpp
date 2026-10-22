#include "fx/spawn_frames.hpp"

#include "control/motion_graph.hpp"
#include "fx/effect.hpp"
#include "res/ctld.hpp"
#include "res/weapons.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace parkan::fx {

std::map<std::string, Mat4> collect_spawn_frames(AssetLibrary& lib) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto scale_of = [](const Mat4& m) {
        float s = 0;
        for (int c = 0; c < 3; ++c)
            s = std::max(s, std::sqrt(m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c)));
        return s;
    };
    std::map<std::string, Mat4> frames;
    auto add = [&](const std::string& fx, const Mat4& m) {
        auto [it, fresh] = frames.try_emplace(lower(fx), m);
        if (!fresh && scale_of(m) > scale_of(it->second)) it->second = m;
    };
    for (const auto& e : lib.archive("objects.rlb").entries()) {
        if (e.type != fourcc("BULL")) continue;
        try {
            const BulletProto b = load_bullet(lib, e.name);
            for (std::size_t i = 0; i < b.spawn_effects.size(); ++i)
                add(b.spawn_effects[i].second, contact_frame(b.spawn_points[i].pos, b.spawn_points[i].vec));
        } catch (const std::exception&) {
            // BULL records no weapon uses whose data is missing
        }
    }
    for (const char* a : {"guns.rlb", "bases.rlb", "static.rlb", "fortif.rlb", "turrets.rlb", "animals.rlb", "system.rlb"}) {
        const NResArchive* ar = nullptr;
        try {
            ar = &lib.archive(a);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& e : ar->entries()) {
            if (e.type != fourcc("CTLD")) continue;
            const NResEntry* pe = ar->find_name(e.name.substr(0, e.name.rfind('.')) + ".cpt");
            if (!pe) continue;
            const auto pts = decode_ctpt(pe->data);
            const Ctld c = decode_ctld(e.data);
            for (const auto& l : c.event_lists)
                for (const auto& ev : l) {
                    if (ev.type != 4 || ev.entry.empty()) continue;
                    std::array<Vec3, 3> pos{}, vec{};
                    bool ok = true;
                    for (std::size_t k = 0; k < 3; ++k) {
                        std::uint32_t i = 0;
                        std::memcpy(&i, ev.raw.data() + 0x10 + 4 * k, 4);
                        if (i >= pts.size()) ok = false;
                        else pos[k] = pts[i].pos, vec[k] = pts[i].vec;
                    }
                    if (ok) add(ev.entry, contact_frame(pos, vec));
                }
        }
    }
    return frames;
}

} // namespace parkan::fx
