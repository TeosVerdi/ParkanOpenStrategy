#include "shell/briefing.hpp"

#include "core/bytes.hpp"
#include "game/mission_runtime.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace parkan::shell {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

// The value types of the original's cfg reader (property +0x14): 1 int, 2 float, 3 bool, 4 string. A quoted
// value is a string; true/false a bool; a number with '.' or an exponent a float; else an int.
enum class Type { Int = 1, Float = 2, Bool = 3, String = 4 };

struct Value {
    Type type;
    std::string text;
};

// [inferred] The game's cfg reader types a value by its spelling; parse_cfg drops the quotes, so the
// raw line is re-read here to tell "1" from 1.
std::map<std::string, Value> typed_entries(std::string_view block) {
    std::map<std::string, Value> out;
    while (!block.empty()) {
        const auto nl = block.find('\n');
        std::string_view line = block.substr(0, nl);
        block = nl == std::string_view::npos ? std::string_view() : block.substr(nl + 1);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        auto trim = [](std::string_view s) {
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
            return s;
        };
        const std::string key(trim(line.substr(0, eq)));
        std::string_view v = trim(line.substr(eq + 1));
        if (const auto sc = v.find(';'); sc != std::string_view::npos && (v.empty() || v.front() != '"')) v = trim(v.substr(0, sc));
        Value val;
        if (!v.empty() && v.front() == '"') {
            const auto close = v.find('"', 1);
            val = {Type::String, std::string(v.substr(1, close == std::string_view::npos ? v.size() - 1 : close - 1))};
        } else if (v == "true" || v == "false") {
            val = {Type::Bool, std::string(v)};
        } else if (v.find_first_of(".eE") != std::string_view::npos) {
            val = {Type::Float, std::string(v)};
        } else {
            val = {Type::Int, std::string(v)};
        }
        out[key] = std::move(val);
    }
    return out;
}

} // namespace

std::vector<BriefingWaypoint> parse_briefing_cfg(std::string_view text) {
    // Split into `object … end` blocks by hand to keep the raw value spelling.
    std::vector<BriefingWaypoint> out;
    std::size_t pos = 0;
    while (true) {
        std::size_t start = std::string_view::npos;
        // Find the next line starting with "object".
        while (pos < text.size()) {
            const auto nl = text.find('\n', pos);
            std::string_view line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
            const std::size_t lstart = pos;
            pos = nl == std::string_view::npos ? text.size() : nl + 1;
            while (!line.empty() && std::isspace((unsigned char)line.front())) line.remove_prefix(1);
            if (lower(line.substr(0, 6)) == "object") {
                start = lstart;
                break;
            }
        }
        if (start == std::string_view::npos) break;
        std::size_t end = pos;
        while (end < text.size()) {
            const auto nl = text.find('\n', end);
            std::string_view line = text.substr(end, (nl == std::string_view::npos ? text.size() : nl) - end);
            const std::size_t lstart = end;
            end = nl == std::string_view::npos ? text.size() : nl + 1;
            while (!line.empty() && std::isspace((unsigned char)line.back())) line.remove_suffix(1);
            while (!line.empty() && std::isspace((unsigned char)line.front())) line.remove_prefix(1);
            if (lower(line) == "end") {
                end = lstart;
                break;
            }
        }
        const auto e = typed_entries(text.substr(pos, end - pos));
        pos = end;
        auto get = [&](const char* key, Type t) -> const std::string& {
            auto it = e.find(key);
            if (it == e.end() || it->second.type != t) throw FormatError(std::string("Invalid property ") + key);
            return it->second.text;
        };
        auto f = [&](const char* k) { return float(std::atof(get(k, Type::Float).c_str())); };
        auto i = [&](const char* k) { return std::atoi(get(k, Type::Int).c_str()); };
        auto b = [&](const char* k) { return get(k, Type::Bool) == "true"; };
        BriefingWaypoint w;
        w.camera = {f("CameraX"), f("CameraY"), f("CameraZ")};
        w.target = {f("TargetX"), f("TargetY"), f("TargetZ")};
        const std::string edge = get("EdgeType", Type::String), wait = get("WaitType", Type::String);
        w.edge_time = f("EdgeTime");
        w.waypoint_time = f("WaypointTime");
        w.wait_for_text = b("WaitForText");
        w.wait_for_sound = b("WaitForSound");
        w.wait_for_time = b("WaitForTime");
        w.wait_for_click = b("WaitForClick");
        w.text_id = get("TextResID", Type::String);
        w.sound_id = get("SoundResID", Type::String);
        w.noise = float(i("NoisePercent")) * 0.01f;
        w.fade = float(i("FadePercent")) * 0.01f;
        w.zoom = b("ZoomOn") ? 1.f : 0.f;
        w.night_vision = b("NightVisionOn");
        const float rotate_time = f("RotateTime");
        w.loop_index = i("LoopIndex");
        w.fade_time = f("FadeTime");
        w.zoom_time = f("ZoomTime");
        w.rotate_speed = rotate_time == 0 ? 0.f : 6.2831855f / rotate_time;
        // The type names compare case-sensitively.
        if (edge == "spline") {
            if (w.edge_time <= 0) throw FormatError("Invalid move type spline (EdgeTime <= 0)");
            w.edge = BriefingWaypoint::Edge::Spline;
        } else if (edge == "jump") {
            w.edge = BriefingWaypoint::Edge::Jump;
        } else if (edge == "linear") {
            w.edge = BriefingWaypoint::Edge::Linear;
        } else {
            throw FormatError("Invalid move type " + edge);
        }
        if (wait == "stay") {
            w.wait = BriefingWaypoint::Wait::Stay;
        } else if (wait == "flyaround" && w.rotate_speed != 0) {
            w.wait = BriefingWaypoint::Wait::FlyAround;
        } else if (wait == "flyby" && w.rotate_speed > 0) {
            w.wait = BriefingWaypoint::Wait::FlyAround;
        } else if (wait == "continuous") {
            w.wait = BriefingWaypoint::Wait::Continuous;
        } else {
            throw FormatError("Invalid wait type " + wait);
        }
        out.push_back(std::move(w));
    }
    return out;
}

