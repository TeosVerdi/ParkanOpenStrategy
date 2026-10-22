// Unit tests for engine logic that needs no game data, plus an optional corpus run.
// Usage: parkan-tests [<game dir>]
#include "ai/script_vm.hpp"
#include "control/unit_motion.hpp"
#include "core/paths.hpp"
#include "input/scancodes.hpp"
#include "input/man_manager.hpp"
#include "input/translate_table.hpp"
#include "mission/scheme.hpp"
#include "mission/ske.hpp"
#include "res/library.hpp"
#include "scene/model.hpp"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>

using namespace parkan;
using namespace parkan::ai;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

float eval(std::string_view text, std::map<std::string, float> vars = {}) {
    std::vector<std::string> names;
    std::vector<float> values;
    for (auto& [k, v] : vars) {
        names.push_back(k);
        values.push_back(v);
    }
    auto f = compile_formula(text, [&](std::string_view n, std::uint32_t& id) {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == n) {
                id = std::uint32_t(i);
                return true;
            }
        return false;
    });
    float out = NAN;
    f.evaluate([&](std::uint32_t i) { return values[i]; }, out);
    return out;
}

void test_formulas() {
    std::printf("formulas\n");
    CHECK(eval("0.5") == 0.5f);
    CHECK(eval("dTemp3 + 0.5*dTemp3", {{"dTemp3", 2}}) == 3.f);
    CHECK(eval("( df5 - 3 )", {{"df5", 10}}) == 7.f);
    CHECK(eval("8 - 2 - 1") == 5.f); // left-associative
    CHECK(eval("1 / 0") == 0.f);     // division by zero yields 0
    CHECK(eval("-3 + 1") == -2.f);
    CHECK(eval("A(-4)") == 4.f);
    CHECK(eval("N(0)") == 0.5f);
    CHECK(eval("N(5)") == 1.f);
    CHECK(eval("B(0.1)") == 1.f);
    CHECK(eval("S(-2)") == 0.f);
    CHECK(eval("1 & 0") == 0.f);
    CHECK(eval("1 | 0") == 1.f);
    CHECK(eval("!0") == 1.f);
}

Record rec(Op op) {
    Record r;
    r.op = op;
    return r;
}

struct RecordingHost : ScriptHost {
    std::vector<std::uint32_t> calls;
    std::uint32_t result = 0;
    std::uint32_t call_handler(std::uint32_t index, ScriptVM&, const Record&) override {
        calls.push_back(index);
        return result;
    }
};

void test_vm() {
    std::printf("vm\n");
    ScriptProgram p;
    p.vars = {{VarType::Dword, "d0", "0"}, {VarType::Dword, "d1", "1"}, {VarType::Dword, "x", "0"},
              {VarType::Float, "f", "0"}};
    p.formulas = {"d1 + 2.9"};
    Event ev{"Main", 0, {}};
    // if d0 == d1 (false) { h7(); x = 5 } ; x = formula (3.9 → 3) ; f = h8() ; stop ; h9()
    // (x = 5 still runs inside the false IF, then the formula overwrites it.)
    Record if_ = rec(Op::If);
    if_.cmp = Cmp::Equal;
    if_.refs = {0, 1};
    Record call7 = rec(Op::Assign);
    call7.handler = 7;
    Record set5 = rec(Op::SetImmediate);
    set5.dest = 2;
    set5.source = 5;
    Record endif = rec(Op::EndIf);
    Record fml = rec(Op::Assign);
    fml.dest = 2;
    fml.formula = 0;
    Record call8 = rec(Op::Assign);
    call8.handler = 8;
    call8.dest = 3;
    Record stop = rec(Op::Stop);
    Record call9 = rec(Op::Assign);
    call9.handler = 9;
    ev.records = {if_, call7, set5, endif, fml, call8, stop, call9};
    p.events.push_back(ev);

    ScriptVM vm(p);
    RecordingHost host;
    host.result = std::bit_cast<std::uint32_t>(1.5f);
    CHECK(vm.run_event(0, host));
    CHECK(host.calls == std::vector<std::uint32_t>{8}); // h7 skipped (false IF), h9 after STOP
    CHECK(vm.var(2).get_dword() == 3);                  // 3.9 truncated toward zero
    CHECK(vm.var(3).get_float() == 1.5f);               // raw bits into a float variable
}

