// The mission briefing: the camera fly-through a mission plays before control passes to the player
// (iron3d CBriefing 10030fd0 / CBriefingPath, view mode 5 BRIEFING). Its data is the mission's
// briefing.cfg plus mission.cfg's `briefing`, `briefing_sounds` and `mission` objects.
// Spec: notes/shell.md "Briefing", notes/ui-shell.md "Briefing (camera fly-through)".
#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parkan::shell {

// One `object WayPointN` (CBriefingPath::ParseWaypoint 1002d340; 0x74 bytes in the original).
struct BriefingWaypoint {
    enum class Edge { Linear = 0, Spline = 1, Jump = 2 };              // EdgeType "linear" / "spline" / "jump"
    enum class Wait { Stay = 0, FlyAround = 1, Continuous = 3 };       // WaitType "stay" / "flyaround" | "flyby" / "continuous"
    Vec3 camera, target;           // CameraX/Y/Z, TargetX/Y/Z (+0x00, +0x0c)
    float waypoint_time = 0;       // +0x18 seconds held at the waypoint (WaitForTime)
    Wait wait = Wait::Stay;        // +0x1c
    float edge_time = 0;           // +0x20 seconds to travel the edge into this waypoint
    Edge edge = Edge::Linear;      // +0x24
    float noise = 0;               // +0x28 NoisePercent / 100
    float fade = 0;                // +0x2c FadePercent / 100
    float zoom = 0;                // +0x30 ZoomOn as 1.0 / 0.0
    bool night_vision = false;     // +0x34
    float fade_time = 0;           // +0x38
    float zoom_time = 0;           // +0x3c
    std::string text_id;           // +0x40 TextResID (DATA/TextRes.cfg name, e.g. "T01_T01")
    bool wait_for_text = false;    // +0x4c
    std::string sound_id;          // +0x50 SoundResID (mission.cfg briefing_sounds key)
    bool wait_for_sound = false;   // +0x5c
    bool wait_for_click = false;   // +0x5d
    bool wait_for_time = false;    // +0x5e
    float rotate_speed = 0;        // +0x6c 2π / RotateTime (rad/s), 0 = RotateTime 0
    int loop_index = -1;           // +0x70
};

// Parses briefing.cfg. Every key must be present with its type (float, int, bool, string), as
// ParseWaypoint requires; otherwise throws FormatError("Invalid property ..."). Also rejected as the
// original does: an unknown EdgeType/WaitType, "spline" with EdgeTime ≤ 0, "flyaround" with RotateTime 0,
// "flyby" with RotateTime ≤ 0. Waypoints are kept in file order.
std::vector<BriefingWaypoint> parse_briefing_cfg(std::string_view text);

// CBriefingPath playback (Play 1002f2d0, Update 1002f680, Reset 1002f3f0 and their helpers): the camera,
// fade, zoom and night-vision state along the waypoints, driven by a millisecond clock (timeGetTime in the
// original; differences are taken as unsigned 32-bit).
//
// Per waypoint i (index +0x44): ARRIVE (FUN_100307c0: voice SoundResID starts, phase 1), HOLD
// WaypointTime when WaitForTime (FUN_100300c0 circles the target for flyaround), then TRAVEL the edge for
// EdgeTime toward the next waypoint (LoopIndex, else i + 1; FUN_10030aa0 / FUN_1002f8d0), then ARRIVE at
// it. The last waypoint still travels its edge (the camera stays put) before the path ends.
// Quirks kept from the original:
// - a linear edge leaves the speed |Δ| / EdgeTime in all three components of the velocity (+0x174), which a
//   following spline edge takes as its start tangent;
// - a spline edge whose next waypoint is `continuous` ends with a tangent along the edge whose length is
//   |Δ| / (EdgeTime_this + EdgeTime_next) (the second distance it adds is always zero);
// - Play() on a waypoint that is neither held nor travelled (no WaitForTime hold and EdgeTime ≤ 0) ends
//   the path (Reset), as the original's fall-through does.
class BriefingPath {
public:
    struct Camera {
        Vec3 eye, forward{1, 0, 0}, up{0, 0, 1}; // matrix columns 3, 0, 2 (World3D: column 0 = forward)
    };

