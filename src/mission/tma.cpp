#include "mission/tma.hpp"

namespace parkan::mission {

namespace {

std::string lp_string(Reader& r) {
    const std::uint32_t n = r.u32();
    if (n > r.remaining()) throw FormatError("TMA string runs past the end");
    std::string s = r.str(n);
    if (auto z = s.find('\0'); z != std::string::npos) s.resize(z); // the NUL is optional
    return s;
}

void expect_version(Reader& r, std::uint32_t want, const char* what) {
    if (const std::uint32_t v = r.u32(); v != want)
        throw FormatError(std::string("unexpected TMA ") + what + " version " + std::to_string(v));
}

std::uint32_t count(Reader& r, std::size_t min_record) {
    const std::uint32_t n = r.u32();
    if (min_record && std::size_t(n) * min_record > r.remaining()) throw FormatError("TMA count exceeds file size");
    return n;
}

} // namespace

Tma decode_tma(std::span<const std::uint8_t> data) {
    Reader r(data);
    Tma t;
    expect_version(r, 1, "file");

    t.paths.resize(count(r, 8));
    for (auto& p : t.paths) {
        p.id = r.i32();
        p.points.resize(count(r, 12));
        for (auto& pt : p.points) pt = {r.f32(), r.f32(), r.f32()};
    }

    expect_version(r, 6, "clan section");
    t.clans.resize(count(r, 16));
    for (auto& c : t.clans) {
        c.name = lp_string(r);
        c.raw_id = r.i32();
        c.anchor_x = r.f32();
        c.anchor_y = r.f32();
        c.mode = r.u32();
        if (c.mode == 0) {
            c.script = lp_string(r);
            for (std::uint32_t g = count(r, 4); g > 0; --g)
                for (std::uint32_t k = count(r, 20); k > 0; --k)
                    c.spatial.push_back({r.f32(), r.f32(), r.f32(), r.f32(), r.f32()});
            c.research = lp_string(r);
            c.research_tag = r.i32();
        } else {
            c.script = lp_string(r);
            c.script_tag = r.i32();
            c.research = lp_string(r);
            c.research_tag = r.i32();
        }
        c.relations.resize(count(r, 8));
        for (auto& rel : c.relations) {
            rel.clan = lp_string(r);
            rel.value = r.i32();
        }
    }

    expect_version(r, 10, "object section");
    t.objects.resize(count(r, 60));
    for (auto& o : t.objects) {
        o.kind = r.u32();
        o.class_flags = r.u32();
        o.resource = lp_string(r);
        o.ai_index = r.u32();
        o.identity = r.u32();
        for (auto* v : {&o.position, &o.orientation, &o.scale}) *v = {r.f32(), r.f32(), r.f32()};
        o.instance_name = lp_string(r);
        o.raw_after_name = r.u32();
        o.link0 = r.i32();
        o.link1 = r.i32();
        expect_version(r, 1, "property schema");
        o.properties.resize(count(r, 20));
        for (auto& p : o.properties) {
            for (auto& w : p.raw) w = r.u32();
            p.name = lp_string(r);
        }
    }

    t.land_path = lp_string(r);
    t.mission_flag = r.u32();
    t.description = lp_string(r);
    expect_version(r, 1, "extra section");
    t.extras.resize(count(r, 28));
    for (auto& e : t.extras) {
        e.position = {r.f32(), r.f32(), r.f32()};
        for (auto& w : e.raw) w = r.u32();
    }
    if (!r.at_end()) throw FormatError("trailing bytes after the TMA extras");
    return t;
}

} // namespace parkan::mission
