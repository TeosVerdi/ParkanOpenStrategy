#include "ui/hq.hpp"

#include "ui/designer.hpp"

#include "ui/hud_resources.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace parkan::ui {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
using game::kNoObject;

// Order types (behaviour-data.md) the HQ gives.
enum : std::uint32_t {
    kGo = 2, kAttack = 3, kPatrol = 4, kSearch = 5, kTransport = 6, kBuild = 7, kReload = 8, kConstruct = 0xc, kResearch = 0xe,
    kCapture = 0x11, kStayGround = 0x15, kFollow = 0x16, kUpgrade = 0x18,
};

game::UnitOrder make_order(std::uint32_t type, std::uint32_t param, std::uint32_t kind) {
    game::UnitOrder o;
    o.type = type, o.param = param, o.target_kind = kind;
    return o; // limits: FUN_1007d3a0 defaults, the last four 0, 0, 0, 1.0 (UnitOrder's defaults)
}

bool in(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

bool is_bunker(std::uint32_t t) { return t == game::kTypeBunkerSmall || t == game::kTypeBunkerMedium || t == game::kTypeBunkerLarge; }
bool is_tower(std::uint32_t t) { return t == game::kTypeTowerMedium || t == game::kTypeTowerLarge; }

} // namespace

// --- tables ---

bool view_transition_exists(int from, int to) {
    // g_ViewStateTransitions 10103b18: non-null handlers (rows = the current top state).
    static constexpr std::uint8_t kRows[8] = {
        0b11110010, // 0 → 1, 4, 5, 6, 7
        0b10001001, // 1 → 0, 3, 7
        0b11111000, // 2 → 3, 4, 5, 6, 7
        0b11011110, // 3 → 1, 2, 3, 4, 6, 7
        0b11011101, // 4 → 0, 2, 3, 4, 6, 7
        0b10000101, // 5 → 0, 2, 7
        0b10011101, // 6 → 0, 2, 3, 4, 7
        0b01111111, // 7 → 0..6
    };
    if (from < 0 || from > 7 || to < 0 || to > 7) return false;
    return (kRows[from] >> to) & 1;
}

int hq_cursor_for(int action) {
    // UpdateHqCursor 10058be0.
    switch (action) {
    case 1: case 0xc: return kCurPlace;
    case 2: return kCurWrong;
    case 3: return kCurTarget;
    case 4: return kCurCapture;
    case 5: case 6: case 7: return kCurPick;
    case 8: case 9: case 10: return kCurGuard;
    default: return kCurArrow;
    }
}

std::uint32_t screen_mask(int screen) {
    switch (screen) {
    case kScreenWarbots: return 0x1018000;
    case kScreenTransports: return 0x1002000;
    case kScreenBuilders: return 0x1004000;
    case kScreenPlants: return 0x80000010;
    case kScreenTowers: return 0x80300000;
    case kScreenBunkers: return 0x80070000;
    case kScreenOther: return 0x8000044e;
    default: return 0;
    }
}

const HqMenuItem kOrderMenu[22] = {
    // 10103f98 (read 2026-10-25).
    {0, 0x103e000, false, 0, 5000},  {1, 0x103e000, true, 4, 5001},  {2, 0x103e000, false, 0, 5002},
    {3, 0x103e000, false, 0, 5003},  {6, 0x103e000, true, 3, 5006},  {7, 0x103e000, false, 0, 5007},
    {8, 0x1002000, false, 0, 5020},  {9, 0x1004000, false, 4, 5030},  {10, 0x1004000, true, 4, 5031},
    {11, 0x1004000, true, 4, 5032},  {12, 0x1004000, true, 4, 5033}, {13, 0x1004000, true, 4, 5034},
    {14, 0x1004000, true, 4, 5035},  {15, 0x1004000, true, 4, 5036}, {16, 0x1004000, true, 4, 5046},
    {17, 0x1004000, true, 0, 1008},  {18, 0x1004000, true, 0, 1029}, {19, 0x1004000, true, 0, 1030},
    {20, 0x1004000, true, 0, 1031},  {21, 0x1004000, true, 0, 1032}, {22, 0x1004000, true, 0, 1033},
    {23, 0x1004000, true, 0, 5047},
};

namespace {
// COrderMenu_Execute cases 10–16 (build: pending mode 3 with this building mask) and 17–23 (upgrade, cmd 0x20).
constexpr std::uint32_t kBuildMask[7] = {game::kTypeMine, game::kTypeStorage, game::kTypePlant, game::kTypeHangar,
                                         game::kTypeInstitute, game::kTypeTowerMedium, game::kTypeTowerLarge};
} // namespace

std::uint32_t order_type_text(std::uint32_t t) {
    switch (t) { // GetOrderTypeName 100773e0
    case 0: return 0x1824;
    case 1: return 0x1825;
    case 2: return 0x1828;
    case 3: return 0x182c;
    case 4: return 0x182d;
    case 5: return 0x182e;
    case 6: return 0x182f;
    case 7: return 0x1830;
    case 8: return 0x1831;
    case 9: return 0x1832;
    case 0x11: return 0x1833;
    case 0x13: return 0x1826;
    case 0x14: return 0x1829;
    case 0x15: return 0x1827;
    case 0x16: return 0x182a;
    case 0x17: return 0x182b;
    case 0x18: return 0x1834;
    default: return 0x1835;
    }
}

HqSprites make_hq_sprites(const std::map<std::string, HudSprite>& cfg) {
    HqSprites s;
    for (const auto& [name, h] : cfg) {
        HudPage page;
        if (h.texture == "ui_menu") page = HudPage::UiMenu;
        else if (h.texture == "ui_menu3") page = HudPage::UiMenu3;
        else if (h.texture == "page6") page = HudPage::Page6;
        else if (h.texture == "page7") page = HudPage::Page7;
        else if (h.texture == "page9") page = HudPage::Page9;
        else if (h.texture == "new_ui1") page = HudPage::NewUi1;
        else if (h.texture == "icons") page = HudPage::Icons;
        else if (h.texture == "page1") page = HudPage::Page1;
        else if (h.texture == "page3") page = HudPage::Page3;
        else if (h.texture == "page4") page = HudPage::Page4;
        else if (h.texture == "page5") page = HudPage::Page5;
        else continue;
        s.by_name[name] = Sprite{page, float(h.x), float(h.y), float(h.w), float(h.h), h.rot};
    }
    return s;
}

// --- camera (§1) ---

void HqCamera::set_pose(Vec3 p, float y) {
    pos = p, yaw = y; // roll 0, pitch 0 (recomputed from the tilt by HqCam_Integrate)
}

void HqCamera::toggle_zoom() {
    // Only when the FOV is not between the two (10072120).
    if (fov_ <= kZoomFov + 1e-4f || fov_ >= kNormalFov - 1e-4f) zoomed = !zoomed;
}

void HqCamera::key(Key k, bool down, double now) {
    Latch& l = keys_[k];
    if (l.on != down) l.on = down, l.since = now;
}

Vec3 HqCamera::forward() const {
    const float p = kPi / 2 - tilt;
    return {std::cos(yaw) * std::cos(p), std::sin(yaw) * std::cos(p), -std::sin(p)};
}

Vec3 HqCamera::up() const {
    const float p = kPi / 2 - tilt;
    return {std::cos(yaw) * std::sin(p), std::sin(yaw) * std::sin(p), std::cos(p)};
}