void BriefingPath::Hermite::setup(Vec3 a0, Vec3 a1, Vec3 v0, Vec3 v1, float t) {
    p0 = a0, p1 = a1, T = t, tv0 = v0 * t, tv1 = v1 * t;
    a = p0, b = tv0;
    c = (p1 - tv0 - p0) * 3.f - (tv1 - tv0);
    d = (tv1 - tv0) - ((p1 - tv0 - p0) * 2.f);
}

Vec3 BriefingPath::Hermite::at(float t) const {
    if (!(t > 0)) return p0;
    if (t >= T) return p1;
    const float s = t / T, s2 = s * s, s3 = s2 * s;
    return d * s3 + c * s2 + b * s + a;
}

Vec3 BriefingPath::Hermite::velocity(float t) const {
    if (!(t > 0)) return tv0; // the original returns the scaled tangents at the ends
    if (t >= T) return tv1;
    const float k = 1.f / T, s = t * k;
    return d * (k * s * s * 3.f) + c * (k * s + k * s) + b * k;
}

void BriefingPath::look(Vec3 eye, Vec3 target) {
    // Start 10030420, FUN_100305d0, FUN_1002f8d0: column 0 = normalize(target − eye), column 1 =
    // normalize((0,0,1) × column 0), column 2 = normalize(column 0 × column 1), column 3 = eye.
    const Vec3 f = normalize(target - eye);
    const Vec3 left = normalize(cross({0, 0, 1}, f));
    cam_.eye = eye, cam_.forward = f, cam_.up = normalize(cross(f, left));
}

int BriefingPath::next_index() const {
    if (!valid(idx_)) return -1;
    const int loop = wps_[std::size_t(idx_)].loop_index;
    if (loop != -1) return loop;
    return std::size_t(idx_) + 1 < wps_.size() ? idx_ + 1 : -1;
}

void BriefingPath::start(std::uint32_t now) {
    if (wps_.empty()) {
        reset();
        return;
    }
    look(wps_[0].camera, wps_[0].target);
    play(now, 0);
}

void BriefingPath::reset() {
    idx_ = -1;
    done_ = true;
    velocity_ = {};
}

