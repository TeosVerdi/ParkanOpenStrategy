#include "res/weapons.hpp"

#include "control/motion_graph.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <map>

namespace parkan {

namespace {

std::string fixed(std::span<const std::uint8_t> b) {
    const auto* p = reinterpret_cast<const char*>(b.data());
    return std::string(p, ::strnlen(p, b.size()));
}

template <class T>
T at(std::span<const std::uint8_t> p, std::size_t off) {
    T v;
    std::memcpy(&v, p.data() + off, sizeof v);
    return v;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

const NResEntry& require(AssetLibrary& lib, const ResName& n, std::string_view type) {
    const NResEntry* e = lib.find(n.first, n.second);
    if (!e) throw FormatError(n.first + "/" + n.second + " missing");
    if (e->type != fourcc(type)) throw FormatError(n.first + "/" + n.second + " is not " + std::string(type));
    return *e;
}

} // namespace

bool is_weapon_device(const CtldDevice& d) { return d.type == 2 || d.type == 0x1e; }

GunDeviceSpec decode_gun_device(const CtldDevice& d) {
    if (!is_weapon_device(d)) throw FormatError("CTLD device type " + std::to_string(d.type) + " is not a weapon");
    const std::span<const std::uint8_t> h(d.header);
    GunDeviceSpec g;
    g.type = d.type;
    g.id = at<std::int32_t>(h, 0x04);
    g.flags = at<std::uint32_t>(h, 0x08);
    g.event = at<std::int32_t>(h, 0x0c);
    g.part = at<std::int32_t>(h, 0x18);
    for (std::size_t i = 0; i < 16; ++i) g.param[i] = at<float>(h, 0x2c + 4 * i);
    g.bullet = {fixed(h.subspan(0x6c, 32)), fixed(h.subspan(0x8c, 32))};
    g.barrels = d.joints;
    g.module = d.text;
    return g;
}

std::vector<NdprPart> decode_ndpr(std::span<const std::uint8_t> data) {
    Reader r(data);
    const std::uint32_t n = r.u32();
    if (data.size() != 4 + std::size_t(n) * 76) throw FormatError("NDPR size " + std::to_string(data.size()) + " != 4 + 76·" + std::to_string(n));
    std::vector<NdprPart> out(n);
    for (auto& p : out) {
        p.flags = r.u32();
        p.hit_points = r.f32();
        p.density = r.f32();
        p.effect.first = fixed(r.bytes(32));
        p.effect.second = fixed(r.bytes(32));
    }
    return out;
}

BullRecord decode_bull(std::span<const std::uint8_t> data) {
    if (data.size() != 320) throw FormatError("BULL size " + std::to_string(data.size()) + " != 320");
    BullRecord b;
    for (std::size_t i = 0; i < 5; ++i) b.pairs[i] = {fixed(data.subspan(64 * i, 32)), fixed(data.subspan(64 * i + 32, 32))};
    return b;
}

BulletProto load_bullet(AssetLibrary& lib, std::string_view name) {
    BulletProto b;
    b.name = std::string(name);
    b.record = decode_bull(require(lib, {"objects.rlb", b.name}, "BULL").data);
    const Ctld c = decode_ctld(require(lib, b.record.ctld(), "CTLD").data);
    b.speed = c.body[7];
    b.range = c.body[22];
    b.turn = {c.body[9], c.body[10], c.body[11]};
    b.removal_ms = std::bit_cast<std::int32_t>(c.body[18]);
    b.physics = std::bit_cast<std::int32_t>(c.body[21]);
    b.parts = decode_ndpr(require(lib, b.record.ndpr(), "NDPR").data);
    if (b.parts.empty()) throw FormatError("bullet " + b.name + ": NDPR without parts");
    if (b.parts[0].has_effect()) b.expl = decode_expl(require(lib, b.parts[0].effect, "EXPL").data);
    for (const auto& d : c.devices)
        if (d.type == 0x11 && !b.seeker) b.seeker = std::array{at<float>(d.header, 0x2c), at<float>(d.header, 0x30), at<float>(d.header, 0x34)};
    auto list = [&](std::size_t situation) -> const std::vector<CtldEvent>* {
        const std::int32_t i = c.situations[situation];
        return i >= 0 && std::size_t(i) < c.event_lists.size() ? &c.event_lists[std::size_t(i)] : nullptr;
    };
    std::vector<ContactPoint> points;
    if (const NResEntry* pe = lib.find(b.record.pairs[2].first, b.record.pairs[2].second)) points = decode_ctpt(pe->data);
    if (const auto* l = list(0))
        for (const auto& e : *l)
            if (e.type == 4 && !e.entry.empty()) {
                b.spawn_effects.push_back({e.lib, e.entry});
                BulletProto::SpawnPoints sp;
                for (std::size_t k = 0; k < 3; ++k) {
                    std::uint32_t i = 0;
                    std::memcpy(&i, e.raw.data() + 0x10 + 4 * k, 4);
                    if (i >= points.size()) throw FormatError("bullet " + b.name + ": effect point " + std::to_string(i) + " out of range");
                    sp.pos[k] = points[i].pos, sp.vec[k] = points[i].vec;
                }
                b.spawn_points.push_back(sp);
            }
    if (const auto* l = list(4))
        for (const auto& e : *l)
            if (e.type == 0x1b && !e.lib.empty() && !e.entry.empty()) {
                b.range_expl_name = {e.lib, e.entry};
                b.range_expl = decode_expl(require(lib, b.range_expl_name, "EXPL").data);
            }
    return b;
}

WeaponCorpus scan_weapon_corpus(AssetLibrary& lib) {
    WeaponCorpus w;
    std::vector<std::string> archives;
    for (const auto& e : std::filesystem::directory_iterator(lib.game_dir())) {
        if (!e.is_regular_file() || lower(e.path().extension().string()) != ".rlb") continue;
        std::ifstream f(e.path(), std::ios::binary);
        char magic[4] = {};
        if (f.read(magic, 4) && std::string_view(magic, 4) == "NRes") archives.push_back(e.path().filename().string());
    }
    std::sort(archives.begin(), archives.end());
    std::map<std::string, bool> bullets; // resolved names (lower case)
    for (const auto& a : archives)
        for (const auto& en : lib.archive(a).entries()) {
            if (en.type == fourcc("NDPR")) {
                w.ndpr_parts += decode_ndpr(en.data).size();
                ++w.ndpr_entries;
            }
            if (en.type != fourcc("CTLD")) continue;
            for (const auto& d : decode_ctld(en.data).devices) {
                if (!is_weapon_device(d)) continue;
                ++w.weapon_devices;
                const GunDeviceSpec g = decode_gun_device(d);
                if (lower(g.bullet.first) != "objects.rlb") throw FormatError(a + "/" + en.name + ": bullet library " + g.bullet.first);
                if (bullets.emplace(lower(g.bullet.second), true).second) load_bullet(lib, g.bullet.second);
            }
        }
    for (const auto& en : lib.archive("objects.rlb").entries()) {
        if (en.type != fourcc("BULL")) continue;
        ++w.bull_records;
        if (bullets.count(lower(en.name))) continue;
        try {
            load_bullet(lib, en.name);
            bullets.emplace(lower(en.name), true);
        } catch (const FormatError& ex) {
            w.unresolved.push_back(en.name + ": " + ex.what());
        }
    }
    w.bullets = bullets.size();
    for (const auto& en : lib.archive("weapon.rlb").entries())
        if (en.type == fourcc("EXPL")) {
            decode_expl(en.data);
            ++w.expl_entries;
        }
    return w;
}

} // namespace parkan