// SetImmediate (op 6) runs even inside a false IF in the original: ExecRecord only tests the IF
// stack for op -1, GOTO, jump-to-event and STOP (verified in the disassembly at 100121e2).
void test_vm_set_ignores_if() {
    std::printf("vm: op 6 inside a false IF\n");
    ScriptProgram p;
    p.vars = {{VarType::Dword, "d0", "0"}, {VarType::Dword, "d1", "1"}, {VarType::Dword, "x", "0"}};
    Record if_ = rec(Op::If);
    if_.cmp = Cmp::Equal;
    if_.refs = {0, 1};
    Record set5 = rec(Op::SetImmediate);
    set5.dest = 2;
    set5.source = 5;
    p.events.push_back({"Main", 0, {if_, set5, rec(Op::EndIf)}});
    ScriptVM vm(p);
    RecordingHost host;
    vm.run_event(0, host);
    CHECK(vm.var(2).get_dword() == 5);
}

// AniMesh key sampling (FUN_10012880): frame = round(t - 0.5) through the type-19 map, then the
// mapped key and the next one interpolated at t by their own times.
void test_anim_sampling() {
    Mesh m;
    MeshNode n;
    n.anim_map_start = 0;
    n.fallback_key = 3;
    m.nodes = {n};
    m.frame_count = 3;
    m.frame_map = {0, 1, 3}; // frame 2 maps to key 3 = the fallback key: not animated
    auto key = [](float t, float x) {
        MeshKey k;
        k.time = t;
        k.pos = {x, 0, 0};
        k.quat = {0, 0, 0, 1};
        return k;
    };
    m.keys = {key(0, 0), key(1, 10), key(2, 20), key(0, 99)};
    CHECK(sample_node(m, 0, 0.f).pos.x == 0.f);
    CHECK(sample_node(m, 0, 0.25f).pos.x == 2.5f);  // frame 0: keys 0..1
    CHECK(sample_node(m, 0, 1.f).pos.x == 10.f);    // round(0.5) = 0 (ties to even): key 0's next, exactly
    CHECK(sample_node(m, 0, 1.75f).pos.x == 17.5f); // frame 1: keys 1..2
    CHECK(sample_node(m, 0, 2.5f).pos.x == 99.f);   // frame 2 maps to the fallback key
    CHECK(sample_node(m, 0, 3.6f).pos.x == 99.f);   // past the frame count
    CHECK(sample_node(m, 0, -1.f).pos.x == 99.f);   // negative time: static pose
    CHECK(sample_node_blend(m, 0, 0.25f, 1.75f, 0.5f).pos.x == 10.f);
    CHECK(sample_node_blend(m, 0, 0.25f, -1.f, 0.5f).pos.x == 2.5f); // B unusable: A alone

    const float r = std::sqrt(0.5f);
    const Quat a{0, 0, 0, 1}, b{0, 0, r, r}; // 90 degrees about z
    const Quat h = slerp(a, b, 0.5f);         // 45 degrees
    CHECK(std::abs(h.z - std::sin(0.3926991f)) < 1e-6f && std::abs(h.w - std::cos(0.3926991f)) < 1e-6f);
    const Quat h2 = slerp(a, Quat{0, 0, -r, -r}, 0.5f); // same rotation, opposite sign: shortest path
    CHECK(std::abs(h2.z - h.z) < 1e-6f && std::abs(h2.w - h.w) < 1e-6f);
}

// Unit design (.dat) node tree: 0x6c-byte node, child count, children (ArealMap 10016a10).
void test_scheme() {
    Bytes b(8);
    b[0] = 0xF1, b[1] = 0xF0;
    auto node = [&](const char* entry, std::int32_t mount, std::uint32_t children) {
        std::uint8_t rec[0x70] = {};
        std::memcpy(rec, "objects.rlb", 11);
        std::memcpy(rec + 0x20, entry, std::strlen(entry));
        rec[0x40] = 1;
        std::memcpy(rec + 0x44, &mount, 4);
        std::memcpy(rec + 0x6c, &children, 4);
        b.insert(b.end(), rec, rec + sizeof rec);
    };
    node("base", -1, 2);
    node("turret", 1, 1);
    node("gun", 6, 0);
    node("engine", 0, 0);
    const auto s = mission::decode_scheme(b);
    CHECK(s.root.entry == "base" && s.root.children.size() == 2);
    CHECK(s.root.children[0].entry == "turret" && s.root.children[0].mount == 1);
    CHECK(s.root.children[0].children.size() == 1 && s.root.children[0].children[0].mount == 6);
    CHECK(s.root.children[1].entry == "engine");
    b.push_back(0);
    bool threw = false;
    try {
        mission::decode_scheme(b);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw); // trailing bytes
}

