#include "res/fx.hpp"

#include "res/library.hpp"

namespace parkan {

namespace {

constexpr std::size_t kHeaderSize = 0x3c;

Vec3 vec3(Reader& r) {
    Vec3 v;
    v.x = r.f32(), v.y = r.f32(), v.z = r.f32();
    return v;
}

std::array<float, 3> f3(Reader& r) { return {r.f32(), r.f32(), r.f32()}; }
std::array<float, 4> f4(Reader& r) { return {r.f32(), r.f32(), r.f32(), r.f32()}; }

std::string fixed_str(Reader& r, std::size_t n) {
    std::string s = r.str(n);
    s.resize(std::min(s.find('\0'), s.size()));
    return s;
}

FxResourceRef resource(Reader& r) {
    FxResourceRef ref;
    ref.archive = fixed_str(r, 32);
    ref.name = fixed_str(r, 32);
    return ref;
}

FxLightCmd light(Reader& r) {
    FxLightCmd c;
    c.kind = r.u32();
    if (c.kind < 1 || c.kind > 7) throw FormatError("FXID light: kind " + std::to_string(c.kind));
    c.t0 = r.f32(), c.t1 = r.f32();
    c.pos0 = vec3(r), c.pos1 = vec3(r), c.dir0 = vec3(r), c.dir1 = vec3(r);
    c.color0 = f4(r), c.color1 = f4(r), c.color_jitter = f4(r);
    c.range0 = r.f32(), c.range1 = r.f32(), c.range_jitter = r.f32();
    c.attenuation = f3(r);
    return c; // +0x88..0xdf: unused by FxLight_Init/Update (one record carries editor garbage)
}

FxSound sound(Reader& r) {
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
    return s;
}

FxSpriteCmd sprite(Reader& r, std::uint32_t op) {
    FxSpriteCmd c;
    c.align = r.u32();
    if (c.align > 2) throw FormatError("FXID sprite: alignment " + std::to_string(c.align));
    c.frame0 = r.f32(), c.frame1 = r.f32(), c.frame_exp = r.f32();
    c.alpha0 = r.f32(), c.alpha1 = r.f32(), c.alpha_exp = r.f32();
    c.t0 = r.f32(), c.t1 = r.f32();
    c.pos0 = vec3(r), c.pos1 = vec3(r), c.pos_exp = f3(r);
    c.axis0 = vec3(r), c.axis1 = vec3(r);
    c.size0 = vec3(r), c.size1 = vec3(r), c.size_exp = f3(r);
    c.material = resource(r);
    if (op == 4) {
        c.distance_norm = r.f32();
        if (!(c.distance_norm > 0)) throw FormatError("FXID op 4: distance normaliser not positive");
    } else if (op == 9) {
        c.shade_mode = r.u32(), c.shade_flag = r.u32();
        if (c.shade_mode > 2) throw FormatError("FXID op 9: shade mode " + std::to_string(c.shade_mode));
    }
    return c;
}

FxBeamCmd beam(Reader& r) {
    FxBeamCmd c;
    c.alpha0 = r.f32(), c.alpha1 = r.f32();
    c.t0 = r.f32(), c.t1 = r.f32();
    c.max_segments = r.u32();
    c.width0 = r.f32(), c.width1 = r.f32();
    c.uv_length = r.f32(), c.segment_length = r.f32();
    c.frame0 = r.f32(), c.frame1 = r.f32();
    c.material = resource(r);
    if (!c.max_segments || !(c.segment_length > 0) || !(c.uv_length > 0)) throw FormatError("FXID beam: bad segmenting");
    return c;
}

FxBurstCmd burst(Reader& r, std::uint32_t op) {
    FxBurstCmd c;
    c.align = r.u32();
    if (op == 7 ? c.align > 2 : (c.align != 0x10 && c.align != 0x20))
        throw FormatError("FXID op " + std::to_string(op) + ": mode " + std::to_string(c.align));
    c.alpha0 = r.f32(), c.alpha1 = r.f32(), c.alpha_exp = r.f32();
    c.t0 = r.f32(), c.t1 = r.f32(), c.life = r.f32(), c.frame_rate = r.f32();
    c.emissions = r.u32(), c.per_emission = r.u32();
    if (!c.emissions || !c.per_emission || c.emissions * c.per_emission > 4096) throw FormatError("FXID burst: bad counts");
    c.pos0 = vec3(r), c.pos1 = vec3(r), c.pos_rand = vec3(r), c.pos_exp = f3(r);
    c.size0 = vec3(r), c.size1 = vec3(r), c.size_rand = vec3(r), c.size_exp = f3(r);
    c.gravity = r.f32();
    c.material = resource(r);
    return c;
}

FxTrailCmd trail(Reader& r) {
    FxTrailCmd c;
    c.alpha0 = r.f32(), c.alpha1 = r.f32(), c.alpha_exp = r.f32();
    c.t0 = r.f32(), c.t1 = r.f32();
    c.interval0 = r.f32(), c.interval1 = r.f32();
    c.frame_exp = r.f32();
    c.count = r.u32();
    if (!c.count || c.count > 4096) throw FormatError("FXID trail: bad ring size");
    c.drift0 = vec3(r), c.drift1 = vec3(r), c.drift_rand = vec3(r), c.drift_exp = f3(r);
    c.pos0 = vec3(r), c.pos1 = vec3(r), c.pos_rand = vec3(r), c.pos_exp = f3(r);
    c.size0 = vec3(r), c.size1 = vec3(r), c.size_rand = vec3(r), c.size_exp = f3(r);
    c.material = resource(r);
    return c;
}

bool printable(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c >= 0x7f) return false;
    return true;
}

} // namespace

