#include "game/mission_runtime.hpp"

#include "core/paths.hpp"

#include <algorithm>
#include <cctype>

namespace parkan::game {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

std::optional<std::string> read_text(const std::filesystem::path& dir, std::string_view name) {
    auto p = resolve_path(dir, name);
    if (!p) return std::nullopt;
    const Bytes b = read_file(*p);
    return cp1251_to_utf8({reinterpret_cast<const char*>(b.data()), b.size()});
}

} // namespace

const std::string* CfgObject::get(std::string_view key) const {
    for (const auto& [k, v] : entries)
        if (lower(k) == lower(key)) return &v;
    return nullptr;
}

std::vector<CfgObject> parse_cfg(std::string_view text) {
    std::vector<CfgObject> out;
    bool open = false;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        // Comments: '#' lines and ';' tails (messages.cfg); a ';' inside quotes is kept.
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            if (!quoted && (line[i] == ';' || (line[i] == '#' && i == line.find_first_not_of(" \t")))) {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (const auto eq = line.find('='); eq != std::string_view::npos && open) {
            std::string_view v = trim(line.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.find('"', 1) == std::string_view::npos ? v.size() - 1 : v.find('"', 1) - 1);
            out.back().entries.emplace_back(std::string(trim(line.substr(0, eq))), std::string(v));
            continue;
        }
        const std::string l = lower(line);
        if (l.rfind("object", 0) == 0 && (l.size() == 6 || std::isspace((unsigned char)l[6]))) {
            out.push_back({std::string(trim(line.substr(6))), {}});
            open = true;
        } else if (l == "end") {
            open = false;
        }
    }
    return out;
}

MissionConfig load_mission_config(const std::filesystem::path& mission_dir) {
    MissionConfig cfg;
    if (auto text = read_text(mission_dir, "mission.cfg")) {
        for (const auto& o : parse_cfg(*text)) {
            const std::string n = lower(o.name);
            if (n == "primary_objectives" || n == "bonus_objectives")
                for (const auto& [k, v] : o.entries) (n[0] == 'p' ? cfg.primary_objectives : cfg.bonus_objectives).push_back(v);
            if (n == "script")
                for (const auto& [k, v] : o.entries)
                    if (lower(k).rfind("script", 0) == 0) cfg.script_commands[std::uint32_t(std::atoi(k.c_str() + 6))] = v;
        }
    }
    if (auto text = read_text(mission_dir, "messages.cfg")) {
        for (const auto& o : parse_cfg(*text)) {
            const std::string* idx = o.get("message_index");
            if (!idx) continue;
            MissionConfig::Message m;
            if (const std::string* t = o.get("text_resource")) m.text_resource = *t;
            if (const std::string* v = o.get("voice_resource")) m.voice_resource = *v;
            cfg.messages[std::uint32_t(std::atoi(idx->c_str()))] = std::move(m);
        }
    }
    return cfg;
}

const char* to_string(MissionEvent::Type t) {
    switch (t) {
    case MissionEvent::Type::MissionFailed: return "mission failed";
    case MissionEvent::Type::MissionComplete: return "mission complete";
    case MissionEvent::Type::Message: return "message";
    case MissionEvent::Type::ObjectiveComplete: return "objective complete";
    case MissionEvent::Type::ObjectiveFailed: return "objective failed";
    case MissionEvent::Type::ObjectiveReset: return "objective reset";
    case MissionEvent::Type::ScriptCommand: return "script command";
    }
    return "?";
}