void test_input() {
    using namespace parkan::input;
    CHECK(scan_from_hid(0x04) == 0x1e);  // A
    CHECK(scan_from_hid(0x52) == 0x148); // Up arrow: extended (gray) G_UP
    CHECK(scan_from_hid(0x4b) == 0x149); // Page Up (Fn+Up on a Mac)
    CHECK(scan_from_hid(0xe2) == 0);     // Option: unbindable, as Alt was
    const auto t = parse_translate_table(
        "// comment\r\nKEY   SCAN_NULL SCAN_G_PGUP 1 CICLS_UNKNOWN MCMD_UP  1.0 0 0 0.0 0 // fly up\r\n"
        "Command % not implemented for device\r\n"
        "MOUSE SCAN_LSHIFT SCAN_MOUSE_X 1 CICLS_TURRET MCMD_ANGLE_X 0.15 1 MAN_WRAP 0.0 0\r\n");
    CHECK(t.size() == 2);
    CHECK(t[0].key == 0x149 && t[0].cmd == kMcmdUp && t[0].value == 1.f && t[0].comment == "fly up");
    CHECK(t[1].device == Device::Mouse && t[1].modifier == 0x2a && t[1].cls == 1 && t[1].state == 0);
    auto m = t;
    apply_mac_defaults(m);
    CHECK(m[0].key == 0x028); // Page Up -> '
    apply_mac_defaults(m);
    CHECK(m[0].key == 0x028); // idempotent
}

