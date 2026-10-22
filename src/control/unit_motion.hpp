// One unit's motion runtime: CControlBase's motion queue and clock (Update 1000bcf0,
// SelectNextMotion 100051c0, QueueMotion 10004f50, AdvanceMotion 10005370, Interpolate 100059a0)
// driving the kinematic mover CMover (BeginStep 100145b0, ClampVelocity 10014610, StepRotation
// 10014780, StepLinear 100153c0, ApplyRootMotion 10015990, IntegrateVelocity 10015920, Interpolate
// 10015a50). Spec: notes/motion-runtime.md, notes/control-physics.md "Mover and collision".
//
// Not modelled: scripted path following, ground following / slope limit / levelling (another
// module adds those through apply_offset), collision, devices, damage, the owner sub-object ticks.
// The unit is class 4 (BTLU): no bullet exemption from the 5 m cap, Interpolate after every Update.
#pragma once

#include "control/motion_graph.hpp"

#include <deque>
#include <functional>

namespace parkan {

// What a frame renders: the output matrix (ctl+0x2e4, parent frame) and the AniMesh sampling state.
// Pose every node with sample_node_blend(mesh, node, frame_a, frame_b, weight_b) and strip node 0's
// translation (it reaches the screen through `matrix`).
struct MotionFrame {
    Mat4 matrix = Mat4::identity();
    float frame_a = 0, frame_b = 0, weight_b = 0;
    // The attitude offset as the render pose has it: m+0x17c − (1 − u)·m+0x188 (z = strafe heading);
    // the turret counter-rotates its yaw by it (CTurretDevice::SetYawOffsetFromAttitude 10028990).
    Vec3 attitude_offset{};
};

class UnitMotion {
public:
    // `mesh` must outlive this object (root motion is sampled from it every frame).
    UnitMotion(const Ctld& ctld, const Mesh& mesh, std::span<const ContactPoint> points = {});

    // IControl requests.
    void set_linear_velocity(Vec3 v);     // body-local m/s; velocity mode (m+0x1a8 = 1)
    void set_throttle(Vec3 t);            // fraction of body[6..8] per axis; throttle mode (ramped)
    void set_angular_command(Vec3 a);     // fraction of body[9..11] per axis; leaves attitude-target mode
    void set_attitude_target(Vec3 turns); // remaining turn per axis as a fraction of 2π, offset by 0.5
                                          // (0.5 = on target); StepRotation then writes the command
    // IControl +0x50 SetAttitudeOffset (m+0x40): StepRotation adds its change since the last step
    // after the rate clamp, so a strafe yaws the chassis by the whole offset in one step.
    void set_attitude_offset(Vec3 o) { att_offset_ = o; }
    Vec3 attitude_target() const { return attitude_; } // m+0x2c (IControl GetProperty 0x30)
    void set_mode(std::int32_t mode) { mode_ = mode; } // IControl slot 19 SetMode (ctl+0x1ac)
    void set_contact_touch(std::size_t c, bool touch) { contacts_.at(c).touch = touch; }

    // The inactive-Update stamp: clocks := now, pose := `pose`; the unit is in state 0 with an empty
    // queue (the spawn state is not traced; motion-runtime.md "Open questions").
    void start(float now_ms, const Mat4& pose = Mat4::identity());
    // CControlBase::Update: plays whole motion states until the motion clock passes `now`, then
    // interpolates the render pose. Calls less than 1 ms apart are ignored (and not stamped).
    void update(float now_ms);
    // CControlBase::Interpolate: the render matrix and the animation sample at `now`.
    MotionFrame interpolate(float now_ms);
    // Ground / fall / collision offsets: added to the start, end and output matrices alike.
    void apply_offset(Vec3 d);