void HqCamera::update(double now, float dt, float mx, float my, bool edges, const std::function<float(float, float)>& ground,
                      float map_size) {
    // The timer bookkeeping of the original (a latch's change time, an axis' last integration) becomes: t =
    // seconds since the latch changed; a released axis decays v = (0.5 − Δ)·v per integration step, Δ = dt.
    auto ramp = [&](const Latch& l) { return float(now - l.since); };
    auto decay = [&](float& v) { v = dt < kRamp ? (kRamp - dt) * v : 0.f; };
    // Edge latches (HqCam_Update 10037bc0): x < 7, x > 633 (0x279), y < 7, y > 473 (0x1d9).
    if (edges) {
        const bool e[4] = {mx < 7, mx > 633, my < 7, my > 473};
        for (int i = 0; i < 4; ++i)
            if (edge_[i].on != e[i]) edge_[i].on = e[i], edge_[i].since = now;
    }
    // HqCam_UpdateTurnRates 10037540: R = 0.2 zoomed, else 1.5; ramp 2·t·R up to R in 0.5 s.
    const float R = zoomed ? kZoomTurnRate : kTurnRate;
    auto turn = [&](const Latch& l, float sign) { const float t = ramp(l); return sign * (t <= kRamp ? 2 * t * R : R); };
    if (edge_[1].on) yaw_rate_ = turn(edge_[1], -1);
    else if (edge_[0].on) yaw_rate_ = turn(edge_[0], 1);
    else if (yaw_rate_ != 0) decay(yaw_rate_);
    if (edge_[3].on) tilt_rate_ = turn(edge_[3], -1);
    else if (edge_[2].on) tilt_rate_ = turn(edge_[2], 1);
    else if (tilt_rate_ != 0) decay(tilt_rate_);
    // Zoom (view mode 2): FOV steps 0.1 per frame toward 0.2 or back to the normal FOV.
    if (zoomed && fov_ > kZoomFov) fov_ -= 0.1f;
    else if (!zoomed && fov_ < kNormalFov) fov_ += 0.1f;
    // HqCam_FollowTarget 10037f70: the goal behind the unit at follow_dist along the yaw and tilt; the move latches
    // drive toward it (dead zone 3 m in x / y, 2 m in z: snap there, speed 0).
    if (follow_pos) {
        const float dd = std::sin(tilt) * follow_dist;
        const Vec3 goal{follow_pos->x - std::cos(yaw) * dd, follow_pos->y - std::sin(yaw) * dd, follow_pos->z + std::cos(tilt) * follow_dist};
        auto steer = [&](Key neg, Key pos_key, float& cur, float g, float dead, float& speed) {
            auto set = [&](Key k, bool on) {
                if (keys_[k].on != on) keys_[k].on = on, keys_[k].since = now;
            };
            if (std::abs(cur - g) <= dead) {
                cur = g, speed = 0;
                set(pos_key, false), set(neg, false);
            } else {
                set(pos_key, cur < g), set(neg, !(cur < g));
            }
        };
        steer(Left, Right, pos.x, goal.x, 3, strafe_);
        steer(Back, Forward, pos.y, goal.y, 3, fwd_);
        steer(Down, Up, pos.z, goal.z, 2, vert_);
    }
    // HqCam_UpdateMoveSpeeds 100372a0.
    const float S = kSpeed;
    auto move = [&](const Latch& l, float sign) { const float t = ramp(l); return sign * (t <= kRamp ? 2 * t * S : S); };
    if (keys_[Left].on) strafe_ = move(keys_[Left], -1);
    else if (keys_[Right].on) strafe_ = move(keys_[Right], 1);
    else if (strafe_ != 0) decay(strafe_);
    if (keys_[Forward].on) fwd_ = move(keys_[Forward], 1);
    else if (keys_[Back].on) fwd_ = move(keys_[Back], -1);
    else if (fwd_ != 0) decay(fwd_);
    if (keys_[Up].on) {
        const float t = ramp(keys_[Up]);
        vert_ = t <= kRamp ? t * S : S * 0.5f;
    } else if (keys_[Down].on) {
        const float t = ramp(keys_[Down]);
        vert_ = t <= kRamp ? -t * S : S * -0.5f;
    } else if (vert_ != 0) {
        decay(vert_);
    }
    // HqCam_Integrate 10037720: each new coordinate is taken only inside (tan(FOV/2)·z, MapSize).
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float lo = std::tan(fov_ * 0.5f) * pos.z;
    if (follow_pos) {
        // HqCam_FollowStep 10038250: the speeds in world axes (x, y, z) and the yaw; z ≥ ground + 2.5; the
        // distance walks +1 per frame below 200 and −1 above 8 × the unit's radius. HqCam_Integrate then only
        // adds the tilt and its clamps (its other clocks were just restarted).
        if (strafe_ != 0)
            if (const float x = pos.x + strafe_ * dt; strafe_ > 0 ? x < map_size - lo : lo < x) pos.x = x;
        if (fwd_ != 0)
            if (const float y = pos.y + fwd_ * dt; fwd_ > 0 ? y < map_size - lo : lo < y) pos.y = y;
        pos.z += vert_ * dt;
        yaw += yaw_rate_ * dt;
        const float g = (ground ? ground(pos.x, pos.y) : 0.f) + 2.5f;
        if (pos.z < g) pos.z = g;
        if (follow_radius * 8 < follow_dist) follow_dist -= 1;
        if (follow_dist < 200) follow_dist += 1;
    } else if (strafe_ != 0) {
        const float d = strafe_ * dt;
        const float x = d * sy + pos.x;
        if (lo < x && x < map_size) pos.x = x;
        const float y = pos.y - d * cy;
        if (lo < y && y < map_size) pos.y = y;
    }
    if (fwd_ != 0) {
        const float d = fwd_ * dt;
        const float y = d * sy + pos.y;
        if (lo < y && y < map_size) pos.y = y;
        const float x = d * cy + pos.x;
        if (lo < x && x < map_size) pos.x = x;
    }
    if (!follow_pos && vert_ != 0) pos.z += vert_ * dt;
    if (!follow_pos && yaw_rate_ != 0) yaw += yaw_rate_ * dt;
    if (tilt_rate_ != 0) {
        const float t = tilt_rate_ * dt + tilt;
        if (0 < t && t < kPi / 2) tilt = t;
    }
    const float h = (ground ? ground(pos.x, pos.y) : 0.f) + kGroundClearance;
    if (pos.z < h) pos.z = h;
    else if (pos.z > h + kHeightBand) pos.z = h + kHeightBand;
    if (anchor_) {
        if (std::abs(std::round(pos.x - anchor_->x)) > kAnchorBox) pos.x = pos.x <= anchor_->x ? anchor_->x - kAnchorBox : anchor_->x + kAnchorBox;
        if (std::abs(std::round(pos.y - anchor_->y)) > kAnchorBox) pos.y = pos.y <= anchor_->y ? anchor_->y - kAnchorBox : anchor_->y + kAnchorBox;
    }
}

void HqCamera::anchor(Vec3 building) {
    // +0x44 (follow) cleared, the vertical latches and the three speeds zeroed, distance 0.
    keys_[Up].on = keys_[Down].on = false;
    strafe_ = fwd_ = vert_ = 0;
    follow_dist = 0, follow_pos.reset();
    anchor_ = building;
}

void HqCamera::follow_unit(Vec3 p, float radius) {
    keys_[Up].on = keys_[Down].on = false; // +0x90 / +0x91
    strafe_ = fwd_ = vert_ = 0;
    follow_dist = 0, follow_pos = p, follow_radius = radius;
    anchor_.reset(); // +0x48 = 0
}

Vec3 hq_pointer_ray(const HqCamera& cam, float vx, float vy, float aspect) {
    // The inverse of project_to_virtual (the viewer's look_at with a horizontal fov).
    const Vec3 f = normalize(cam.forward());
    const Vec3 s = normalize(cross(f, cam.up()));
    const Vec3 u = cross(s, f);
    const float t = std::tan(cam.fov() / 2);
    const float nx = (vx - 320) / 320, ny = (240 - vy) / 240;
    return normalize(f + s * (nx * t) + u * (ny * t / std::max(aspect, 1e-3f)));
}

// --- controller ---

HqController::HqController(game::World& world, economy::Economy* eco, std::uint32_t clan, HqHost host)
    : world_(world), eco_(eco), clan_(clan), host_(std::move(host)), designer_(std::make_unique<Designer>(*this)) {
    expand_now(); // LoadHqInterface 10082930 ends with HqUi_ExpandNow
    rebuild_plants();
    number_new_units();
}

HqController::~HqController() = default;

bool HqController::designer_open() const { return designer_->open(); }

void HqController::open_designer() {
    const HqPlant* p = current_plant();
    if (!p || !host_.library) {
        say("factory: the designer needs the plant and the asset library");
        return;
    }
    designer_->open_for_plant(eco_ ? eco_->building_size(p->lid) : 0);
    say("factory: the unit designer");
}

HqController::MenuAction HqController::menu_click(float vx, float vy) {
    // CGameMenuMainPage buttons (210, 210 + 25k)-(430, 235 + 25k): Return, Save, Load, Exit.
    if (!menu_open() || !in(vx, vy, 210, 210, 430, 310)) return MenuAction::None;
    switch (int((vy - 210) / 25)) {
    case 0:
        pop_state(); // ProcessTransitions
        return MenuAction::Resume;
    case 1: return menu_save_enabled ? MenuAction::Save : MenuAction::None;
    case 2: return menu_load_enabled ? MenuAction::Load : MenuAction::None;
    default: return MenuAction::Exit;
    }
}

void HqController::on_unit_removed(std::uint32_t lid, bool local_hero, bool controlled) {
    if (cstate_ == 1) {
        std::erase(units_, lid);
        if (units_.empty()) cstate_ = 0;
    }
    if (local_hero) {
        clear_units();
        return;
    }
    if (controlled) {
        const int s = state();
        if (s == kStatePilot || s == kStateRemote || s == kStateInBuilding || s == kStateMenu) pop_state();
    }
}

void HqController::key_char(char c) {
    if (designer_->open()) designer_->key_char(c);
}

void HqController::number_new_units() {
    // FUN_100761a0 runs at CUnit::BindInterfaces for every unit of every clan: `++player[clan]+0x10` names it.
    for (const auto& o : world_.objects())
        if (!o.building() && o.logic_id != kNoObject && !unit_number_.count(o.logic_id))
            unit_number_[o.logic_id] = ++clan_unit_counter_[o.clan];
}

namespace {
// GetObjectTypeName 10033a90: a building's name string by type and size class (property 0x201).
std::uint32_t building_name_id(std::uint32_t type, std::uint32_t size) {
    auto by = [&](std::uint32_t first, std::uint32_t lo, std::uint32_t hi) { return size >= lo && size <= hi ? first + (size - lo) : 0x183d; };
    switch (type) {
    case game::kTypeBridge: return by(0x17b2, 2, 5);
    case game::kTypePlant: return by(0x17a3, 2, 4);
    case game::kTypeGenerator: return 0x1799;
    case game::kTypeMine: return by(0x178f, 2, 4);
    case game::kTypeStorage: return by(0x1794, 2, 4);
    case game::kTypeHangar: return 0x17a8;
    case game::kTypeTeleport: return by(0x17b7, 2, 3);
    case game::kTypeInstitute: return by(0x179e, 2, 5);
    case game::kTypeRuins: return by(0x17ad, 2, 5);
    case game::kTypeBunkerSmall: return 0x17c6;
    case game::kTypeBunkerMedium: return 0x17cc;
    case game::kTypeBunkerLarge: return 0x17d2;
    case game::kTypeTowerMedium: return 0x17bd;
    case game::kTypeTowerLarge: return 0x17c3;
    default: return 0x183d;
    }
}
} // namespace

std::string HqController::display_name(std::uint32_t lid) const {
    const game::WorldObject* o = world_.find(lid);
    if (!o || !host_.text) return {};
    if (o->building()) return host_.text(building_name_id(o->type, eco_ ? eco_->building_size(lid) : 0));
    if (o->type == game::kTypeHero) return host_.text(0x1856);
    if (o->type == game::kTypeAnimal) return host_.text(0x186d);
    // [deferred] the 0x17bc case (a robot whose unit+0x64 device reports a positive value) is not identified.
    const auto st = host_.unit_stats ? host_.unit_stats(lid) : std::nullopt;
    const std::string code = economy::unit_debug_code(st ? st->size_class : o->size_class, st ? st->chassis_type : 0, o->type);
    auto n = unit_number_.find(lid);
    return code + "-" + std::to_string(n != unit_number_.end() ? n->second : 0) + " " + host_.text(economy::unit_type_name_id(o->type));
}

void HqController::say(std::string line) {
    if (host_.log) host_.log(line);
    log.push_back(std::move(line));
}