std::size_t fx_command_size(std::uint32_t op) {
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

FxEffect decode_fx(std::span<const std::uint8_t> data) {
    Reader r(data);
    FxEffect fx;
    FxHeader& h = fx.header;
    h.command_count = r.u32();
    h.time_mode = r.u32();
    if (h.time_mode > 17) throw FormatError("FXID: time mode " + std::to_string(h.time_mode));
    h.duration = r.f32(), h.t_jitter = r.f32();
    h.flags = r.u32(), h.settings_key = r.u32();
    h.pos_jitter = vec3(r), h.probe_point = vec3(r), h.scale = vec3(r);
    fx.commands = h.command_count;
    r.seek(kHeaderSize);
    for (std::size_t i = 0; i < fx.commands; ++i) {
        const std::size_t start = r.pos();
        const std::uint32_t word = r.u32(), op = word & 0xff;
        const std::size_t size = fx_command_size(op);
        if (!size) throw FormatError("FXID: unknown opcode " + std::to_string(op) + " at " + std::to_string(start));
        if (word & ~0x1ffu) throw FormatError("FXID: command word " + std::to_string(word) + " at " + std::to_string(start));
        FxCommand c;
        c.opcode = op;
        c.tested = word & 0x100;
        switch (op) {
        case 1: c.data = light(r); break;
        case 2: c.data = sound(r), fx.sounds.push_back(std::get<FxSound>(c.data)); break;
        case 3: case 4: case 9: c.data = sprite(r, op); break;
        case 5: c.data = beam(r); break;
        case 7: case 10: c.data = burst(r, op); break;
        case 8: c.data = trail(r); break;
        }
        if (r.pos() > start + size) throw FormatError("FXID: opcode " + std::to_string(op) + " overruns its record");
        r.seek(start + size); // throws when the record runs past the entry
        fx.list.push_back(std::move(c));
    }
    if (!r.at_end()) throw FormatError("FXID: " + std::to_string(r.remaining()) + " bytes after the last command");
    return fx;
}

std::vector<FxSound> load_fx_sounds(AssetLibrary& lib, std::string_view name) {
    const NResEntry* e = lib.find("effects.rlb", name);
    return e ? decode_fx(e->data).sounds : std::vector<FxSound>{};
}

ExplosionDef decode_expl(std::span<const std::uint8_t> data) {
    if (data.size() != 792) throw FormatError("EXPL: size " + std::to_string(data.size()) + ", expected 792");
    Reader r(data);
    ExplosionDef x;
    x.kind = r.u32();
    if (x.kind < 1 || x.kind > 4) throw FormatError("EXPL: kind " + std::to_string(x.kind));
    x.damage = r.f32(), x.radius = r.f32();
    x.c = r.f32(), x.d = r.f32();
    x.orient = r.u32();
    // fx[0] is empty in ba_b_04_part / ba_b_05_part (surface slots only).
    for (std::size_t k = 0; k < x.fx.size(); ++k) {
        x.fx[k].first = fixed_str(r, 32);
        x.fx[k].second = fixed_str(r, 32);
        x.fx_valid[k] = printable(x.fx[k].first) && printable(x.fx[k].second);
    }
    if (!r.at_end()) throw FormatError("EXPL: trailing bytes");
    return x;
}

} // namespace parkan