MissionRuntime::MissionRuntime(World world, const std::filesystem::path& scripts_dir, MissionConfig cfg, MissionOptions opt)
    : world_(std::move(world)), cfg_(std::move(cfg)), registry_(kMaxClans, nullptr), rand_seed_(opt.seed) {
    objectives_.assign(cfg_.primary_objectives.size() + cfg_.bonus_objectives.size(), 0);
    if (opt.player_clan) {
        player_ = *opt.player_clan;
    } else {
        for (const auto& o : world_.objects())
            if (o.type == kTypeHero && o.clan < world_.clans.size()) {
                player_ = o.clan;
                break;
            }
    }
    // The CRT's rand(): one generator for every SuperAI (Takt jitter, h70).
    RandomSource rand = [this] {
        rand_seed_ = rand_seed_ * 214013u + 2531011u;
        return int((rand_seed_ >> 16) & 0x7fff);
    };
    auto cb = [this](std::uint32_t clan, std::uint32_t channel, std::uint32_t kind, std::uint32_t b) { on_callback(clan, channel, kind, b); };
    for (std::uint32_t c = 0; c < world_.clans.size() && c < kMaxClans; ++c) {
        ai::ScriptProgram program;
        try {
            program = ai::load_script_bundle(scripts_dir, world_.clans[c].script);
        } catch (const std::exception&) {
            program = ai::load_script_bundle(scripts_dir, "default"); // CSuperAI ctor: a missing bundle → default
        }
        auto ai = std::make_unique<SuperAI>(world_, registry_, c, std::move(program), cb, rand);
        const std::uint32_t brains = opt.brains_per_clan ? *opt.brains_per_clan : world_.clans[c].brains;
        for (std::uint32_t i = 0; i < brains; ++i) ai->add_brain();
        ais_.push_back(std::move(ai));
    }
}

std::unique_ptr<MissionRuntime> MissionRuntime::load(const std::filesystem::path& game_dir, std::string_view mission_dir,
                                                     MissionOptions opt) {
    auto dir = resolve_path(game_dir, mission_dir);
    if (!dir) throw std::runtime_error("mission folder not found: " + std::string(mission_dir));
    auto tma = resolve_path(*dir, "data.tma");
    if (!tma) throw std::runtime_error("no data.tma in " + dir->string());
    auto scripts = resolve_path(game_dir, "MISSIONS/SCRIPTS");
    if (!scripts) throw std::runtime_error("no MISSIONS/SCRIPTS");
    const mission::Tma t = mission::decode_tma(read_file(*tma));
    World w = World::from_tma(t, &game_dir);
    auto rt = std::make_unique<MissionRuntime>(std::move(w), *scripts, load_mission_config(*dir), opt);
    rt->set_economy(std::make_unique<economy::Economy>(rt->world(), game_dir, &t, opt.seed));
    return rt;
}

void MissionRuntime::set_economy(std::unique_ptr<economy::Economy> eco) {
    economy_ = std::move(eco);
    if (!economy_) return;
    economy_->set_brains([this](std::uint32_t clan) -> std::vector<std::uint32_t>* {
        SuperAI* s = super_ai(clan);
        return s ? &s->brains() : nullptr;
    });
    economy_->set_event_sink([this](const economy::EconomyEvent& e) { on_economy_event(e); });
    for (auto& ai : ais_) ai->set_economy(economy_.get());
}

void MissionRuntime::on_economy_event(const economy::EconomyEvent& e) {
    using T = economy::EconomyEvent::Type;
    const WorldObject* o = world_.find(e.object);
    SuperAI* owner = o && o->clan < kMaxClans ? registry_[o->clan] : nullptr;
    // The host first (it creates the new object's body and attaches its control), then the strategist.
    if (eco_sink_) eco_sink_(e);
    switch (e.type) {
    case T::UnitProduced:
    case T::BuildingPlaced:
        if (economy_) economy_->wrap_controls();
        if (owner) owner->on_object_added(e.type == T::UnitProduced ? 1 : 2, e.object); // OnWorldNotify slot 4
        break;
    case T::BuildingUpgraded:
        for (auto& ai : ais_) ai->on_object_removed(e.object);
        if (owner) owner->on_object_added(2, e.object);
        break;
    case T::TaskComplete:
        if (owner) owner->notify_object_event(e.object, 3);
        break;
    default: break;
    }
}

void MissionRuntime::update(std::uint32_t now_ms) {
    if (!started_) {
        started_ = true;
        if (economy_) {
            economy_->wrap_controls();
            economy_->start([this](std::uint32_t clan) { return super_ai(clan) != nullptr; });
        }
        for (const auto& o : world_.objects())
            if (o.alive && o.clan < kMaxClans && registry_[o.clan]) registry_[o.clan]->on_object_added(o.building() ? 2 : 1, o.logic_id, true);
        for (auto& ai : ais_) { // 10039440
            const bool nature = ai->clan() < world_.clans.size() && world_.clans[ai->clan()].mode == 0;
            ai->start(nature || ai->clan() == player_ ? 1 : 2);
        }
    }
    if (now_ms >= mission_gate_) { // RunMissionEvent (10001b80): the player's clan only
        mission_gate_ = now_ms + 2000;
        if (SuperAI* p = super_ai(player_)) p->run_mission_event();
    }
    if (economy_) economy_->update(now_ms);
    for (auto& ai : ais_) ai->takt(now_ms);
}