bool HqController::own(std::uint32_t lid) const {
    const game::WorldObject* o = world_.live(lid);
    return o && o->clan == clan_;
}
bool HqController::own_alive(std::uint32_t lid) const { return own(lid); }
std::uint32_t HqController::type_of(std::uint32_t lid) const {
    const game::WorldObject* o = world_.find(lid);
    return o ? o->type : 0;
}
std::uint32_t HqController::kind_of(std::uint32_t lid) const {
    // Unit +0x30 (the voice / size kind). [inferred] = the size class (property 0x201).
    const game::WorldObject* o = world_.find(lid);
    return o ? o->size_class : 0;
}
bool HqController::selected(std::uint32_t lid) const { return std::find(units_.begin(), units_.end(), lid) != units_.end(); }

int HqController::view_mode() const {
    // ViewTrans_HqToMenu 10064af0 keeps view mode 2 under the menu; the others leave the view below as it was.
    const int s = state() == kStateMenu && states_.size() >= 2 ? states_[states_.size() - 2].state : state();
    return s == kStateHqUnit || s == kStateHqBunker ? 2 : s == kStateTurret ? 6 : 1;
}

// --- view states ---

bool HqController::push_state(int s, std::uint32_t object) {
    const int from = state();
    if (!view_transition_exists(from, s)) {
        say("view: push " + std::to_string(s) + " refused from " + std::to_string(from));
        return false;
    }
    // The handler for from → s (g_ViewStateTransitions). [port] States without a ported view are not
    // pushed (the hero would be left without control): logged instead.
    if (s == kStateHqBunker) {
        enter_hq(object);
    } else if (s == kStateInBuilding) {
        // ViewTrans_HeroToBuildingScreen 100648d0: clear the unit selection, the hero's control off, view mode 1.
        clear_units();
    } else if (s == kStateMenu) {
        // ViewTrans_HeroToMenu 10064a80 / HqToMenu 10064af0 / BuildingScreenToMenu 10064c40: the controlled unit's
        // input off, CGameMenu_Show(1) (page 0; the host pauses).
    } else if (s == kStateHqUnit) {
        // CViewStateQueue::Push: state 3 only for an HQ-capable unit; a state-3 record of that unit below the top
        // is popped back to instead (CheckPushDuplicate 10062ee0); at the top: nothing.
        if (!hq_capable(object)) return false;
        for (std::size_t i = states_.size(); i-- > 0;)
            if (states_[i].state == kStateHqUnit && states_[i].object == object) {
                while (states_.size() > i + 1 && pop_state()) {
                }
                return false;
            }
        if (host_.transition && !host_.transition(from, s, object)) return false;
        // ViewTrans_WarbotToHqUnit 10063ec0 / RemoteToHqUnit 10064090 / HqUnitToHqUnit 10064da0 /
        // HqBunkerToHqUnit 10064c80: the HQ camera posed at the unit (yaw π/2) and following it, view mode 2; the
        // unit back to its AI (the host). From 3 / 4 the unit becomes the selection; from 1 the screen closes.
        if (from == kStateHqUnit || from == kStateHqBunker) clear_units(), select_unit(object, true);
        if (from == kStatePilot) set_screen(kScreenNone);
        if (const game::WorldObject* u = world_.live(object)) {
            camera.set_pose(u->position());
            camera.follow_unit(u->position(), host_.radius ? host_.radius(object) : 1.f);
        }
        if (from == kStateHqBunker) clear_building();
        states_.push_back({s, object});
        say("view: push 3");
        return true;
    } else if (s == kStateTurret) {
        // ViewTrans_HeroToTurret 10064470 / RemoteToTurret 100647b0 / HqUnitToTurret 100646b0 /
        // HqBunkerToTurret 100645a0: the selection cleared (from 0 / 2), the building's turret on (the host),
        // view mode 6; from 4 the HQ camera detaches (from 3 it stays attached).
        if (host_.transition && !host_.transition(from, s, object)) {
            say("view: push 6 refused by the game");
            return false;
        }
        if (from == kStateHero || from == kStateRemote) clear_units();
        if (from == kStateHqBunker) camera.detach();
        states_.push_back({s, object});
        say("view: push 6");
        return true;
    } else if (s == kStateRemote) {
        // ViewTrans_HqUnitToRemote 10063fe0 / HqBunkerToRemote 10064330: the selection = the unit; the host hands
        // it to the player (Unit_SetPlayerControl) and leaves the HQ camera (view mode 1). The HQ screen stays.
        if (host_.transition && !host_.transition(from, s, object)) {
            say("view: push 2 refused by the game");
            return false;
        }
        clear_units();
        select_unit(object, true);
        states_.push_back({s, object});
        say("view: push 2");
        return true;
    } else if (s == kStatePilot) {
        // ViewTrans_HeroToWarbot 10063c60: the host hides the hero and hands the warbot to the player; the warbot
        // becomes the selection (SelectUnit, exclusive).
        if (host_.transition && !host_.transition(from, s, object)) {
            say("view: push 1 refused by the game");
            return false;
        }
        select_unit(object, true);
        states_.push_back({s, object});
        say("view: push 1");
        return true;
    } else {
        say("view: state " + std::to_string(s) + " is not ported (requested for " + std::to_string(object) + ")");
        return false;
    }
    states_.push_back({s, object});
    say("view: push " + std::to_string(s));
    return true;
}

void HqController::enter_hq(std::uint32_t building) {
    // ViewTrans_HeroToHqBunker 10064140 (also 4 → 4, another bunker): the HQ camera anchored at the bunker and
    // posed at its position with yaw π/2, view mode 2; mouse latches cleared; an open screen closes.
    const game::WorldObject* b = world_.find(building);
    const Vec3 p = b ? b->position() : Vec3{};
    camera.anchor(p);
    camera.set_pose(p, 1.5707964f);
    lbutton_ = boxing_ = false;
    if (screen_ != kScreenNone) set_screen(kScreenNone);
}

void HqController::leave_to_hero() {
    // ViewTrans_HqBunkerToHero 10064200 / ViewTrans_BuildingScreenToHero 10064950: the hero's control back
    // on, the unit selection cleared (from the HQ), view mode 1, the HQ camera detached. The lift rises by
    // itself once the hero has left the capture volume (CBuilding tick, notes/buildings.md §3.4).
    if (state() == kStateHqBunker) clear_units();
    camera.detach();
    end_placement();
    pending_.reset();
    mode_ = kModeFree;
}

bool HqController::pop_state() {
    if (states_.size() <= 1) return false;
    const int from = state();
    const int to = states_[states_.size() - 2].state;
    // ViewTrans_WarbotToHero 10063d60 can veto (no exit point: lamp 4, pager 6211); the warbot is deselected.
    if ((from == kStatePilot || from == kStateRemote) && to != kStateMenu) {
        if (host_.transition && !host_.transition(from, to, states_[states_.size() - 2].object)) return false;
        clear_units();
    }
    if (from == kStateTurret && to != kStateMenu) {
        // ViewTrans_TurretTo* (10064510 / 10064850 / 10064720 / 10064620): the turret off (the host); to 3 the HQ
        // camera follows the unit again (no new pose), to 4 it is anchored at the bunker (no new pose); the HQ
        // screen re-opens (it was never closed here).
        if (host_.transition && !host_.transition(from, to, states_[states_.size() - 2].object)) return false;
        const std::uint32_t below = states_[states_.size() - 2].object;
        if (const game::WorldObject* o = world_.find(below)) {
            if (to == kStateHqUnit) camera.follow_unit(o->position(), host_.radius ? host_.radius(below) : 1.f);
            if (to == kStateHqBunker) camera.anchor(o->position());
        }
    }
    if (from == kStateHqUnit && (to == kStatePilot || to == kStateRemote)) {
        // ViewTrans_HqUnitToWarbot 10063f70 (unit+0x9c = 0) / HqUnitToRemote: back into the unit, HqCam_Detach.
        const std::uint32_t u = states_[states_.size() - 2].object;
        if (host_.transition && !host_.transition(from, to, u)) return false;
        if (to == kStatePilot) drive_mode[u] = 0;
        clear_units(), select_unit(u, true);
        camera.detach();
    }
    if (to == kStateHero && (from == kStateHqBunker || from == kStateInBuilding)) leave_to_hero();
    if (from == kStateInBuilding && screen_ != kScreenNone) set_screen(kScreenNone);
    states_.pop_back();
    // ViewTrans_MenuTo* (10064b50 / 10064bd0 / 10064c60): Show(0), the view below continues as it was.
    // ViewTrans_RemoteToHqBunker 100643d0: HqCam_AnchorBuilding without a new pose; the screen stays open.
    if (state() == kStateHqBunker && from != kStateMenu && from != kStateRemote && from != kStateTurret) enter_hq(state_object());
    say("view: pop to " + std::to_string(state()));
    return true;
}

void HqController::pop_to_hero() {
    while (states_.size() > 1 && pop_state()) {
    }
}

void HqController::hero_enters_building(std::uint32_t hero, std::uint32_t building) {
    if (state() == kStateMenu) return;
    select_building(building);
    map_open = false; // OnUICommandDown(0x2e4) when the satellite map is up
    const std::uint32_t t = type_of(building);
    if (t == game::kTypeInstitute) {
        if (push_state(kStateInBuilding, building)) set_screen(kScreenResearch);
    } else if (t == game::kTypePlant) {
        if (push_state(kStateInBuilding, building)) set_screen(kScreenPlants);
    } else if (t == game::kTypeGenerator || t == game::kTypeMine || t == game::kTypeStorage || t == game::kTypeHangar) {
        select_unit(hero, true); // SelectUnit refuses the hero (type 0x1020000); FUN_100a5600 sets the controlled unit
    } else if (is_bunker(t)) {
        push_state(kStateHqBunker, building);
    } else if (is_tower(t)) {
        // IsTurretDisabled 10034000: control property 0x52 ≤ 0 [port: never]
        push_state(kStateTurret, building);
    }
}

// --- selection ---

void HqController::select_unit(std::uint32_t lid, bool exclusive) {
    const game::WorldObject* o = world_.live(lid);
    if (!o || o->type == game::kTypeHero) return;
    if (!selected(lid) && host_.voice) host_.voice("VOICE_SELECTED", lid);
    mode_ = kModeFree;
    clear_building();
    if (exclusive) units_.clear();
    if (!selected(lid)) units_.push_back(lid);
    cstate_ = 1;
    // HqCam_FollowUnit(mission+0x68): the spectator camera follows the unit, not the HQ camera.
}

void HqController::clear_units() {
    units_.clear();
    if (cstate_ == 1) cstate_ = 0;
}