void BriefingPath::arrive(std::uint32_t t) {
    // FUN_100307c0.
    const BriefingWaypoint& w = wps_[std::size_t(idx_)];
    start_eye_ = cam_.eye, start_target_ = w.target;
    phase_ = 1;
    t_wait_ = t_fade_ = t;
    const int next = next_index();
    const BriefingWaypoint& n = valid(next) ? wps_[std::size_t(next)] : w;
    zoom_from_ = w.zoom, zoom_to_ = n.zoom;
    fade_from_ = w.fade, fade_to_ = n.fade;
    night_vision_ = w.night_vision;
    if (w.wait == BriefingWaypoint::Wait::FlyAround) {
        // The angle of (eye − target) in the xy plane from +x (acos with the sign of the cross product),
        // the horizontal radius, and the height kept from the arrival.
        const float dx = start_eye_.x - start_target_.x, dy = start_eye_.y - start_target_.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len >= 0.0001f) {
            const float c = dx / len;
            fly_angle_ = c < -1 ? 3.1415927f : c > 1 ? 0.f : std::acos(c) * (dy >= 0 ? 1.f : -1.f);
        } else {
            fly_angle_ = 0;
        }
        fly_radius_ = len;
        fly_height_ = start_eye_.z;
    }
    if (!w.sound_id.empty()) sounds_.push_back(w.sound_id);
}

void BriefingPath::hold(std::uint32_t t) {
    // FUN_100300c0: flyaround circles the waypoint's target at its rotate speed.
    const BriefingWaypoint& w = wps_[std::size_t(idx_)];
    if (w.wait != BriefingWaypoint::Wait::FlyAround) return;
    const float a = secs(t, t_wait_) * w.rotate_speed + fly_angle_;
    const Vec3 eye{std::cos(a) * fly_radius_ + start_target_.x, std::sin(a) * fly_radius_ + start_target_.y, fly_height_};
    velocity_ = {-std::sin(a) * w.rotate_speed * fly_radius_, std::cos(a) * w.rotate_speed * fly_radius_, 0};
    look(eye, start_target_);
}

void BriefingPath::begin_edge(std::uint32_t t) {
    // FUN_10030aa0.
    const BriefingWaypoint& w = wps_[std::size_t(idx_)];
    start_eye_ = cam_.eye, start_target_ = w.target;
    t_edge_ = t;
    phase_ = 0;
    const int next = next_index();
    if (valid(next)) {
        next_eye_ = wps_[std::size_t(next)].camera, next_target_ = wps_[std::size_t(next)].target;
    } else {
        next_eye_ = start_eye_, next_target_ = start_target_;
    }
    if (w.edge != BriefingWaypoint::Edge::Spline) return;
    // FUN_10030c10: the end tangent.
    Vec3 v1{};
    if (valid(next)) {
        const BriefingWaypoint& n = wps_[std::size_t(next)];
        if (n.wait == BriefingWaypoint::Wait::Continuous) {
            if (n.edge == BriefingWaypoint::Edge::Spline) {
                const Vec3 d1 = next_eye_ - start_eye_, d2 = n.camera - next_eye_;
                const float speed = (std::sqrt(dot(d2, d2)) + std::sqrt(dot(d1, d1))) / (n.edge_time + w.edge_time);
                const Vec3 u2 = dot(d2, d2) > 0 ? normalize(d2) : Vec3{}, u1 = dot(d1, d1) > 0 ? normalize(d1) : Vec3{};
                const Vec3 sum = u1 + u2;
                if (dot(sum, sum) > 0) v1 = normalize(sum) * speed;
            } else if (n.edge == BriefingWaypoint::Edge::Linear && n.edge_time > 0) {
                v1 = (n.camera - next_eye_) * (1.f / n.edge_time);
            }
        }
        if (n.wait == BriefingWaypoint::Wait::FlyAround) {
            const float w_ = n.rotate_speed;
            v1 = {-w_ * (n.camera.y - n.target.y), w_ * (n.camera.x - n.target.x), 0};
        }
    }
    eye_spline_.setup(start_eye_, next_eye_, velocity_, v1, w.edge_time);
    target_spline_.setup(start_target_, next_target_, {}, {}, w.edge_time);
}