    // Runtime body (ctl+0x470). RecomputePerformance is not modelled: it starts as the file body
    // (so acceleration is not doubled and max speed / turn rate are not scaled by engines and HP);
    // the owner may overwrite it.
    std::array<float, 27> body{};
    // The pilot (ctl+0x2c IWizard::Takt, unit-ai-runtime.md §0): Update calls it once per motion step
    // with the motion clock, just before SelectNextMotion, so its IControl requests pick the next state.
    std::function<void(UnitMotion&, float clock_ms)> pilot;
    // RunMotionEvents(list) for the entry event list of each state as it is popped (list >= 0).
    std::function<void(std::int32_t list)> on_enter_events;
    // Left / right drive health (m+0x1a0 / +0x1a4): their difference makes the yaw drift.
    float drive_left = 1, drive_right = 1;
    // The jitter LFSR (Control 10042230, BSS); its seeding is not traced.
    std::uint32_t jitter_lfsr = 0;

    const MotionGraph& graph() const { return graph_; }
    const std::vector<ContactRecord>& contacts() const { return contacts_; }
    const std::deque<std::uint32_t>& queue() const { return queue_; }
    std::uint32_t current_state() const { return cur_; } // ctl+0xfc
    const MotionState& live() const { return live_; }    // ctl+0x100
    float step_start() const { return step_start_; }     // ctl+0xd8
    float clock() const { return clock_; }               // ctl+0xdc: end of the step in flight
    float step_duration() const { return step_dur_; }    // ctl+0xf0
    float weight() const { return w_; }                  // ctl+0xf8: track-B weight of this step
    float prev_weight() const { return prev_w_; }        // ctl+0xf4
    Vec3 velocity() const { return v_; }                 // m+0x14, body-local
    Vec3 angular_command() const { return ang_cmd_; }    // m+0x20
    Vec3 world_velocity() const { return world_vel_; }   // m+0x4c, last step
    Quat step_rotation() const { return q_step_; }       // m+0x58
    const Mat4& end_matrix() const { return end_; }      // m+0xb0
    const Mat4& start_matrix() const { return start_; }  // m+0xf0
    const Mat4& output_matrix() const { return out_; }   // m+0x130

private:
    // CAniMesh's stored range (+0x1e4..+0x1f0), time (+0x1f4) and track-B weight (+0x1f8).
    struct AnimState {
        std::array<float, 2> a{}, b{};
        float time = 0, weight = 1;
        // SetAnimBlend (AniMesh 10005890) returns at once when the time is unchanged: the weight is
        // then not stored either.
        void set_blend(float t, float w) {
            if (t == time) return;
            time = t, weight = w;
        }
    };

    bool matches(const CtldState& s) const;
    void select_next_motion();
    void queue_motion(std::uint32_t target, bool clear);
    void advance_motion();
    void step_rotation(float dt, bool unclamped);
    void step_linear(float dt, bool unclamped);

    MotionGraph graph_;
    MotionState default_state_{}; // ctl+0x3ac, used for an index >= n (contents not traced: zeros)
    std::vector<ContactRecord> contacts_;
    const Mesh* mesh_;
    std::array<float, 27> file_body_{}; // m+0x1ac: nominal body (lean sources)
    std::deque<std::uint32_t> queue_;
    std::uint32_t cur_ = 0;
    MotionState live_;
    AnimState anim_;
    bool active_ = false;
    std::int32_t mode_ = 0;
    float last_time_ = 0, step_start_ = 0, clock_ = 0, step_dur_ = 0;
    float w_ = 1, prev_w_ = 1;

    // CMover
    Vec3 throttle_{}, v_{}, ang_cmd_{};
    Vec3 attitude_{0.5f, 0.5f, 0.5f};
    Vec3 level_{}, world_vel_{}, dv_{}, lean_angles_{};
    Vec3 att_offset_{}, att_prev_{}, att_delta_{}; // m+0x40, m+0x17c, m+0x188
    bool velocity_mode_ = true, attitude_mode_ = false;
    Quat q_step_{}, lean_prev_{}, lean_cur_{};
    Mat4 end_ = Mat4::identity(), start_ = Mat4::identity(), out_ = Mat4::identity();
};

} // namespace parkan