void HqController::select_building(std::uint32_t lid) {
    if (!world_.live(lid)) return;
    clear_units();
    building_ = lid;
    cstate_ = 2;
    if (type_of(lid) == game::kTypePlant) plant_ = lid; // the factory screen's plant
}

void HqController::clear_building() {
    building_ = kNoObject;
    if (cstate_ == 2) cstate_ = 0;
    if (screen_ == kScreenPlants && state() != kStateInBuilding) set_screen(kScreenNone); // closes screen 5
}

bool HqController::hero_selected() const {
    for (std::uint32_t u : units_)
        if (type_of(u) == game::kTypeHero) return true;
    return false;
}

bool HqController::all_capturers() const {
    for (std::uint32_t u : units_) {
        const std::uint32_t k = kind_of(u);
        if (k != 1 && k != 2) return false;
    }
    return true;
}

int HqController::classify(const HqPick& pick) const {
    if (view_mode() != 2) return 0;
    const std::uint32_t obj = pick.object;
    const game::WorldObject* o = obj != kNoObject ? world_.live(obj) : nullptr;
    if (o && o->building() && eco_ && eco_->showing(obj)) return 2; // attribute 0x20c: under construction
    if (hero_selected()) return 2;
    if (mode_ == kModePlaceBuilding || mode_ == kModePlaceMine) return 0xd;
    const bool passable = pick.ground && (!host_.passable || host_.passable(pick.ground->x, pick.ground->y));
    if (mode_ == kModeWay) return passable ? 0xc : 2;
    const bool own_obj = o && o->clan == clan_;
    if (cstate_ == 0) return own_obj ? 7 : 0;
    if (cstate_ == 2) {
        if (!own_obj) return 0;
        if (!o->building()) return 7;
        return obj == building_ ? 0x11 : 7;
    }
    switch (mode_) {
    case kModeAttackTarget: return o ? 0x10 : 0;
    case kModeBuilding:
        if (o && o->building() && o->clan != clan_ && o->type != game::kTypeTeleport && o->type != game::kTypeBridge &&
            o->type != game::kTypeRuins)
            return 4;
        return 0;
    case kModeGuardTarget: return o ? 9 : (passable ? 8 : 0);
    default: break;
    }
    if (o && !o->building()) {
        if (!own_obj) return 3;
        if (units_.size() == 1 && units_[0] == obj) return 0x11;
        return 7;
    }
    if (o) {
        if (own_obj) return 10;
        return all_capturers() ? 4 : 3;
    }
    if (pick.ground) return passable ? 1 : 2;
    return 0;
}

// --- commands ---

bool HqController::give(std::uint32_t lid, const game::UnitOrder& o, int mode) {
    game::WorldObject* w = world_.find(lid);
    if (!w || !w->alive || !w->control) return false;
    return w->control->give_order(o, mode);
}

void HqController::ack_voice(std::uint32_t lid) {
    // HqExecuteCommand's acknowledgement: CGame+0x4c pool by unit kind (FUN_1008ecc0: 1/2 → pool 1, 4/5 → 3,
    // else 2), PickRandomSoundFromPool 1008eb10 (no repeat of the last pick). [inferred] the pools are the
    // _S, plain and _B voice sets in initGame's order; the generator's seed is not traced.
    static const char* kNames[5] = {"VOICE_ACKNOWLEDGE", "VOICE_AFFIRMATIVE", "VOICE_OK", "VOICE_YES_SIR", "VOICE_EXECUTE"};
    static std::uint16_t a = 0x1234, b = 0x5678;
    static int last = -1;
    const std::uint32_t k = kind_of(lid);
    const char* suffix = k == 1 || k == 2 ? "_S" : k == 4 || k == 5 ? "_B" : "";
    int idx;
    do {
        a = std::uint16_t(a << 1 ^ b);
        b = std::uint16_t(b >> 1 ^ a);
        idx = b % 5;
    } while (idx == last);
    last = idx;
    if (host_.voice) host_.voice(std::string(kNames[idx]) + suffix, lid);
}

void HqController::execute(int cmd, const std::vector<std::uint32_t>& units, std::uint32_t target, Vec3 point, std::uint32_t param) {
    std::uint32_t last = kNoObject;
    for (std::uint32_t u : units) {
        if (!world_.live(u)) continue;
        last = u;
        game::UnitOrder o;
        switch (cmd) {
        case 1: o = make_order(kStayGround, 0, 0x204); break;
        case 2: o = make_order(kGo, 0, 0x202), o.place = {point.x, point.y, 0}; break;
        case 3: o = make_order(kSearch, 0, 0x203), o.mask = 0x8017365e; break; // "find and capture"
        case 4: o = make_order(kSearch, 0, 0x204), o.target_id = 0xffffffff; break; // "find and destroy"
        case 5: // IssueAttack 10079130: the first target unit, else the first target building
            if (target == kNoObject) continue;
            o = make_order(kAttack, 0, 0x201), o.target_id = target;
            break;
        case 6: // capture this building
            if (target == kNoObject) continue;
            o = make_order(kSearch, 0, 0x201), o.target_id = target;
            break;
        case 7: // IssuePatrol 10079240: around the target, else the point; radius 100 (150 autodemo)
            o = make_order(kPatrol, 100, target != kNoObject ? 0x201 : 0x202);
            o.target_id = target, o.place = {point.x, point.y, 0};
            break;
        case 8: o = make_order(kReload, 0, 0x204); break;
        case 9: continue;
        case 10: o = make_order(kFollow, 50, 0x201), o.target_id = param; break;
        case 0x14: o = make_order(kTransport, 0xffffffff, 0x204); break; // "fill the storage"
        case 0x1e: o = make_order(kSearch, 0, 0x203), o.mask = 0x10001000; break; // mineral search
        case 0x20: { // IssueUpgrade 100793b0: the nearest own building of type `param` CanUpgrade allows
            if (!eco_) continue;
            const Vec3 up = world_.live(u)->position();
            std::uint32_t best = kNoObject;
            float best_d = 0;
            for (const auto& b : world_.objects()) {
                if (!b.alive || b.clan != clan_ || b.type != param || !eco_->can_upgrade(u, b.logic_id)) continue;
                const Vec3 d = b.position() - up;
                const float dd = d.x * d.x + d.y * d.y;
                if (best == kNoObject || dd < best_d) best = b.logic_id, best_d = dd;
            }
            if (best != kNoObject) eco_->order_upgrade(u, best);
            continue;
        }
        default: return;
        }
        give(u, o, 3);
    }
    if (last != kNoObject) ack_voice(last);
    say("command " + std::to_string(cmd) + " to " + std::to_string(units.size()) + " units");
}

bool HqController::building_available(std::uint32_t mask) const {
    // BuildAvailable (1007c000 cases 10–16 + IsBuildingTypeAvailable 100341d0): the first selected unit has
    // energy (property 0x52 > 0, HasNoEnergy 100771f0) [port: always], the clan owns no building of the type,
    // no own builder's first order is BUILD with that param, and the type's level-0 scheme passes the research.
    if (units_.empty() || !eco_) return false;
    for (const auto& o : world_.objects()) {
        if (!o.alive || o.clan != clan_) continue;
        if (o.building() && (o.type & mask) == o.type && o.type != 0) return false;
        if (o.type == game::kTypeBuilder)
            if (auto b = eco_->builder(o.logic_id); b && b->order == kBuild && b->building_type == mask) return false;
    }
    const std::string* scheme = eco_->building_scheme(mask, 0);
    if (!scheme) return false;
    const economy::Research* r = eco_->research(clan_);
    if (!r) return true; // no research tree: nothing to check
    const mission::Scheme* s = eco_->schemes().get(*scheme);
    // FUN_1008b550(name, research) == 0 [inferred]: the root's research item is researched (or absent).
    if (!s) return false;
    const std::int32_t item = r->find_item(s->root.entry);
    return item < 0 || (r->state(item) & economy::kResResearched);
}

bool HqController::menu_available(int cmd) const {
    // COrderMenu_IsAvailable 1007c000.
    switch (cmd) {
    case 2: return !units_.empty() && (kind_of(units_[0]) == 1 || kind_of(units_[0]) == 2);
    case 9: { // some mineral field not found yet [inferred: any lode with ore left]
        if (!eco_) return false;
        for (const auto& l : eco_->lodes())
            if (l.amount > 0 && l.flag == 0) return true;
        return false;
    }
    default: break;
    }
    if (cmd >= 10 && cmd <= 16) return building_available(kBuildMask[cmd - 10]);
    if (cmd >= 17 && cmd <= 23) {
        if (units_.empty() || !eco_) return false;
        const std::uint32_t type = kBuildMask[cmd - 17];
        for (const auto& b : world_.objects())
            if (b.alive && b.clan == clan_ && b.type == type && eco_->can_upgrade(units_[0], b.logic_id)) return true;
        return false;
    }
    return true;
}