void MissionRuntime::object_added(std::uint32_t logic_id) {
    if (economy_) {
        economy_->wrap_controls();
        economy_->on_object_added(logic_id);
    }
    const WorldObject* o = world_.live(logic_id);
    if (o && o->clan < kMaxClans && registry_[o->clan]) registry_[o->clan]->on_object_added(o->building() ? 2 : 1, logic_id);
}

void MissionRuntime::object_killed(std::uint32_t logic_id) {
    for (auto& ai : ais_) ai->on_object_removed(logic_id);
    if (economy_) economy_->on_object_removed(logic_id);
    world_.kill(logic_id);
}

void MissionRuntime::object_captured(std::uint32_t logic_id, std::uint32_t new_clan) {
    for (auto& ai : ais_) ai->on_object_removed(logic_id);
    const WorldObject* o = world_.find(logic_id);
    const std::uint32_t old = o ? o->clan : kNoClan;
    world_.set_clan(logic_id, new_clan);
    if (economy_) economy_->on_object_captured(logic_id, old);
    const WorldObject* n = world_.live(logic_id);
    if (n && n->clan < kMaxClans && registry_[n->clan]) registry_[n->clan]->on_object_added(n->building() ? 2 : 1, logic_id);
}

void MissionRuntime::object_event(std::uint32_t logic_id, std::uint32_t event) {
    const WorldObject* o = world_.find(logic_id);
    if (o && o->clan < kMaxClans && registry_[o->clan]) registry_[o->clan]->notify_object_event(logic_id, event);
}

void MissionRuntime::emit(MissionEvent e) {
    log_.push_back(e);
    if (sink_) sink_(log_.back());
}

// iron3d OnSuperAINotify (100611d0).
void MissionRuntime::on_callback(std::uint32_t clan, std::uint32_t channel, std::uint32_t kind, std::uint32_t b) {
    using T = MissionEvent::Type;
    if (channel == 2) { // run mission.cfg script<kind> through the console
        auto it = cfg_.script_commands.find(kind);
        emit({T::ScriptCommand, kind, clan, it != cfg_.script_commands.end() ? it->second : std::string()});
        return;
    }
    if (channel != 0) return;
    switch (kind) {
    case 0: // mission failed (b 0) / complete (b 1), each once
        if (b == 0 && !failed_) failed_ = true, emit({T::MissionFailed, b, clan, {}});
        if (b == 1 && !complete_) complete_ = true, emit({T::MissionComplete, b, clan, {}});
        break;
    case 1: {
        auto it = cfg_.messages.find(b);
        emit({T::Message, b, clan, it != cfg_.messages.end() ? it->second.text_resource : std::string()});
        break;
    }
    case 3: // complete; the last primary objective completes the mission (1006b550)
        if (b < objectives_.size() && objectives_[b] == 0) {
            objectives_[b] = 1;
            emit({T::ObjectiveComplete, b, clan, {}});
            const std::size_t n = cfg_.primary_objectives.size();
            if (n > 0 && std::all_of(objectives_.begin(), objectives_.begin() + std::ptrdiff_t(n), [](int s) { return s == 1; }))
                on_callback(clan, 0, 0, 1);
        }
        break;
    case 4:
        if (b < objectives_.size() && objectives_[b] != -1) objectives_[b] = -1, emit({T::ObjectiveFailed, b, clan, {}});
        break;
    case 5:
        if (b < objectives_.size() && objectives_[b] != 0) objectives_[b] = 0, emit({T::ObjectiveReset, b, clan, {}});
        break;
    default: // 2 CLAN_HERO_KILLED: no case in iron3d
        break;
    }
}

} // namespace parkan::game
