// A running mission's strategy layer: the world model, one SuperAI per clan, and iron3d's side of
// the SuperAI host callback (OnSuperAINotify 100611d0: mission end, messages, objectives, mission.cfg
// script commands) turned into events the app shows. Spec: notes/script-vm.md "Host callback",
// notes/mission-data.md, notes/components.md "ISuperAI".
//
// The original's driver (iron3d):
// - OnWorldNotify creates a SuperAI for EVERY data.tma clan (CreateSuperAI per clan record, mode-0
//   nature clans included: Tut_2's player script counts clan 1's animals through it), then starts
//   each: Start(1) for the player's clan and for mode-0 clans, Start(2) for the others (10039440).
// - MainLoop each frame: RunMissionEvent on the PLAYER's SuperAI only (game+0xad0; the 2-second gate
//   DAT_10054c68 is global), then Takt on every clan's SuperAI (1005f27b..1005f2c0).
//   So only the player's script runs "Mission"; the others act through Problems.
//
// Port decisions: the placed objects are entered into their clan's unit list before Start (so Init's
// SetUnitReserved / CountOwnUnits see them; the original's order is not traced); brains per clan
// default to 0 (iron3d adds `clan+0x20` free brains, source not traced); fDifficulty keeps its
// varset value (10005d00 sets it from a table not yet read).
#pragma once

#include "game/super_ai.hpp"

#include <map>
#include <memory>

namespace parkan::game {

// mission.cfg / messages.cfg: `object <name>` … `key = value` … `end` (CP1251 → UTF-8, quotes cut).
struct CfgObject {
    std::string name;
    std::vector<std::pair<std::string, std::string>> entries;
    const std::string* get(std::string_view key) const;
};
std::vector<CfgObject> parse_cfg(std::string_view text);

struct MissionConfig {
    std::vector<std::string> primary_objectives, bonus_objectives;
    std::map<std::uint32_t, std::string> script_commands; // mission.cfg object script: scriptN
    struct Message {
        std::string text_resource, voice_resource;
    };
    std::map<std::uint32_t, Message> messages; // messages.cfg by message_index
};
MissionConfig load_mission_config(const std::filesystem::path& mission_dir);

struct MissionEvent {
    enum class Type { MissionFailed, MissionComplete, Message, ObjectiveComplete, ObjectiveFailed, ObjectiveReset, ScriptCommand };
    Type type;
    std::uint32_t id = 0;   // message index, objective number or scriptN number
    std::uint32_t clan = 0; // the clan whose script sent it
    std::string text;       // ScriptCommand: the mission.cfg command line ("create(...)"); Message: text key
};
const char* to_string(MissionEvent::Type t);

struct MissionOptions {
    std::optional<std::uint32_t> player_clan; // default: the hero's clan, else 0
    std::uint32_t brains_per_clan = 0;
    std::uint32_t seed = 1; // the CRT rand() seed shared by every SuperAI
};

class MissionRuntime {
public:
    // `scripts_dir` is MISSIONS/SCRIPTS (bundles fall back to `default` when missing).
    MissionRuntime(World world, const std::filesystem::path& scripts_dir, MissionConfig cfg, MissionOptions opt = {});
    // Reads <mission_dir>/data.tma, mission.cfg and messages.cfg; `mission_dir` is game-relative.
    static std::unique_ptr<MissionRuntime> load(const std::filesystem::path& game_dir, std::string_view mission_dir,
                                                MissionOptions opt = {});
    MissionRuntime(const MissionRuntime&) = delete; // the SuperAIs hold references into it
    MissionRuntime& operator=(const MissionRuntime&) = delete;

    World& world() { return world_; }
    const MissionConfig& config() const { return cfg_; }
    SuperAI* super_ai(std::uint32_t clan) { return clan < registry_.size() ? registry_[clan] : nullptr; }
    std::uint32_t player_clan() const { return player_; }

    void set_event_sink(std::function<void(const MissionEvent&)> sink) { sink_ = std::move(sink); }
    const std::vector<MissionEvent>& events() const { return log_; }

    // Once per game frame. The first call starts every SuperAI (Init runs).
    void update(std::uint32_t now_ms);

    // World changes the app reports.
    void object_added(std::uint32_t logic_id);                    // a spawned object, already in world()
    void object_killed(std::uint32_t logic_id);
    void object_captured(std::uint32_t logic_id, std::uint32_t new_clan);
    void object_event(std::uint32_t logic_id, std::uint32_t event); // NotifyObjectEvent to the owner's SuperAI

    // Objective states as OnSuperAINotify keeps them: 0 open, 1 complete, -1 failed. Primary first,
    // then bonus (the scripts number them in that order: c2m1p completes objective 4 = bonus 1).
    const std::vector<int>& objectives() const { return objectives_; }
    bool mission_over() const { return failed_ || complete_; }
    bool mission_complete() const { return complete_; }

private:
    void on_callback(std::uint32_t clan, std::uint32_t channel, std::uint32_t kind, std::uint32_t b);
    void emit(MissionEvent e);

    World world_;
    MissionConfig cfg_;
    std::vector<SuperAI*> registry_;
    std::vector<std::unique_ptr<SuperAI>> ais_;
    std::function<void(const MissionEvent&)> sink_;
    std::vector<MissionEvent> log_;
    std::uint32_t rand_seed_;
    std::uint32_t player_ = 0;
    bool started_ = false;
    std::uint32_t mission_gate_ = 0; // DAT_10054c68
    std::vector<int> objectives_;
    bool failed_ = false, complete_ = false;
};

} // namespace parkan::game