std::vector<int> HqController::order_menu() const {
    // COrderMenu_Rebuild 1007aef0: per selected unit, every table entry whose mask covers the unit's type and
    // is available; then sorted and de-duplicated.
    std::vector<int> out;
    for (std::uint32_t u : units_) {
        const std::uint32_t t = type_of(u);
        for (const HqMenuItem& m : kOrderMenu)
            if ((m.mask & t) == t && t != 0 && menu_available(m.cmd)) out.push_back(m.cmd);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

float HqController::menu_y() const { return 0x76 + 20.f * float(screen_units().size()); }

int HqController::menu_rows(std::size_t count) const {
    const int rows = (0x1c6 - int(menu_y())) / 0x15;
    return std::min(int(count), rows);
}

void HqController::menu_execute(int cmd) {
    // COrderMenu_Execute 1007bb90.
    const std::vector<std::uint32_t> sel = units_;
    switch (cmd) {
    case 0: execute(1, sel); return;
    case 1: // pending mode 1 (route): SELECT_WAY, waypoint 0 = the unit's own position
        pending_ = Pending{1, 0, sel, {}, kNoObject, kNoObject};
        for (std::uint32_t u : sel)
            if (const game::WorldObject* o = world_.live(u)) {
                pending_->waypoints.push_back(o->position());
                break;
            }
        mode_ = kModeWay, map_open = false;
        return;
    case 2: execute(3, sel); return;
    case 3: execute(4, sel); return;
    case 6: pending_ = Pending{4, 0, sel, {}, kNoObject, kNoObject}, mode_ = kModeGuardTarget; return;
    case 7: execute(8, sel); return;
    case 8: execute(0x14, sel); return;
    case 9: execute(0x1e, sel); return;
    default: break;
    }
    if (cmd >= 10 && cmd <= 16) {
        pending_ = Pending{3, kBuildMask[cmd - 10], sel, {}, kNoObject, kNoObject};
        begin_placement(kBuildMask[cmd - 10]);
        return;
    }
    if (cmd >= 17 && cmd <= 23) execute(0x20, sel, kNoObject, {}, kBuildMask[cmd - 17]);
}

void HqController::begin_placement(std::uint32_t type) {
    // HqPending_Build phase 0 / Placement_Begin 100583a0: SELECT_PLACE_FM for a mine, else _FB; cursor 8.
    mode_ = type == game::kTypeMine ? kModePlaceMine : kModePlaceBuilding;
    cursor_ = kCurGhost;
    ghost_ = Ghost{type, {}, 0, 0, false};
    say("placement: begin " + std::to_string(type));
}

void HqController::end_placement() {
    if (!ghost_) return;
    ghost_.reset(); // Placement_End 10058360
    cursor_ = kCurPick;
}

void HqController::rotate_ghost(int dir) {
    if (ghost_ && view_mode() == 2) ghost_->angle += 0.05f * float(dir);
}

// --- input ---

void HqController::mouse_move(float vx, float vy) {
    mouse_x = vx, mouse_y = vy;
    // HqInput_OnMouseMove 100718a0: LMB held over 0.35 s in view mode 2 (not SELECT_WAY) → the box cursor.
    if (lbutton_ && view_mode() == 2 && mode_ != kModeWay && now_ - lbutton_since_ > 350) boxing_ = true, cursor_ = kCurBox;
}

void HqController::lbutton_down(float vx, float vy, const HqPick& pick) {
    mouse_x = vx, mouse_y = vy;
    if (designer_->open()) {
        designer_->lbutton_down(vx, vy);
        return;
    }
    if (!interface_up()) return;
    // HqUi_OnLButtonDown 1008db10: outside the resource strip, the interface first, then the world.
    if (!in(vx, vy, 374, 0, 640, 42)) {
        if (click_interface(vx, vy)) return;
        if (over_interface(vx, vy)) return;
        if (view_mode() == 2) click_world(pick);
    }
    lbutton_ = true, lbutton_since_ = now_, box_x0 = vx, box_y0 = vy;
}

void HqController::lbutton_up(float vx, float vy, const HqPick&) {
    mouse_x = vx, mouse_y = vy;
    if (boxing_) {
        // BoxSelectUnits 10076c70: a box over 9×9 px adds every own, alive, not selected non-hero unit whose
        // projected position lies in it.
        const float x0 = std::min(box_x0, vx), x1 = std::max(box_x0, vx), y0 = std::min(box_y0, vy), y1 = std::max(box_y0, vy);
        if (x1 - x0 > 9 && y1 - y0 > 9 && project) {
            bool any = false;
            for (const auto& o : world_.objects()) {
                if (!o.alive || o.clan != clan_ || o.building() || o.type == game::kTypeHero || selected(o.logic_id)) continue;
                const auto p = project(o.position());
                if (!p || p->x < x0 || p->x > x1 || p->y < y0 || p->y > y1) continue;
                clear_building();
                units_.push_back(o.logic_id), cstate_ = 1, mode_ = kModeFree, any = true;
            }
            if (any && host_.voice) host_.voice("VOICE_SELECTED", units_.back());
        }
        cursor_ = kCurArrow;
    }
    lbutton_ = boxing_ = false;
}

void HqController::rbutton_down() {
    // HqUi_OnRButtonDown 1008fd50.
    if (designer_->open()) return;
    if (boxing_) {
        boxing_ = lbutton_ = false, cursor_ = kCurArrow;
        return;
    }
    if (mode_ == kModeWay) {
        if (pending_ && pending_->mode == 1) pending_->route_done = true; // +0x130 on every selected unit
        return;
    }
    if (mode_ == kModePlaceBuilding || mode_ == kModePlaceMine) {
        end_placement();
        cursor_ = kCurArrow, mode_ = kModeFree, pending_.reset();
        if (host_.pager && host_.text) host_.pager(host_.text(1541), host_.text(6207)); // "Постройка здания отменена игроком"
        return;
    }
    if (cstate_ == 1 && view_mode() == 2) {
        clear_units();
        if (screen_ >= kScreenWarbots && screen_ <= kScreenBuilders) set_screen(kScreenNone);
        return;
    }
    if (cstate_ == 2) {
        clear_building();
        if (screen_ >= kScreenResearch) set_screen(kScreenNone);
        if (state() == kStateInBuilding) pop_state();
        return;
    }
    if (view_mode() == 2 && screen_ != kScreenNone) {
        set_screen(kScreenNone);
        return;
    }
    map_open = false;
}

bool HqController::hotkey(char letter) {
    // HqInput_OnHotkey 10071bb0 (states 3/4): W warbots, C transports, B builders, R laboratory, P plants;
    // each expands the sidebar first.
    if (designer_->open() || (state() != kStateHqBunker && state() != kStateHqUnit)) return false;
    int s = 0;
    switch (letter) {
    case 'W': s = kScreenWarbots; break;
    case 'C': s = kScreenTransports; break;
    case 'B': s = kScreenBuilders; break;
    case 'R': s = kScreenResearch; break;
    case 'P': s = kScreenPlants; break;
    default: return false;
    }
    expand_now();
    int button = -1;
    for (int i = 0; i < 12; ++i)
        if (kSidebar[i].screen == s) button = i;
    if (button >= 0 && sidebar_enabled(button)) set_screen(s);
    return true;
}

bool HqController::escape() {
    // OnGameKeyDown 10071240: placement → cancel; view mode 2: the satellite map, else the open screen.
    // [inferred] With nothing open, ROLLBACK_STATE (the same key) pops the view state.
    if (state() == kStateHero) return false;
    if (state() == kStateMenu) { // ROLLBACK_STATE pops the menu
        pop_state();
        return true;
    }
    if (designer_->open()) {
        designer_->key_escape();
        return true;
    }
    if (mode_ == kModePlaceBuilding || mode_ == kModePlaceMine) {
        rbutton_down();
        return true;
    }
    if (view_mode() == 2 && map_open) {
        map_open = false;
        return true;
    }
    if (screen_ != kScreenNone) {
        set_screen(kScreenNone);
        if (state() == kStateInBuilding) pop_state();
        return true;
    }
    pop_state();
    return true;
}

// --- screens ---

void HqController::expand_now() {
    items_ = 16, expanded_ = true, animating_ = false, lock_y_ = 457;
}

bool HqController::sidebar_enabled(int i) const {
    // HqUi_Update 10084040: the buttons' enabled flags.
    auto has_unit = [&](std::uint32_t mask) {
        for (const auto& o : world_.objects())
            if (o.alive && o.clan == clan_ && !o.building() && (o.type & mask) == o.type && o.type) return true;
        return false;
    };
    auto has_building = [&](std::uint32_t mask, bool finished) {
        for (const auto& o : world_.objects())
            if (o.alive && o.clan == clan_ && o.building() && (o.type & mask) == o.type && o.type &&
                (!finished || !eco_ || !eco_->showing(o.logic_id)))
                return true;
        return false;
    };
    switch (i) {
    case 0: return true;
    case 1: return has_unit(0x1018000);
    case 2: return has_unit(0x1004000);
    case 3: return has_unit(0x1002000);
    case 4: return has_building(0x80000400, true);
    case 5: return has_building(0x80000010, true);
    case 6: return has_building(0x80300000, true);
    case 7: return has_building(0x80070000, true);
    case 8: return has_building(0x8000044e, false);
    case 9: return false; // the chat terminal: multiplayer only
    default: return true;
    }
}

bool HqController::sidebar_selected(int i) const {
    if (i == 10) return map_open;
    if (i == 9 || i == 11) return false; // terminal / menu not open
    return kSidebar[i].screen != 0 && kSidebar[i].screen == screen_;
}

int HqController::hovered_button() const {
    if (!expanded_ || state() == kStateInBuilding) return -1;
    for (int i = 0; i < 12; ++i)
        if (in(mouse_x, mouse_y, 0, kSidebar[i].y, 46, kSidebar[i].y + 29)) return i;
    return -1;
}

std::vector<std::uint32_t> HqController::screen_units() const {
    std::vector<std::uint32_t> out;
    if (screen_ < kScreenWarbots || screen_ > kScreenBuilders) return out;
    const std::uint32_t mask = screen_mask(screen_);
    for (const auto& o : world_.objects())
        if (o.alive && o.clan == clan_ && !o.building() && o.type && (o.type & mask) == o.type) out.push_back(o.logic_id);
    return out;
}

std::vector<std::uint32_t> HqController::screen_buildings() const {
    std::vector<std::uint32_t> out;
    if (screen_ < kScreenPlants) return out;
    const std::uint32_t mask = screen_mask(screen_);
    for (const auto& o : world_.objects())
        if (o.alive && o.clan == clan_ && o.building() && o.type && (o.type & mask) == o.type && !(eco_ && eco_->showing(o.logic_id)))
            out.push_back(o.logic_id);
    return out;
}

void HqController::open_unit_screen(int s) {
    // HqUi_SetUnitScreen 10085240 for units: keep a single selected unit of the type, else select the first.
    const std::uint32_t mask = screen_mask(s);
    const bool keep = units_.size() == 1 && (type_of(units_[0]) & mask) == type_of(units_[0]);
    if (!keep) {
        clear_units();
        for (const auto& o : world_.objects())
            if (o.alive && o.clan == clan_ && !o.building() && o.type && (o.type & mask) == o.type) {
                select_unit(o.logic_id, true);
                break;
            }
    }
    menu_scroll = 0;
}

void HqController::set_screen(int s) {
    screen_ = s;
    if (s >= kScreenWarbots && s <= kScreenBuilders) {
        open_unit_screen(s);
    } else if (s == kScreenResearch) {
        const auto list = research_list(); // FUN_1008a3e0
        if (research_scroll >= int(list.size())) research_scroll = 0;
        if (!list.empty() && std::find(list.begin(), list.end(), research_selected) == list.end())
            research_selected = list[std::size_t(research_scroll)];
    } else if (s >= kScreenPlants) {
        // The building screens: keep a single selected building of the type, else select the first.
        const std::uint32_t mask = screen_mask(s);
        const std::uint32_t t = type_of(building_);
        if (building_ == kNoObject || (t & mask) != t || t == 0) {
            const std::uint32_t keep_plant = plant_;
            clear_building();
            screen_ = s;
            for (const auto& o : world_.objects())
                if (o.alive && o.clan == clan_ && o.building() && o.type && (o.type & mask) == o.type) {
                    select_building(o.logic_id);
                    break;
                }
            if (s == kScreenPlants && building_ == kNoObject) plant_ = keep_plant;
        }
    }
    say("screen " + std::to_string(s));
}

bool HqController::over_interface(float vx, float vy) const {
    // HqUi_HitTest 10084f60: the sidebar column, the screen panel and its rows, the order menu, the
    // factory / research screens; plus the resource strip (ClassifyPointer).
    if (in(vx, vy, 374, 0, 640, 42)) return true;
    if (state() != kStateInBuilding && in(vx, vy, 0, 0, 46, 480)) return true;
    switch (screen_) {
    case kScreenWarbots: case kScreenTransports: case kScreenBuilders: {
        const std::size_t rows = screen_units().size();
        if (in(vx, vy, 51, 0, 369, 118 + 20.f * float(rows))) return true;
        const std::size_t n = order_menu().size();
        return in(vx, vy, menu_x(), menu_y(), menu_x() + 269, menu_y() + 20 + 20.f * float(menu_rows(n)));
    }
    case kScreenResearch: return vx > 50 && vx < 369 && vy >= 0 && vy < 177 + 20.f * float(std::min<std::size_t>(12, research_list().size()));
    case kScreenPlants: return in(vx, vy, 51, 0, 369, 21 + 164) || in(vx, vy, 51, 171, 369, 171 + 20.f * float(screen_buildings().size()));
    case kScreenTowers: case kScreenBunkers: case kScreenOther:
        return in(vx, vy, 51, 0, 369, 20 + 20.f * float(screen_buildings().size()));
    default: return false;
    }
}

std::uint32_t HqController::tooltip_at(float vx, float vy) const {
    if (!interface_up()) return 0;
    if (designer_->open()) return designer_->tooltip_at(vx, vy);
    std::uint32_t tip = 0;
    // Sidebar buttons (CSidebarButton_DrawAndHover 1009c320), drawn when not in state 5 and fully expanded.
    if (state() != kStateInBuilding)
        for (int k = 0; k < items_; ++k)
            if (const int i = kSidebarItem[k]; i >= 0 && in(vx, vy, 0, kSidebar[i].y, 46, kSidebar[i].y + 29)) tip = kSidebar[i].tooltip;
    if (state() != kStateInBuilding && !sidebar_expanded()) return tip;
    if (screen_ != kScreenNone && in(vx, vy, 334, 0, 369, 20)) tip = 6169; // HqUi_DrawTitle's close box
    switch (screen_) {
    case kScreenWarbots: case kScreenTransports: case kScreenBuilders:
        if (units_.size() == 1) { // HqUi_DrawUnitPanel 10085c70: the icon rects ±2
            const float x = 51, y = 20;
            if (in(vx, vy, x + 10, y + 69, x + 29, y + 88)) tip = 1502;
            else if (in(vx, vy, x + 34, y + 69, x + 53, y + 88)) tip = 1519;
            else if (in(vx, vy, x + 58, y + 69, x + 77, y + 88)) tip = 1518;
            else if (in(vx, vy, x + 275, y + 70, x + 303, y + 87)) tip = 6244;
            if (type_of(units_[0]) == game::kTypeHq && in(vx, vy, x + 88, y + 69, x + 107, y + 88)) tip = 1517;
        }
        break;
    case kScreenResearch: {
        if (!research_list().empty() && in(vx, vy, 170, 127, 202, 148)) tip = 6243; // FUN_10089090
        const auto list = research_list();
        for (int k = 0; k < 12 && research_scroll + k < int(list.size()); ++k) // FUN_10088a00: the toggle button
            if (in(vx, vy, 56, 179 + 20.f * float(k), 91, 198 + 20.f * float(k)))
                tip = researching(list[std::size_t(research_scroll + k)]) ? 6252 : 6251;
        break;
    }
    case kScreenPlants:
        if (const HqPlant* p = current_plant()) { // CPlantPanel_Draw 10097430 (ctor 100965f0 stores +0x8f0..+0x914)
            const float x = 51, y = 21, x1 = x + 318, by = y + 129;
            const bool start = can_start(*p);
            if (in(vx, vy, x + 5, by, x + 55, by + 19)) tip = 1511;
            else if (in(vx, vy, x + 262, by, x + 312, by + 19)) tip = start ? 1553 : 3070;
            else if (in(vx, vy, x + 212, by, x + 262, by + 19)) tip = start ? 6238 : 6239;
            if (in(vx, vy, x1 - 45, y + 106, x1 - 30, y + 121)) tip = 3067;
            const auto info = eco_ ? eco_->plant(p->lid) : std::nullopt;
            if (in(vx, vy, x + 0xe4, y + 0x6a, x + 0x102, y + 0x79)) tip = info && info->free_bots > 0 ? 6248 : 6249;
            if (!start && in(vx, vy, x + 0x87, y + 0x68, x + 0xa3, y + 0x79)) tip = 3069;
            for (int i = 0; i < int(projects_.size()); ++i)
                if (const float sx = x + 8 + 23.f * float(i); in(vx, vy, sx + 2, y + 0x68, sx + 0x15, y + 0x79)) tip = 3068;
        }
        [[fallthrough]];
    case kScreenTowers: case kScreenBunkers: case kScreenOther: {
        if (screen_ == kScreenPlants && state() == kStateInBuilding) break;
        const auto rows = screen_buildings(); // CBuildingRow_Draw 10096190: the two row buttons
        const float y0 = screen_ == kScreenPlants ? 171.f : 20.f;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const float y = y0 + 20.f * float(r), bx = 51 + 10 + 19 + 5;
            const std::uint32_t t = type_of(rows[r]);
            if (is_bunker(t) && in(vx, vy, bx, y, bx + 35, y + 19)) tip = 1517;
            else if ((is_bunker(t) || is_tower(t)) && in(vx, vy, bx + (is_bunker(t) ? 35.f : 0.f), y, bx + (is_bunker(t) ? 70.f : 35.f), y + 19))
                tip = 1502;
        }
        break;
    }
    default: break;
    }
    return tip;
}

bool HqController::click_interface(float vx, float vy) {
    // HqUi_Click 10084580.
    if (state() != kStateInBuilding) {
        // The lock while expanded and idle: collapse.
        if (in(vx, vy, 0, lock_y_, 46, lock_y_ + 24) && !animating_ && expanded_) {
            animating_ = true, items_t_ = now_;
            if (host_.sound) host_.sound("BAR_OPEN");
            return true;
        }
    }
    if (expanded_ && (state() == kStateHqBunker || state() == kStateHqUnit)) {
        for (int i = 0; i < 12; ++i) {
            if (!in(vx, vy, 0, kSidebar[i].y, 46, kSidebar[i].y + 29) || !sidebar_enabled(i)) continue;
            if (i == 0) pop_to_hero();
            else if (kSidebar[i].screen) set_screen(sidebar_selected(i) ? kScreenNone : kSidebar[i].screen);
            else if (i == 9) say("chat terminal (not ported)");
            else if (i == 10) map_open = !map_open;
            else if (i == 11) open_menu();
            return true;
        }
    }
    // The screen's close box (HqUi_DrawTitle: x 334..369, y 0..20); in state 5 it leaves the building.
    if (screen_ != kScreenNone && in(vx, vy, 334, 0, 369, 20)) {
        set_screen(kScreenNone);
        if (state() == kStateInBuilding) pop_state();
        return true;
    }
    switch (screen_) {
    case kScreenWarbots: case kScreenTransports: case kScreenBuilders: {
        if (units_.size() >= 1) {
            const std::uint32_t u = units_.front();
            const float x = 51, y = 20;
            // HqUi_DrawUnitPanel's button rects (the icons ±2).
            const bool b1 = in(vx, vy, x + 10, y + 69, x + 29, y + 88), b2 = in(vx, vy, x + 34, y + 69, x + 53, y + 88),
                       b3 = in(vx, vy, x + 58, y + 69, x + 77, y + 88);
            if (b1 || b2 || b3) {
                // +0x9c = 0 manual / 1 autogunner / 2 autopilot, then Push(2, 0, unit) (not while upgrading).
                if (world_.live(u) && world_.live(u)->current_order() != kUpgrade) {
                    drive_mode[u] = b1 ? 0 : b2 ? 1 : 2;
                    push_state(kStateRemote, u);
                }
                return true;
            }
            if (type_of(u) == game::kTypeHq && in(vx, vy, x + 88, y + 69, x + 107, y + 88)) {
                push_state(kStateHqUnit, u); // "Стратегическое управление": Push(3, 0, unit)
                return true;
            }
            if (in(vx, vy, x + 275, y + 70, x + 303, y + 87)) {
                self_destruct.insert(u); // ArmSelfDestruct 100763f0 [the explosion is not ported]
                return true;
            }
        }
        const auto rows = screen_units();
        for (std::size_t r = 0; r < rows.size(); ++r)
            if (in(vx, vy, 51, 118 + 20.f * float(r), 368, 137 + 20.f * float(r))) {
                select_unit(rows[r], true); // CListRow_OnClick 10086480
                menu_scroll = 0;
                return true;
            }
        // COrderMenu_Click 1007b960: the header's scroll buttons, then the rows.
        const auto menu = order_menu();
        const float mx = menu_x(), my = menu_y();
        if (in(vx, vy, mx + 163, my, mx + 213, my + 19)) {
            menu_scroll = std::max(0, menu_scroll - 1);
            return true;
        }
        if (in(vx, vy, mx + 213, my, mx + 263, my + 19)) {
            if (menu_scroll + menu_rows(menu.size()) < int(menu.size())) ++menu_scroll;
            return true;
        }
        const int rows_n = menu_rows(menu.size());
        for (int k = 0; k < rows_n; ++k)
            if (in(vx, vy, mx, my + 20 + 20.f * float(k), mx + 269, my + 40 + 20.f * float(k))) {
                const std::size_t i = std::size_t(menu_scroll + k);
                if (i < menu.size()) {
                    if (host_.sound) host_.sound("BUTTON_CLICK");
                    menu_execute(menu[i]);
                }
                return true;
            }
        return false;
    }
    case kScreenResearch: click_research(vx, vy); return over_interface(vx, vy);
    case kScreenPlants: {
        if (state() != kStateInBuilding) {
            const auto rows = screen_buildings();
            for (std::size_t r = 0; r < rows.size(); ++r)
                if (in(vx, vy, 51, 171 + 20.f * float(r), 368, 190 + 20.f * float(r))) {
                    select_building(rows[r]);
                    return true;
                }
        }
        click_factory(vx, vy);
        return in(vx, vy, 51, 21, 51 + 318, 21 + 164);
    }
    case kScreenTowers: case kScreenBunkers: case kScreenOther: {
        const auto rows = screen_buildings();
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const float y = 20 + 20.f * float(r);
            if (!in(vx, vy, 51, y, 368, y + 19)) continue;
            // CListRow_OnClick 10086480: select; button 0 (bunkers) → Push(4, bunker); button 1 → Push(6).
            select_building(rows[r]);
            const std::uint32_t t = type_of(rows[r]);
            const float bx = 51 + 10 + 19 + 5;
            if (is_bunker(t) && in(vx, vy, bx, y, bx + 35, y + 19) && (state() == kStateHqBunker || state() == kStateHqUnit))
                push_state(kStateHqBunker, rows[r]);
            else if ((is_bunker(t) || is_tower(t)) && in(vx, vy, bx + (is_bunker(t) ? 35 : 0), y, bx + (is_bunker(t) ? 70 : 35), y + 19))
                push_state(kStateTurret, rows[r]);
            return true;
        }
        return false;
    }
    default: return false;
    }
}