// CTPT (notes/formats-wea-cpt-ndp.md): u32 n, n 36-byte points {flags, meshPart, damagePart, pos, vec}, names.
bool near(float a, float b, float tol) { return std::abs(a - b) <= tol; }
Vec3 mat_t(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

// notes/motion-runtime.md §7: r_l_01 hand simulations; the numbers are from tools/motion_sim/sim.py.
struct SimStep {
    std::uint32_t state;
    float start_ms, dur_ms, w, b_from, b_to;
    Vec3 root; // blended root displacement of the step (body frame)
};

void test_unit_motion(const std::filesystem::path& game) {
    std::printf("unit motion (r_l_01)\n");
    AssetLibrary lib(game);
    const NResEntry *ce = lib.find("bases.rlb", "r_l_01.ctl"), *me = lib.find("bases.rlb", "r_l_01.msh"),
                    *pe = lib.find("bases.rlb", "r_l_01.cpt");
    if (!ce || !me || !pe) {
        CHECK(!"r_l_01 ctl/msh/cpt missing");
        return;
    }
    const Ctld ctld = decode_ctld(ce->data);
    const Mesh mesh = decode_mesh(me->data);
    const std::vector<ContactPoint> points = decode_ctpt(pe->data);

    {
        UnitMotion u(ctld, mesh, points);
        MotionGraph g = u.graph();
        CHECK(near(g.cost(15, 0), 8.9f, 1e-5f) && near(g.cost(19, 15), 6.7f, 1e-5f));
        CHECK(near(g.cost(29, 23), 6.7f, 1e-5f) && near(g.cost(8, 0), 2.89f, 1e-5f));
        CHECK(near(g.cost(8, 14), 4.74f, 1e-5f) && g.cost(20, 19) == 0.f);
        CHECK(near(g.shortest_path(0, 19), 15.6f, 1e-5f) && g.next(0) == 15 && g.next(15) == 19);
        CHECK(g.shortest_path(0, 29) == g.shortest_path(0, 19)); // the tie that index order breaks
        CHECK(near(g.states[19].len_b, 0.518212f, 1e-5f) && g.states[19].len_a == 0.f);
        CHECK(near(g.states[19].start_b.y, 0.319997f, 1e-5f));
        // Contacts: r_l_01 constraints are 0x125 / 0x205 / 0x204; feet start down.
        CHECK(u.contacts().size() == 2 && u.contacts()[0].point == 0 && u.contacts()[1].point == 2);
        CHECK((g.states[0].contacts[0].flags & 0x1001) == 0x1001);
    }

    const auto run = [&](Vec3 v, Vec3 a, std::span<const SimStep> expect, bool walk) {
        UnitMotion u(ctld, mesh, points);
        u.set_linear_velocity(v);
        u.set_angular_command(a);
        u.start(0);
        float yaw = 0;
        for (std::size_t k = 0; k < expect.size(); ++k) {
            const SimStep& e = expect[k];
            u.update(k == 0 ? 1.f : u.clock()); // exactly one motion step per call
            const Vec3 d = mat_t(u.end_matrix()) - mat_t(u.start_matrix());
            const float len = std::sqrt(dot(d, d)), rlen = std::sqrt(dot(e.root, e.root));
            CHECK(u.current_state() == e.state);
            CHECK(near(u.step_start(), e.start_ms, 2e-3f) && near(u.step_duration(), e.dur_ms, 1e-3f));
            CHECK(near(u.weight(), e.w, 1e-5f) && near(u.prev_weight(), k ? expect[k - 1].w : 1.f, 1e-5f));
            CHECK(u.live().track_a[0] == 0.f && u.live().track_a[1] == 0.f);
            CHECK(near(u.live().track_b[0], e.b_from, 1e-4f) && near(u.live().track_b[1], e.b_to, 1e-4f));
            CHECK(near(len, rlen, 1e-5f)); // the step rotation does not change the length
            if (walk) {
                CHECK(near(d.x, e.root.x, 1e-5f) && near(d.y, e.root.y, 1e-5f) && near(d.z, e.root.z, 1e-5f));
                if (k > 0) CHECK(near(len / (u.step_duration() * 0.001f), 5.f, 1e-3f)); // 5.000 m/s
            } else {
                // 0.5 · body[11] · 0.10714 s of yaw per step (runtime body = file body: 4.2 rad/s).
                const Mat4& m = u.end_matrix();
                const float step = std::atan2(m.at(1, 0), m.at(0, 0)) - yaw;
                CHECK(near(std::remainder(step, 6.2831853f), 0.5f * 4.2f * 0.1071429f, 1e-4f));
                yaw += step;
            }
        }
        return u;
    };

    static const SimStep walk[] = {
        {15, 0.0000f, 500.0000f, 1.000000f, 2.0000f, 4.0000f, {0.190309f, -0.034253f, 0.000000f}},
        {19, 500.0000f, 59.0944f, 0.570175f, 10.0000f, 10.8000f, {0.027368f, 0.291930f, 0.036491f}},
        {20, 559.0944f, 58.4305f, 0.570175f, 10.8000f, 11.6000f, {-0.000000f, 0.291930f, -0.011404f}},
        {21, 617.5249f, 58.5922f, 0.570175f, 11.6000f, 12.4000f, {-0.009123f, 0.291930f, -0.022807f}},
        {22, 676.1171f, 58.4590f, 0.570175f, 12.4000f, 13.2000f, {-0.009123f, 0.291930f, -0.011404f}},
        {23, 734.5761f, 58.4430f, 0.570175f, 13.2000f, 14.0000f, {-0.009123f, 0.291930f, 0.009123f}},
        {29, 793.0191f, 58.8403f, 0.570175f, 14.0000f, 14.8000f, {0.000000f, 0.291930f, 0.036491f}},
        {30, 851.8594f, 58.5745f, 0.570175f, 14.8000f, 15.6000f, {-0.020526f, 0.291930f, -0.011404f}},
        {31, 910.4340f, 58.5922f, 0.570175f, 15.6000f, 16.4000f, {-0.009123f, 0.291930f, -0.022807f}},
        {32, 969.0262f, 58.4325f, 0.570175f, 16.4000f, 17.2000f, {0.011404f, 0.291717f, -0.011404f}},
        {33, 1027.4587f, 58.3582f, 0.570175f, 17.2000f, 18.0000f, {0.018246f, 0.291077f, 0.009123f}},
        {19, 1085.8170f, 59.0944f, 0.570175f, 10.0000f, 10.8000f, {0.027368f, 0.291930f, 0.036491f}},
    };
    {
        UnitMotion u = run({0, 5, 0}, {}, walk, true);
        CHECK(u.queue().size() == 9 && u.queue().front() == 20 && u.queue().back() == 33);
        // Per frame: the blend moves from prevW to w over the first 25 % of the step; time is linear.
        const float t0 = u.step_start(), dur = u.step_duration(), w = u.weight();
        MotionFrame f = u.interpolate(t0 + dur * 0.125f);
        CHECK(near(f.frame_b, 10.1f, 1e-4f) && f.frame_a == 0.f && near(f.weight_b, w, 1e-6f)); // prevW = w here
        f = u.interpolate(t0 + dur);
        const Vec3 end = mat_t(u.end_matrix()), out = mat_t(f.matrix);
        CHECK(near(out.x, end.x, 1e-4f) && near(out.y, end.y, 1e-4f) && near(out.z, end.z, 1e-4f));

        // The step after 15 starts with prevW = 1: the blend at u = 1/8 is halfway to w.
        UnitMotion s(ctld, mesh, points);
        s.set_linear_velocity({0, 5, 0});
        s.start(0);
        s.update(1);
        s.update(s.clock());
        f = s.interpolate(s.step_start() + s.step_duration() * 0.125f);
        CHECK(near(f.weight_b, 0.5f * (1 + 0.570175f), 1e-5f) && near(f.frame_b, 10.1f, 1e-4f));
    }

    static const SimStep yaw[] = {
        {8, 0.0000f, 107.1429f, 1.000000f, 464.0000f, 465.2857f, {0.235098f, 0.118267f, -0.024207f}},
        {9, 107.1429f, 107.1429f, 1.000000f, 465.2857f, 466.5714f, {0.012289f, 0.051047f, -0.019367f}},
        {10, 214.2857f, 107.1429f, 1.000000f, 466.5714f, 467.8571f, {-0.050073f, -0.035734f, 0.009410f}},
        {11, 321.4286f, 107.1429f, 1.000000f, 467.8571f, 469.1429f, {-0.080851f, -0.055464f, 0.014293f}},
        {12, 428.5714f, 107.1429f, 1.000000f, 469.1429f, 470.4286f, {-0.082737f, -0.055397f, 0.014078f}},
        {13, 535.7143f, 107.1429f, 1.000000f, 470.4286f, 471.7143f, {-0.033727f, -0.022719f, 0.005794f}},
        {14, 642.8572f, 107.1429f, 1.000000f, 471.7143f, 473.0000f, {0.000000f, 0.000000f, 0.000000f}},
        {8, 750.0000f, 107.1429f, 1.000000f, 464.0000f, 465.2857f, {0.235098f, 0.118267f, -0.024207f}},
        {9, 857.1429f, 107.1429f, 1.000000f, 465.2857f, 466.5714f, {0.012289f, 0.051047f, -0.019367f}},
    };
    {
        UnitMotion u = run({}, {0, 0, 0.5f}, yaw, false);
        // Back inside ±0.1: the next selectable state on the cycle (14) leads to idle 0.
        u.set_angular_command({});
        while (u.current_state() != 14) u.update(u.clock());
        u.update(u.clock());
        CHECK(u.current_state() == 0 && near(u.step_duration(), 250.f, 1e-3f));
    }
}

// CManManager: one-shot press/release bindings set a channel, ramps move toward VALUE by
// ACCEL·min(1, elapsed/RAMP_MS) per frame, and a modified binding wins while its modifier is held.
void test_man_manager() {
    using namespace parkan::input;
    auto table = parse_translate_table(
        "KEY SCAN_NULL SCAN_G_UP 1 CICLS_UNKNOWN MCMD_WALK_F 1.0 0 0 0.0 0\n"
        "KEY SCAN_NULL SCAN_G_UP 0 CICLS_UNKNOWN MCMD_WALK_F 0.0 0 0 0.0 0\n"
        "KEY SCAN_NULL SCAN_W_PLUS 1 CICLS_UNKNOWN MCMD_FORWARD 1.0 0 0 0.05 1000\n"
        "KEY SCAN_NULL SCAN_COMMA 1 CICLS_UNKNOWN MCMD_ROTATE_Z 0.7 0 0 0.0 0\n"
        "KEY SCAN_NULL SCAN_COMMA 0 CICLS_UNKNOWN MCMD_ROTATE_Z 0.0 0 0 0.0 0\n"
        "KEY SCAN_LSHIFT SCAN_COMMA 1 CICLS_UNKNOWN MCMD_ROTATE_Z 0.2 0 0 0.0 0\n");
    ManManager m(table);
    CommandState c;
    const std::uint16_t up = 0x148, plus = 0x0d, comma = 0x33, shift = 0x2a;
    InputEvent press_up[] = {{true, up}};
    m.process(press_up, 0, 0, 16, c);
    CHECK(c.get(0, 0, kMcmdWalkF) == 1.f);
    m.process(press_up, 0, 0, 16, c); // auto-repeat: ignored
    InputEvent release_up[] = {{false, up}};
    m.process(release_up, 0, 0, 16, c);
    CHECK(c.get(0, 0, kMcmdWalkF) == 0.f);

    InputEvent press_plus[] = {{true, plus}};
    m.process(press_plus, 0, 0, 500, c); // elapsed 500 of 1000 ms: step 0.05·0.5
    CHECK(std::abs(c.get(0, 0, kMcmdForward) - 0.025f) < 1e-6f);
    m.process({}, 0, 0, 500, c); // elapsed 1000: step 0.05
    CHECK(std::abs(c.get(0, 0, kMcmdForward) - 0.075f) < 1e-6f);

    InputEvent turn[] = {{true, comma}};
    m.process(turn, 0, 0, 16, c);
    CHECK(c.get(0, 0, kMcmdRotateZ) == 0.7f);
    InputEvent shift_down[] = {{true, shift}};
    m.process(shift_down, 0, 0, 16, c); // Shift while COMMA is held switches to the Shift binding
    CHECK(c.get(0, 0, kMcmdRotateZ) == 0.2f);
    InputEvent release_comma[] = {{false, comma}};
    m.process(release_comma, 0, 0, 16, c);
    CHECK(c.get(0, 0, kMcmdRotateZ) == 0.f);
}

void test_corpus(const std::filesystem::path& game) {
    test_unit_motion(game);

    // Tut_2's sky at 07:00 (notes/atmosphere-render.md §10): the clock starts at 01:30 and a day lasts
    // 900 s, so 07:00 is 900·330/1440 s in; that is a layer time, so its values come out unblended.
    if (auto p = resolve_path(game, "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02/sky.ske")) {
        const auto ske = mission::decode_ske(read_file(*p));
        const auto a = mission::evaluate_ske(ske, 900.0 * 330 / 1440);
        CHECK(std::abs(a.fog_end - 560.f) < 0.01f);
        CHECK(std::abs(a.ambient.x - 0x28 / 255.f) < 1e-4f);
        CHECK(a.sun && !a.is_moon);
        CHECK(std::abs(a.light_color.x - 2.1f) < 1e-3f); // FFFFF5E6 × 2.1: red 255/255·2.1
        CHECK(a.light_dir.z < -0.5f);                     // mid-morning: the sun is well up
    } else {
        CHECK(!"Tut_2 sky.ske missing");
    }

    std::printf("corpus\n");
    auto dir = resolve_path(game, "MISSIONS/SCRIPTS");
    if (!dir) {
        std::printf("  (no MISSIONS/SCRIPTS, skipped)\n");
        return;
    }
    struct ZeroHost : ScriptHost {
        std::size_t calls = 0;
        std::uint32_t call_handler(std::uint32_t, ScriptVM&, const Record&) override {
            ++calls;
            return 0;
        }
    } host;
    int bundles = 0, events = 0, exhausted = 0;
    for (const auto& e : std::filesystem::directory_iterator(*dir)) {
        if (e.path().extension() != ".scr") continue;
        try {
            ScriptVM vm(load_script_bundle(*dir, e.path().stem().string()));
            vm.record_budget = 100'000;
            ++bundles;
            for (std::uint32_t i = 0; i < vm.program().events.size(); ++i, ++events) {
                vm.run_event(i, host);
                exhausted += vm.budget_exhausted;
            }
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("  FAILED %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("  %d bundles, %d events run, %zu handler calls, %d hit the record budget\n", bundles, events,
                host.calls, exhausted);
}

} // namespace

int main(int argc, char** argv) {
    test_formulas();
    test_vm();
    test_vm_set_ignores_if();
    test_anim_sampling();
    test_scheme();
    test_input();
    test_man_manager();
    if (argc > 1) test_corpus(argv[1]);
    std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
