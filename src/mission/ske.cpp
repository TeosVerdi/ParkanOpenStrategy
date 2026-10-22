#include "mission/ske.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace parkan::mission {

namespace {

SkeTime read_time(Reader& r) {
    std::int32_t v[6];
    for (auto& x : v) x = r.i32();
    r.bytes(8);
    return {v[0], v[3], v[4]};
}

std::string read_str(Reader& r) {
    const std::int32_t n = r.i32();
    if (n < 0 || std::size_t(n) > r.remaining()) throw FormatError("ske string runs past the end");
    return r.str(std::size_t(n));
}

SkeLayer read_layer(Reader& r) {
    if (const std::int32_t v = r.i32(); v != 3) throw FormatError("unsupported ske layer version " + std::to_string(v));
    SkeLayer l;
    l.time = read_time(r);
    l.event = r.i32();
    l.unused_2c = r.i32();
    for (auto& c : l.horizon) c = r.u32();
    l.fog_start = r.f32();
    l.fog_end = r.f32();
    for (auto& c : l.mid) c = r.u32();
    for (auto& c : l.high) c = r.u32();
    l.zenith = r.u32();
    l.unused_70 = r.u32(); // disk order: L70 before L6C
    l.sprite = r.u32();
    l.cloud = r.u32();
    l.sun_light = r.u32();
    l.ambient = r.u32();
    l.back_light = r.u32();
    for (auto& s : l.strings) s = read_str(r);
    l.sprite_w = r.f32();
    l.sprite_h = r.f32();
    l.intensity = r.f32();
    l.density = r.f32();
    const std::int32_t n = r.i32();
    if (n < 0 || std::size_t(n) * 4 > r.remaining()) throw FormatError("ske list count exceeds the file");
    l.list.resize(std::size_t(n));
    for (auto& s : l.list) s = read_str(r);
    return l;
}

Vec3 rgb(std::uint32_t argb) {
    return {float((argb >> 16) & 0xFF) / 255.f, float((argb >> 8) & 0xFF) / 255.f, float(argb & 0xFF) / 255.f};
}

// Rotation of v by `angle` about the unit `axis` (Rodrigues, StartSunPath 1004a550).
Vec3 rotate(Vec3 v, Vec3 axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

} // namespace

Ske decode_ske(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.i32() != -1) throw FormatError("old version ske file");
    Ske s;
    s.version = r.i32();
    if (s.version != 5) throw FormatError("unsupported ske version " + std::to_string(s.version));
    const std::int32_t keys = r.i32();
    if (keys < 0 || keys > 64) throw FormatError("bad ske key count");
    s.keys.resize(std::size_t(keys));
    for (auto& k : s.keys) {
        k.present = r.i32() == 1;
        if (!k.present) continue;
        const std::int32_t layers = r.i32();
        if (layers < 0 || layers > 1000) throw FormatError("bad ske layer count");
        k.time_a = read_time(r);
        k.period = read_time(r);
        k.layers.resize(std::size_t(layers));
        for (auto& l : k.layers) l = read_layer(r);
    }
    s.start = read_time(r);
    s.tail_a = r.i32();
    s.tail_b = r.i32();
    if (!r.at_end()) throw FormatError("trailing bytes after the ske file");
    return s;
}

std::uint32_t lerp_argb(std::uint32_t a, std::uint32_t b, float f) {
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
        out |= std::uint32_t(std::lround(x + (y - x) * f)) << shift;
    }
    return out;
}