void HqController::click_world(const HqPick& pick) {
    // HqUi_ClickWorld 100900d0.
    const int code = classify(pick);
    const std::vector<std::uint32_t> sel = units_;
    const std::uint32_t obj = pick.object;
    const game::WorldObject* o = obj != kNoObject ? world_.live(obj) : nullptr;
    const Vec3 p = pick.ground ? *pick.ground : Vec3{};
    switch (code) {
    case 1: execute(2, sel, kNoObject, p); break;
    case 3: execute(5, sel, obj); break;
    case 4:
        if (mode_ == kModeBuilding) {
            if (pending_) pending_->building_flag = true, pending_->target_building = obj;
            mode_ = kModeFree;
        } else {
            execute(6, sel, obj);
        }
        break;
    case 7:
        if (o && !o->building()) {
            clear_building();
            select_unit(obj, true);
            if (screen_ >= kScreenWarbots && screen_ <= kScreenBuilders) {
                const std::uint32_t t = o->type;
                set_screen(t == game::kTypeTransport ? kScreenTransports : t == game::kTypeBuilder ? kScreenBuilders : kScreenWarbots);
            }
        } else if (o) {
            clear_units();
            select_building(obj);
        }
        break;
    case 8:
        if (pending_) pending_->waypoints.push_back(p), pending_->placed = true;
        mode_ = kModeFree;
        break;
    case 9:
        if (mode_ == kModeGuardTarget) {
            if (pending_) pending_->unit_flag = true, pending_->target_unit = obj;
            mode_ = kModeFree;
        } else {
            execute(7, sel, obj);
        }
        break;
    case 10:
        if (mode_ == kModeGuardTarget) {
            if (pending_) pending_->building_flag = true, pending_->target_building = obj;
            mode_ = kModeFree;
        } else {
            execute(7, sel, obj);
        }
        break;
    case 0xc:
        if (pending_) pending_->waypoints.push_back(p);
        break;
    case 0xd:
        if (ghost_ && ghost_->validity != 0 && pending_) {
            pending_->placed = true;
            mode_ = kModeFree;
        }
        break;
    case 0x10:
        if (pending_) {
            if (o && o->building()) pending_->building_flag = true, pending_->target_building = obj;
            else pending_->unit_flag = true, pending_->target_unit = obj;
        }
        mode_ = kModeFree;
        break;
    case 0x11:
        if (cstate_ == 1 && o) {
            expand_now();
            const std::uint32_t t = o->type;
            set_screen(t == game::kTypeTransport ? kScreenTransports : t == game::kTypeBuilder ? kScreenBuilders : kScreenWarbots);
        } else if (cstate_ == 2 && o) {
            const std::uint32_t t = o->type;
            set_screen(t == game::kTypeInstitute ? kScreenResearch : t == game::kTypePlant ? kScreenPlants
                       : is_tower(t)               ? kScreenTowers
                       : is_bunker(t)              ? kScreenBunkers
                                                   : kScreenOther);
        }
        break;
    default: break;
    }
}

