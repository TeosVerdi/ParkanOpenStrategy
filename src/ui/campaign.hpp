// Campaign progression: iron3d's CCampaignDispatcher (ctor 100222d0) over MISSIONS/CAMPAIGN and
// missions/dispatcher.ini, and where the shell goes when a game ends (CShell_ShowFormAfterGame
// 100098e0). Everything but scan_campaigns is a pure function of its inputs.
// Spec: notes/mission-end.md §5, notes/mission-data.md.
//
// Port decisions:
// - Paths are the original's lower-case strings ("missions/campaign/campaign.00/mission.01/"); they are
//   both the dispatcher.ini keys and, resolved case-insensitively, the folders.
// - A title is the first line of `descr` (at most 127 bytes, cp1251); the port also drops a trailing CR.
// - The original edits dispatcher.ini in the game folder (WritePrivateProfileString); the port edits the
//   text and leaves where it lives to the app.
#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace parkan::ui {

struct CampaignMission {
    std::string path;  // "missions/campaign/campaign.00/mission.01/"
    std::string title; // descr, cp1251
    bool complete = false, available = false;
};
struct Campaign {
    std::string path; // "missions/campaign/campaign.00"
    std::string title;
    std::vector<CampaignMission> missions;
    bool complete = false, available = false;
};

// BuildCampaignList 10022a00 / BuildMissionList 10022480: campaign.00, .01 … up to the first folder
// without `descr`; inside each, mission.01, .02 … likewise. Flags are left clear (apply_progress).
std::vector<Campaign> scan_campaigns(const std::filesystem::path& game_dir);

// missions/dispatcher.ini: [COMMON] ALL_AVAILABLE=1, [COMPLETE] <key>=1.
struct CampaignProgress {
    bool all_available = false;
    std::set<std::string> complete; // mission keys whose value is non-zero
};
CampaignProgress parse_dispatcher_ini(std::string_view text);
// MakeMissionKey 10022f40: every non-alphanumeric byte of the path becomes '_'.
std::string mission_key(std::string_view mission_path);
// SetMissionComplete 10022e40: WritePrivateProfileString("COMPLETE", key, "1"): the key's line is replaced,
// or appended to the [COMPLETE] section (created at the end when missing). Other lines are kept.
std::string set_mission_complete(std::string ini_text, std::string_view mission_path);

// The flags BuildCampaignList / BuildMissionList compute:
// - mission 1 is available; mission n is available when mission n−1 is complete (port: and only inside an
//   available campaign, whose list is the only one the shell opens);
// - a mission is complete when it is available and its key is set (ALL_AVAILABLE: every available one);
// - campaign 0 is available; campaign n when campaign n−1 is complete;
// - a campaign is complete when it is available and all its missions are (ALL_AVAILABLE: when available).
void apply_progress(std::vector<Campaign>& campaigns, const CampaignProgress& progress);

// Port helper (the original never starts the next mission itself; the shell lists it): the mission after
// `mission_path` in campaign order (the next mission.NN, else the next campaign's first mission), if it is
// available under `campaigns`' flags. nullopt after the last mission or when it is still locked.
std::optional<std::string> next_mission(const std::vector<Campaign>& campaigns, std::string_view mission_path);

// CShell_ShowFormAfterGame: the shell form shown after a game, from the launch block.
struct ShellTarget {
    enum Form { MainMenu, CampaignMissionList, SingleList, LoadGame } form = MainMenu;
    int campaign = -1, mission = -1; // CampaignMissionList: campaign.NN and the 0-based index of mission.NN
};
// `launch_mode` = launch+0x14c (1 Esc / menu exit, 3 load); `new_mission` = launch+0x154 (0 = the game was
// loaded from a save); `game_type` = launch+0 (1 single player [inferred]).
ShellTarget shell_target_after_game(std::string_view mission_path, int launch_mode, bool new_mission, int game_type = 1);

} // namespace parkan::ui