AtmosphereState evaluate_ske(const Ske& ske, double seconds) {
    // A layer at virtual hh:mm sits period·(hh·60+mm)·60/86400 s into its key; keys follow each
    // other cyclically (FindPrevSkeLayer 10040530 / FindNextSkeLayer 10040600).
    struct Point {
        double t;
        const SkeLayer* layer;
    };
    std::vector<Point> timeline;
    std::vector<double> key_base;
    double total = 0;
    for (const auto& k : ske.keys) {
        key_base.push_back(total);
        for (const auto& l : k.layers) timeline.push_back({total + k.period_seconds() * l.time.total_minutes() / 1440.0, &l});
        total += k.period_seconds();
    }
    AtmosphereState a;
    if (timeline.empty()) return a;
    std::stable_sort(timeline.begin(), timeline.end(), [](const Point& x, const Point& y) { return x.t < y.t; });

    double now = 0;
    if (total > 0) {
        const auto sk = std::size_t(std::clamp(ske.start.key, 0, int(ske.keys.size()) - 1));
        const double start = key_base[sk] + ske.keys[sk].period_seconds() * ske.start.total_minutes() / 1440.0;
        now = std::fmod(start + seconds, total);
        if (now < 0) now += total;
    }
    a.day_seconds = now;

    // Previous and next layer around now, cyclically.
    auto prev_it = std::upper_bound(timeline.begin(), timeline.end(), now, [](double v, const Point& p) { return v < p.t; });
    const Point prev = prev_it == timeline.begin() ? Point{timeline.back().t - total, timeline.back().layer} : *(prev_it - 1);
    const Point next = prev_it == timeline.end() ? Point{timeline.front().t + total, timeline.front().layer} : *prev_it;
    const float f = next.t > prev.t ? float((now - prev.t) / (next.t - prev.t)) : 0.f;
    const SkeLayer &p = *prev.layer, &n = *next.layer;
    auto lerpf = [f](float x, float y) { return x + (y - x) * f; };
    auto argb = [f](std::uint32_t x, std::uint32_t y) { return lerp_argb(x, y, f); };

    // CSky parameters: quadrant order {L34, L38, L30, L3C} etc. (CAtmData_GetObjectParams).
    static constexpr int kLow[4] = {1, 2, 0, 3}, kMid[4] = {0, 3, 1, 2}, kHigh[4] = {0, 3, 1, 2};
    for (int q = 0; q < 4; ++q) {
        a.low[q] = argb(p.horizon[kLow[q]], n.horizon[kLow[q]]);
        a.mid[q] = argb(p.mid[kMid[q]], n.mid[kMid[q]]);
        a.high[q] = argb(p.high[kHigh[q]], n.high[kHigh[q]]);
    }
    a.zenith = argb(p.zenith, n.zenith);
    a.cloud = rgb(argb(p.cloud, n.cloud));
    a.ambient = rgb(argb(p.ambient, n.ambient));
    a.fog_start = lerpf(p.fog_start, n.fog_start) * 700.f;
    a.fog_end = lerpf(p.fog_end, n.fog_end) * 700.f;
    a.light_color = rgb(argb(p.sun_light, n.sun_light)) * lerpf(p.intensity, n.intensity);
    a.back_color = rgb(argb(p.back_light, n.back_light));
    a.sprite = argb(p.sprite, n.sprite);

    // Sun or moon: alive from an event-0 layer to the next event-1 layer. Events are scanned over
    // the previous, current and next cycle so a body that rose before the wrap is still found.
    const SkeLayer* on = nullptr;
    double on_t = -1e300;
    for (double base : {-total, 0.0})
        for (const auto& pt : timeline)
            if ((pt.layer->event == 0 || pt.layer->event == 1) && pt.t + base <= now && pt.t + base >= on_t) {
                on_t = pt.t + base;
                on = pt.layer->event == 0 ? pt.layer : nullptr;
            }
    if (on) {
        double off_t = on_t + total;
        for (double base : {-total, 0.0, total})
            for (const auto& pt : timeline)
                if (pt.layer->event == 1 && pt.t + base > on_t && pt.t + base < off_t) off_t = pt.t + base;
        const double life = off_t - on_t;
        const float t = float(life > 0 ? std::min(1.0, (now - on_t) / life) : 0.0);
        a.sun = true;
        a.is_moon = on->strings[0] != "sun";
        const float pi = std::numbers::pi_v<float>;
        const float a1 = a.is_moon ? 0.f : pi / 2, a2 = (a.is_moon ? 50.f : 30.f) * pi / 180.f;
        const float theta = (1.2f * t - 0.1f) * pi; // UpdateSunDirection 1004b570
        a.light_dir = rotate({std::cos(theta), 0, -std::sin(theta)}, {std::cos(a1), std::sin(a1), 0}, a2);
    }
    return a;
}

} // namespace parkan::mission