// --- research (§8) ---

std::vector<std::int32_t> HqController::research_list() const {
    std::vector<std::int32_t> out;
    const economy::Research* r = eco_ ? eco_->research(clan_) : nullptr;
    if (!r) return out;
    for (std::int32_t i = 0; i < std::int32_t(r->item_count()) && out.size() < 50; ++i) {
        const auto info = r->info(i);
        if (info && !info->researched && info->present && info->available && (info->research_power != 0 || info->research_ore != 0))
            out.push_back(i);
    }
    return out;
}

bool HqController::researching(std::int32_t item) const {
    if (!eco_) return false;
    const auto orders = eco_->research_orders(clan_);
    return std::find(orders.begin(), orders.end(), item) != orders.end();
}

bool HqController::start_research(std::int32_t item) {
    // StartResearch 100884c0 through Economy::order_research (the lab with the fewest orders, insert mode 1).
    if (!eco_ || researching(item) || !eco_->order_research(clan_, item)) return false;
    say("research: start " + std::to_string(item));
    return true;
}

void HqController::stop_research(std::int32_t item) {
    // The row's stop: the lab's type-0xe order with that param goes (control slot 0xa0(orderIndex)).
    if (!eco_) return;
    for (const auto& o : world_.objects())
        if (o.alive && o.clan == clan_ && o.type == game::kTypeInstitute && eco_->cancel_order(o.logic_id, kResearch, std::uint32_t(item))) {
            say("research: stop " + std::to_string(item));
            return;
        }
}

void HqController::click_research(float vx, float vy) {
    // 1008a0f0.
    const auto list = research_list();
    const int count = int(list.size());
    if (count > 0 && in(vx, vy, 170, 127, 202, 148)) { // the batch button: stop everything, start every item
        // 10088320: every type-0xe order leaves every lab.
        for (std::int32_t queued : eco_->research_orders(clan_)) stop_research(queued);
        for (std::int32_t i : list) start_research(i);
        research_batch_flash = now_;
        return;
    }
    if (in(vx, vy, 264, 159, 314, 178)) {
        if (research_scroll > 0) --research_scroll;
        return;
    }
    if (in(vx, vy, 314, 159, 364, 178)) {
        if (research_scroll + 12 < count) ++research_scroll;
        return;
    }
    for (int k = 0; k < 12; ++k) {
        const int i = research_scroll + k;
        if (i >= count) break;
        const float y = 179 + 20.f * float(k);
        if (!in(vx, vy, 51, y, 369, y + 19)) continue;
        if (in(vx, vy, 56, y, 91, y + 19)) {
            if (researching(list[std::size_t(i)])) stop_research(list[std::size_t(i)]);
            else start_research(list[std::size_t(i)]);
        }
        research_selected = list[std::size_t(i)];
        return;
    }
}

// --- factory (§9) ---

void HqController::rebuild_plants() {
    // PlantList: the player's plants (BindInterfaces / OnObjectCaptured add, removal erases).
    std::vector<HqPlant> next;
    for (const auto& o : world_.objects()) {
        if (!o.alive || o.clan != clan_ || o.type != game::kTypePlant) continue;
        auto it = std::find_if(plants_.begin(), plants_.end(), [&](const HqPlant& p) { return p.lid == o.logic_id; });
        HqPlant fresh;
        fresh.lid = o.logic_id;
        next.push_back(it != plants_.end() ? *it : fresh);
    }
    plants_ = std::move(next);
}

HqPlant* HqController::current_plant() {
    for (auto& p : plants_)
        if (p.lid == plant_) return &p;
    return nullptr;
}
const HqPlant* HqController::current_plant() const { return const_cast<HqController*>(this)->current_plant(); }

void HqController::add_project(HqProject p) {
    projects_.insert(projects_.begin(), std::move(p)); // the slots shift down, the new one at 0
    if (projects_.size() > 5) projects_.resize(5);
}

bool HqController::can_start(const HqPlant& p) const {
    const auto info = eco_ ? eco_->plant(p.lid) : std::nullopt;
    return !(info && info->busy) && !p.batch;
}

std::string HqController::bld_path(std::uint32_t plant) const {
    // "%s\units\bld_unit_%d.dat" (10104 66c) with the current folder; the port keeps it in the scheme cache.
    return "UNITS\\bld_unit_" + std::to_string(std::int32_t(plant)) + ".dat";
}

const mission::Scheme* HqController::preview_scheme() const {
    const HqPlant* p = current_plant();
    if (!p) return nullptr;
    if (p->project >= 0 && p->project < int(projects_.size())) return &projects_[std::size_t(p->project)].scheme;
    if (eco_ && !can_start(*p)) {
        const auto info = eco_->plant(p->lid);
        if (info && !info->scheme.empty()) return eco_->schemes().get(info->scheme);
    }
    return nullptr;
}

std::uint32_t HqController::free_brains() const { return eco_ ? eco_->free_brains(clan_) : 0; }

bool HqController::select_project(int i) {
    // SelectProject 10098240: every part must be in the research catalogue as researched and present; the
    // preview (view_unit_<LID>.dat) is always made, bld_unit_<LID>.dat only when the plant can start.
    HqPlant* p = current_plant();
    if (!p || i < 0 || i >= int(projects_.size()) || !eco_) return false;
    if (const economy::Research* r = eco_->research(clan_); r && !r->build_cost(projects_[std::size_t(i)].scheme).ok) {
        say("factory: project " + std::to_string(i) + " has parts not researched");
        return false;
    }
    if (can_start(*p)) eco_->schemes().put(bld_path(p->lid), projects_[std::size_t(i)].scheme);
    p->project = i;
    return true;
}

