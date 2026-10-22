#include "res/fx.hpp"

#include "res/library.hpp"

namespace parkan {

namespace {

constexpr std::size_t kHeaderSize = 0x3c;

// Record size per opcode, including the command word; 0 = never seen in the data.
std::size_t command_size(std::uint32_t op) {
    switch (op) {
    case 1: return 224;
    case 2: return 148;
    case 3: return 200;
    case 4: return 204;
    case 5: return 112;
    case 7: return 208;
    case 8: return 248;
    case 9: return 208;
    case 10: return 208;
    default: return 0;
    }
}

Vec3 vec3(Reader& r) {
    Vec3 v;
    v.x = r.f32(), v.y = r.f32(), v.z = r.f32();
    return v;
}

std::string fixed_str(Reader& r, std::size_t n) {
    std::string s = r.str(n);
    s.resize(std::min(s.find('\0'), s.size()));
    return s;
}

} // namespace

FxEffect decode_fx(std::span<const std::uint8_t> data) {
    Reader r(data);
    FxEffect fx;
    fx.commands = r.u32();
    r.seek(kHeaderSize);
    for (std::size_t i = 0; i < fx.commands; ++i) {
        const std::size_t start = r.pos();
        const std::uint32_t op = r.u32() & 0xff;
        const std::size_t size = command_size(op);
        if (!size) throw FormatError("FXID: unknown opcode " + std::to_string(op) + " at " + std::to_string(start));
        if (op == 2) {
            FxSound s;
            s.mode = r.u32();
            s.t0 = r.f32(), s.t1 = r.f32();
            s.pos0 = vec3(r), s.pos1 = vec3(r);
            s.dir0 = vec3(r), s.dir1 = vec3(r);
            s.min_distance = r.f32(), s.max_distance = r.f32();
            s.pitch0 = r.f32(), s.pitch1 = r.f32();
            s.user = r.u32();
            s.archive = fixed_str(r, 32);
            s.entry = fixed_str(r, 32);
            fx.sounds.push_back(std::move(s));
        }
        r.seek(start + size); // throws when the record runs past the entry
    }
    if (!r.at_end()) throw FormatError("FXID: " + std::to_string(r.remaining()) + " bytes after the last command");
    return fx;
}

std::vector<FxSound> load_fx_sounds(AssetLibrary& lib, std::string_view name) {
    const NResEntry* e = lib.find("effects.rlb", name);
    return e ? decode_fx(e->data).sounds : std::vector<FxSound>{};
}

} // namespace parkan