    explicit BriefingPath(std::vector<BriefingWaypoint> waypoints) : wps_(std::move(waypoints)) {}

    // CBriefing::Start 10030420: the camera at waypoint 0 looking at its target; then Play(now, 0).
    void start(std::uint32_t now_ms);
    // Update 1002f680: advances by the clock. False while the current waypoint is busy; the path is over
    // once done() (also after the last waypoint).
    bool update(std::uint32_t now_ms);
    void reset();
    bool done() const { return done_; }

    const Camera& camera() const { return cam_; }
    float fade() const { return fade_; }    // path+0x18 (FUN_10036c40); 1 = black [inferred], 0 = clear
    float zoom() const { return zoom_; }    // FUN_10036e00, 0..1 between ZoomOn states
    bool night_vision() const { return night_vision_; }
    int index() const { return idx_; }
    int phase() const { return phase_; }    // +0x170: 0 travelling, 1 holding, 2 edge done
    const std::vector<BriefingWaypoint>& waypoints() const { return wps_; }
    // The SoundResID keys started since the last call (arrivals), in order.
    std::vector<std::string> take_sounds() { return std::exchange(sounds_, {}); }

private:
    struct Hermite { // FUN_1002ce50 / 1002d090 / 1002d1a0
        Vec3 p0, p1, tv0, tv1, a, b, c, d;
        float T = 0;
        void setup(Vec3 p0, Vec3 p1, Vec3 v0, Vec3 v1, float T);
        Vec3 at(float t) const;
        Vec3 velocity(float t) const;
    };
    bool valid(int i) const { return i >= 0 && std::size_t(i) < wps_.size(); }
    int next_index() const;
    bool play(std::uint32_t t, int index);
    void arrive(std::uint32_t t);
    void hold(std::uint32_t t);
    void begin_edge(std::uint32_t t);
    void travel(std::uint32_t t);
    void zoom_fade(std::uint32_t t);
    bool busy(std::uint32_t t); // IsWaitSatisfied 1002f800
    void look(Vec3 eye, Vec3 target);
    static float secs(std::uint32_t now, std::uint32_t since) { return float(std::uint32_t(now - since)) * 0.001f; }

    std::vector<BriefingWaypoint> wps_;
    Camera cam_;
    int idx_ = -1, phase_ = 0;
    std::uint32_t t_fade_ = 0, t_wait_ = 0, t_edge_ = 0; // +0x48, +0x4c, +0x50
    Vec3 start_eye_, start_target_, next_eye_, next_target_; // +0x54, +0x60, +0x6c, +0x78
    float fade_from_ = 0, fade_to_ = 0, zoom_from_ = 0, zoom_to_ = 0; // +0x84, +0x88, +0x8c, +0x90
    Hermite eye_spline_, target_spline_;                               // +0x94, +0xfc
    float fly_angle_ = 0, fly_radius_ = 0, fly_height_ = 0;            // +0x164, +0x168, +0x16c
    Vec3 velocity_;                                                    // +0x174
    bool done_ = true;                                                 // +0x180
    float fade_ = 0, zoom_ = 0;
    bool night_vision_ = false;
    std::vector<std::string> sounds_;
};

// The briefing-related parts of mission.cfg.
struct BriefingSetup {
    std::string filename;            // object briefing: filename (relative to the mission folder); empty = none
    std::optional<int> cd_track;     // object briefing: cd_track (CD play index)
    bool only_briefing = false;      // object mission: only_briefing
    std::map<std::string, std::string> sounds; // object briefing_sounds: key → voices.lib entry
};
BriefingSetup parse_briefing_setup(std::string_view mission_cfg);

} // namespace parkan::shell