void HqController::issue_build(bool batch) {
    // 10098680 → PlantRec_IssueBuildOrder 100873b0: only with a free brain; {0xc, header mask, 0x205 path}, mode 3.
    HqPlant* p = current_plant();
    if (!p || !eco_ || free_brains() == 0 || p->project < 0) return;
    const std::string path = bld_path(p->lid);
    if (!eco_->schemes().get(path)) eco_->schemes().put(path, projects_[std::size_t(p->project)].scheme);
    if (eco_->order_construct(p->lid, path, 3)) {
        p->path = path, p->batch = batch;
        say("factory: build " + projects_[std::size_t(p->project)].file + (batch ? " (batch)" : ""));
    }
}

void HqController::stop_build(bool batch) {
    // 100986c0: only for the matching mode (build button: not batch; batch button: batch). Stop (10087680):
    // a running 0xc task is cancelled (control slot 0xa0(0)), the path cleared, batch off; no project → 0.
    HqPlant* p = current_plant();
    if (!p || p->batch != batch) return;
    if (eco_ && eco_->current_order(p->lid) == kConstruct) eco_->cancel(p->lid);
    p->path.clear(), p->batch = false;
    if (p->project == -1) select_project(0);
    say("factory: stop");
}

void HqController::click_factory(float vx, float vy) {
    // 10097fe0 with the rects of 10097430 at (51, 21).
    HqPlant* p = current_plant();
    if (!p) return;
    const float x = 51, y = 21, by = y + 129;
    const bool start = can_start(*p);
    if (in(vx, vy, x + 5, by, x + 55, by + 19)) { // constructor (1511)
        open_designer();
        return;
    }
    if (in(vx, vy, x + 262, by, x + 312, by + 19)) { // build / stop
        if (p->project < 0 && start) return;
        if (start) issue_build(false);
        else stop_build(false);
        return;
    }
    if (in(vx, vy, x + 212, by, x + 262, by + 19)) { // batch / stop batch
        if (p->project < 0 && start) return;
        if (start) issue_build(true);
        else stop_build(true);
        return;
    }
    if (!start && in(vx, vy, x + 135, y + 104, x + 163, y + 121)) { // the current production
        p->project = -1;
        return;
    }
    for (int i = 0; i < int(projects_.size()); ++i) {
        const float sx = x + 8 + 23.f * float(i);
        if (in(vx, vy, sx + 2, y + 104, sx + 21, y + 121)) {
            select_project(i);
            return;
        }
    }
}

// --- per frame ---

void HqController::update(double now_ms, const HqPick& pick) {
    now_ = now_ms;
    designer_->update(now_ms);
    // HqCam_FollowTarget reads the followed unit's placement every frame (state 3).
    if (camera.follow_pos && state() == kStateHqUnit)
        if (const game::WorldObject* u = world_.live(state_object())) camera.follow_pos = u->position();
    rebuild_plants();
    number_new_units();
    // Selections drop dead objects (1007de10 on unit removal).
    std::erase_if(units_, [&](std::uint32_t u) { return !own_alive(u); });
    if (units_.empty() && cstate_ == 1) cstate_ = 0;
    if (building_ != kNoObject && !own_alive(building_)) clear_building();
    // PlantList_Update 100878c0 (every MainLoop pass): an idle batch plant with a free brain gets its order again.
    if (eco_)
        for (auto& p : plants_) {
            const auto info = eco_->plant(p.lid);
            if (p.batch && info && !info->busy && free_brains() > 0 && !p.path.empty()) eco_->order_construct(p.lid, p.path, 3);
        }
    if (!interface_up()) return;

    // HqUi_Update 10084040: the sidebar (not in state 5): hovering the lock while collapsed starts expanding;
    // one item per 0.05 s; at 16 the lock sits at 457, at 0 at 12.
    if (state() != kStateInBuilding) {
        if (in(mouse_x, mouse_y, 0, lock_y_, 46, lock_y_ + 24) && !animating_ && !expanded_) {
            animating_ = true, items_t_ = now_;
            if (host_.sound) host_.sound("BAR_OPEN");
        }
        if (animating_ && now_ - items_t_ > 50) {
            items_t_ = now_;
            items_ += expanded_ ? -1 : 1;
            if (items_ <= 0) items_ = 0, animating_ = false, expanded_ = false, lock_y_ = 12;
            else if (items_ >= 16) items_ = 16, animating_ = false, expanded_ = true, lock_y_ = 457;
        }
        const int h = hovered_button();
        if (h >= 0 && now_ - hover_t_ > 100) hover_t_ = now_, hover_blink = !hover_blink;
        // A screen whose button went disabled closes.
        for (int i = 1; i < 9; ++i)
            if (kSidebar[i].screen == screen_ && !sidebar_enabled(i)) set_screen(kScreenNone);
    }
    if (screen_ >= kScreenWarbots && screen_ <= kScreenBuilders && screen_units().empty()) set_screen(kScreenNone);
    // FUN_10097430: a plant without a preview selects its project (or the first) when the list has any.
    if (screen_ == kScreenPlants)
        if (HqPlant* p = current_plant(); p && p->project < 0 && can_start(*p) && !projects_.empty()) select_project(0);

    // The cursor (UpdateHqCursor; not while box-selecting or placing).
    if (cursor_ != kCurBox && cursor_ != kCurGhost) cursor_ = over_interface(mouse_x, mouse_y) ? kCurArrow : hq_cursor_for(classify(pick));

    // Placement_Update 100584c0: the ghost at the terrain point; validity from the placement query
    // (0 red, 1 green); a mine also needs a mineral field within 20 m (MineralFieldAt 10073350).
    if (ghost_) {
        ghost_->on_ground = pick.ground.has_value();
        if (pick.ground) {
            ghost_->pos = *pick.ground;
            bool ok = eco_ && eco_->placement_ok(ghost_->type, ghost_->pos);
            if (ok && mode_ == kModePlaceMine) {
                ok = false;
                for (const auto& l : eco_->lodes()) {
                    const float dx = l.pos.x - std::trunc(ghost_->pos.x), dy = l.pos.y - std::trunc(ghost_->pos.y);
                    if (std::sqrt(dx * dx + dy * dy) < 20.f && l.amount != 0) ok = true;
                }
            }
            ghost_->validity = ok ? 1 : 0;
        } else {
            ghost_->validity = 0;
        }
    }

    // HqPending_Tick 10079b50: the pending command completes into its order, or ends silently when the
    // mode went back to 0 without its flag.
    if (pending_) {
        Pending& pd = *pending_;
        std::vector<std::uint32_t> alive;
        for (std::uint32_t u : pd.units)
            if (own_alive(u)) alive.push_back(u);
        switch (pd.mode) {
        case 1: // route
            if (pd.route_done) {
                for (std::uint32_t u : alive) {
                    bool first = true;
                    for (std::size_t k = 1; k < pd.waypoints.size(); ++k) {
                        game::UnitOrder o = make_order(kGo, 0, 0x202);
                        o.place = {pd.waypoints[k].x, pd.waypoints[k].y, 0};
                        give(u, o, first ? 3 : 1);
                        first = false;
                    }
                }
                if (!alive.empty()) ack_voice(alive.back());
                mode_ = kModeFree;
                pending_.reset();
            } else if (mode_ != kModeWay) {
                pending_.reset();
            }
            break;
        case 3: // build: BUILD {7, mask, 0x206 placement matrix}, insert 3
            if (pd.placed && ghost_) {
                for (std::uint32_t u : alive) eco_->order_build(u, pd.param, ghost_->pos, ghost_->angle, 3);
                if (!alive.empty()) ack_voice(alive.back());
                end_placement();
                pending_.reset();
            } else if (mode_ != kModePlaceBuilding && mode_ != kModePlaceMine) {
                end_placement();
                pending_.reset();
            }
            break;
        case 4: // patrol: PATROL {4, 300, 0x201 LID | 0x202 the first waypoint}
            if (mode_ == kModeFree) {
                if (pd.unit_flag || pd.building_flag || pd.placed) {
                    for (std::uint32_t u : alive) {
                        game::UnitOrder o = make_order(kPatrol, 300, 0x202);
                        if (pd.unit_flag || pd.building_flag) o.target_kind = 0x201, o.target_id = pd.unit_flag ? pd.target_unit : pd.target_building;
                        else if (!pd.waypoints.empty()) o.place = {pd.waypoints.front().x, pd.waypoints.front().y, 0};
                        give(u, o, 3);
                    }
                    if (!alive.empty()) ack_voice(alive.back());
                }
                pending_.reset();
            }
            break;
        default: pending_.reset(); break;
        }
    }

    // MainLoop: VOICE_NO_CPU once with the factory screen up, no plant busy for 1 s and no free brain.
    if (screen_ == kScreenPlants && eco_) {
        bool busy = false;
        for (const auto& p : plants_)
            if (auto info = eco_->plant(p.lid); info && info->busy) busy = true;
        if (busy) plants_busy_ms_ = now_;
        if (!busy && now_ - plants_busy_ms_ > 1000 && free_brains() == 0 && !no_cpu_played_) {
            no_cpu_played_ = true;
            if (host_.voice) host_.voice("VOICE_NO_CPU", kNoObject);
        }
    }
    // CResourceStrip_UpdateValues 1006dd80: the shown values step ±1 toward the targets every 0.05 s; a zero
    // value blinks every 0.5 s.
    if (eco_ && now_ - strip_t_ >= 50) {
        strip_t_ = now_;
        const int m = int(eco_->minerals_percent(clan_)), e = int(eco_->energy_percent(clan_));
        minerals_shown += minerals_shown < m ? 1 : minerals_shown > m ? -1 : 0;
        energy_shown += energy_shown < e ? 1 : energy_shown > e ? -1 : 0;
    }
    if ((minerals_shown == 0 || energy_shown == 0) && now_ - blink_t_ > 500) blink_t_ = now_, strip_blink = !strip_blink;
}

} // namespace parkan::ui
