#include "ai/ai_unit.hpp"

#include <cmath>
#include <memory>

namespace parkan::ai {

namespace {
// Copies of the returned source draw from one generator.
RandomSource share(RandomSource rand) {
    auto p = std::make_shared<RandomSource>(std::move(rand));
    return [p] { return (*p)(); };
}
} // namespace

AiUnit::AiUnit(UnitMotion& motion, const LandMap& map, const ArealGraph& graph, RandomSource rand, AiUnitConfig cfg)
    : motion_(&motion), rand_(share(std::move(rand))), walker_(map, graph, rand_), brain_(walker_, rand_) {
    walker_.can_fly = cfg.can_fly;
    walker_.walk_chassis = cfg.walk_chassis;
    walker_.animal = cfg.animal;
    walker_.unit_state = [this] { return state(); };
    // Control's Update loop: IWizard::Takt(round(clock)) before SelectNextMotion (class 3, a building,
    // has no such call). The placement is the pose at the motion clock (assumed for
    // GetPlacement(2)); the look-ahead is the duration of the step that just ended (ctl+0xf0).
    motion.pilot = [this](UnitMotion& m, float clock) {
        if (auto out = wizard_.takt(std::uint32_t(std::lround(clock)), m.step_duration(), m.end_matrix(), m.body)) {
            m.set_linear_velocity(out->linear);
            m.set_angular_command({0, 0, out->yaw});
        }
    };
}

AiUnit::~AiUnit() { motion_->pilot = nullptr; }

void AiUnit::give_order_go(Vec3 target) { brain_.give_order({kOrderGo, 0, kTargetPlace, target}, 3); }

void AiUnit::set_migration(ClanMigration* clan) {
    brain_.migration = clan;
    brain_.give_order({kOrderMigrate, 0, kTargetNone, {}}, 3);
}

void AiUnit::set_perception(const Perception* world, const SelfInfo& self) {
    brain_.world = world;
    brain_.self = self;
}

UnitState AiUnit::state() const {
    const Mat4& m = motion_->output_matrix();
    UnitState s;
    s.pos = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    s.vel = motion_->world_velocity();
    s.dir = normalize(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)});
    s.time = now_;
    return s;
}

void AiUnit::update(float now_ms) {
    now_ = std::uint32_t(std::lround(now_ms));
    // Parts summary (1001bbe0): runtime max forward speed body[7], min speed body[4].
    walker_.max_speed = motion_->body[7];
    walker_.min_speed = motion_->body[4];
    wizard_.set_tick_time(now_);
    brain_.takt(now_, wizard_);
    motion_->update(now_ms);
}

BuildingAi::BuildingAi(const LandMap& map, const ArealGraph& graph, RandomSource rand, std::function<Mat4()> placement)
    : rand_(share(std::move(rand))), walker_(map, graph, rand_), brain_(walker_, rand_), placement_(std::move(placement)) {
    walker_.unit_state = [this] {
        const Mat4 m = placement_();
        UnitState s;
        s.pos = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        s.dir = normalize(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)});
        s.time = now_;
        return s;
    };
}

void BuildingAi::set_perception(const Perception* world, const SelfInfo& self) {
    brain_.world = world;
    brain_.self = self;
}

void BuildingAi::update(float now_ms) {
    now_ = std::uint32_t(std::lround(now_ms));
    brain_.takt(now_, wizard_);
}

} // namespace parkan::ai