void BriefingPath::travel(std::uint32_t t) {
    // FUN_1002f8d0.
    const BriefingWaypoint& w = wps_[std::size_t(idx_)];
    const float s = secs(t, t_edge_), T = w.edge_time;
    const bool over = T <= 0 || s >= T;
    switch (w.edge) {
    case BriefingWaypoint::Edge::Linear: {
        const float k = over ? 1.f : s / T;
        const Vec3 eye = start_eye_ * (1 - k) + next_eye_ * k, tgt = start_target_ * (1 - k) + next_target_ * k;
        if (T <= 0) {
            velocity_ = {};
        } else {
            const Vec3 d = next_eye_ - start_eye_;
            const float v = std::sqrt(dot(d, d)) / T;
            velocity_ = {v, v, v};
        }
        look(over ? next_eye_ : eye, over ? next_target_ : tgt);
        break;
    }
    case BriefingWaypoint::Edge::Spline: {
        const Vec3 eye = over ? next_eye_ : eye_spline_.at(s), tgt = over ? next_target_ : target_spline_.at(s);
        velocity_ = eye_spline_.velocity(s);
        look(eye, tgt);
        break;
    }
    case BriefingWaypoint::Edge::Jump: {
        float f;
        if (over) {
            look(next_eye_, next_target_);
            f = fade_to_;
        } else if (const float half = T * 0.5f; s >= half) {
            f = (fade_to_ - 1.f) * (s - half) / half + 1.f;
            look(next_eye_, next_target_);
        } else {
            f = (1.f - fade_from_) * s / half + fade_from_;
            look(start_eye_, start_target_);
        }
        fade_ = f;
        velocity_ = {};
        break;
    }
    }
}

void BriefingPath::zoom_fade(std::uint32_t t) {
    // FUN_10030320: zoom over ZoomTime and fade over FadeTime, both from the arrival.
    const BriefingWaypoint& w = wps_[std::size_t(idx_)];
    const float s = secs(t, t_fade_);
    zoom_ = (w.zoom_time <= s || w.zoom_time <= 0) ? zoom_to_ : s / w.zoom_time * (zoom_to_ - zoom_from_) + zoom_from_;
    if (w.edge == BriefingWaypoint::Edge::Jump) fade_ = fade_from_;
    else if (s < w.fade_time && w.fade_time > 0) fade_ = s / w.fade_time * (fade_to_ - fade_from_) + fade_from_;
    else fade_ = fade_to_;
}

bool BriefingPath::busy(std::uint32_t t) {
    for (;;) {
        zoom_fade(t);
        if (phase_ == 0) break;
        if (phase_ != 1) return false;
        hold(t);
        const BriefingWaypoint& w = wps_[std::size_t(idx_)];
        if (w.wait_for_time && secs(t, t_wait_) < w.waypoint_time) return true;
        begin_edge(t);
    }
    travel(t);
    if (secs(t, t_edge_) < wps_[std::size_t(idx_)].edge_time) return true;
    phase_ = 2;
    return false;
}

bool BriefingPath::play(std::uint32_t t, int index) {
    if (valid(index)) {
        idx_ = index;
        arrive(t);
        if (busy(t)) {
            done_ = false;
            return true;
        }
    }
    reset();
    return false;
}

bool BriefingPath::update(std::uint32_t t) {
    if (!valid(idx_)) return false;
    if (busy(t)) return false;
    return play(t, next_index());
}

BriefingSetup parse_briefing_setup(std::string_view mission_cfg) {
    BriefingSetup s;
    for (const auto& o : game::parse_cfg(mission_cfg)) {
        const std::string n = lower(o.name);
        if (n == "briefing") {
            if (const std::string* f = o.get("filename")) s.filename = *f;
            if (const std::string* t = o.get("cd_track")) s.cd_track = std::atoi(t->c_str());
        } else if (n == "mission") {
            if (const std::string* ob = o.get("only_briefing")) s.only_briefing = lower(*ob) == "true";
        } else if (n == "briefing_sounds") {
            for (const auto& [k, v] : o.entries) {
                const std::string key = lower(k);
                if (key != "desc" && key != "library" && key != "libtype" && key != "type") s.sounds[k] = v;
            }
        }
    }
    return s;
}

} // namespace parkan::shell
