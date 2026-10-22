// The end of a mission: iron3d's game state (CGame+0x08), the message drawn over the frozen scene
// once the mission is over (DrawGameStateMessage 1009f850), and the keys that leave it. The game
// tells the controller what happened; the controller says what to voice, whether the world still
// runs, and what to do on a key. Spec: notes/mission-end.md.
//
// The original shows no statistics and no debriefing: a title, up to four hint lines, and a
// one-frame "Exiting..." screen while the game shuts down. Nothing here is timed.
//
// Port decisions:
// - Rounding: the original rounds with FISTP (nearest, ties to even); the port uses nearbyint.
// - The line step uses the pager's convention for the scaled font: (height + 2) × glyph scale.
#pragma once

#include "ui/hud_text.hpp"

#include <map>
#include <optional>

namespace parkan {
class TextRes;
}

namespace parkan::ui {

// CGame+0x08. The values are the original's.
enum class GameState : int {
    Complete = 0,       // CGame_SetMissionComplete 10061e60 / OnSuperAINotify(0, 0, 1)
    Failed = 1,         // CGame_SetMissionFailed 10061d90 / OnSuperAINotify(0, 0, 0)
    ConnectionLost = 2, // multiplayer: the session dropped
    Leaving = 3,        // CGame_LeaveWithLaunchMode 10061f20: MainLoop returns at the top of the next frame
    Running = 4,        // set by MainLoop 1005eb70 on entry
};

// The iron3d strings of the end message (cp1251), RT_STRING ids in comments.
struct EndStrings {
    std::string complete;       // 1012 "МИССИЯ УСПЕШНО ЗАВЕРШЕНА!" / "MISSION COMPLETE !"
    std::string failed;         // 1013 "МИССИЯ НЕ ВЫПОЛНЕНА..." / "MISSION FAILED..."
    std::string connection_lost; // 6225 "Связь потеряна..." / "Multiplayer session lost"
    std::string leaving;        // 5083 "Выход..." / "Exiting..."
    std::string press_esc;      // 5082 "Нажмите Esc для продолжения" / "Press 'Esc' to continue"
    std::string press_r;        // 3075 "Press 'R' to restart mission"
    std::string press_l;        // 3076 "Press 'L' to load saved game"
    std::string server_warning; // 3077 multiplayer host: quitting disconnects the others
};
// From iron3d.dll's string table (TextRes::from_pe of iron3d.dll), converted to cp1251.
EndStrings load_end_strings(const TextRes& iron3d);

// What the message shows. `multiplayer` drops the R/L hints; `host` adds the red server warning
// (both only in the Complete / Failed states, as the original).
struct EndScreen {
    GameState state = GameState::Running;
    bool multiplayer = false, host = false;
};

// The laid-out message, virtual 640×480 coordinates (also what the tests check).
struct EndText {
    std::string text;
    float x = 0, y = 0;
    std::uint32_t argb = 0;
};
struct EndLayout {
    bool visible = false;          // false while Running
    float rect[4] = {};            // the 60%-black backdrop x0, y0, x1, y1
    std::uint32_t rect_argb = 0x99000000u;
    std::optional<EndText> title;  // MENU_FONT
    std::vector<EndText> lines;    // GAME_FONT, the non-empty hint lines in order
    float line_step = 0;           // L
};
EndLayout layout_end_screen(const VirtualScreen& vs, const TftFont& game_font, const TftFont& menu_font, const EndScreen& s,
                            const EndStrings& str);
// Draws the layout: backdrop, title, lines (DrawGameStateMessage's order).
void draw_end_screen(OverlayFrame& out, const VirtualScreen& vs, HudFont game_font, HudFont menu_font, const EndScreen& s,
                     const EndStrings& str);

// Layout constants (DrawGameStateMessage).
inline constexpr float kEndTitleY = 50, kEndLinesY = 75, kEndRectY0 = 35, kEndRectPadX = 30, kEndRectPadBottom = 15;
inline constexpr float kEndHostExtraStep = 15; // added to L when the server warning shows
inline constexpr std::uint32_t kEndTitleGreen = 0xff64ff64u, kEndTitleRed = 0xffff6464u, kEndLineColour = 0xfff0f0f0u,
                               kEndWarningColour = 0xffff6464u;
// CMission_EnterSpectatorAtUnit 100a4df0: the free camera starts this far above the ground under the dead hero.
inline constexpr float kDeathCameraHeight = 16;

// What the app does after the message (the exe's launch mode, launch+0x14c, read by iron_3d.exe 401190).
enum class EndAction {
    None,
    ToShell,  // mode 1: Esc. The shell records a win and opens the campaign's mission list.
    Restart,  // mode 2: R. The exe runs the same launch block again: the mission reloads from scratch.
    LoadGame, // mode 3: L. The shell opens the load-game form.
};

// The state machine of CGame+0x08 for one mission (single player unless `multiplayer`).
class MissionEndController {
public:
    explicit MissionEndController(bool multiplayer = false, bool host = false, bool autodemo = false)
        : multiplayer_(multiplayer), host_(host), autodemo_(autodemo) {}

    GameState state() const { return state_; }
    // launch+0x157: the shell marks the mission complete when the game returns with it set.
    bool won() const { return won_; }
    // MainLoop: stdCalculateGame runs only while Running (multiplayer: unless ConnectionLost). Rendering,
    // sound, the camera and the player's mission script keep running in every state but Leaving.
    bool simulating() const { return state_ == GameState::Running || (multiplayer_ && state_ != GameState::ConnectionLost); }
    // The render overlay draws CHud_Draw (cockpit, map, pager, objectives) only while Running; otherwise the
    // end message (or the help screen, when it is open).
    bool hud_visible() const { return state_ == GameState::Running; }
    bool message_visible() const { return state_ != GameState::Running; }
    EndScreen screen() const { return {state_, multiplayer_, host_}; }

    // The events. Each returns the voice key to queue (QueueUnitVoice with no unit: no _S/_B suffix), or
    // nullptr. The guards are the original's: failing needs state ≠ Failed, completing state ≠ Complete,
    // so a later opposite event flips the result (the scripts keep running after the end).
    const char* mission_failed();   // script kind 0 b=0; objective failure is the script's business
    const char* mission_complete(); // script kind 0 b=1, or the last primary objective (OnSuperAINotify kind 3)
    // OnUnitRemoved 100755f0: the local player's hero left the world. The game must also move the camera:
    // spectator view (view mode 4) at the hero's x, y, ground + kDeathCameraHeight, the hero's orientation.
    const char* hero_killed() { return mission_failed(); }
    // MainLoop: a mission.cfg `only_briefing` mission ends when its briefing does: won, Leaving, mode 1.
    void briefing_only_finished();
    // Multiplayer: the network layer lost the session.
    void connection_lost() { state_ = GameState::ConnectionLost; }

    // Keys while the message shows. `escape` = WM_KEYDOWN VK_ESCAPE (OnGameKeyDown 10071240); `ch` = the
    // WM_CHAR byte (OnGameCharKey 10071680: 'R'/'r' and the Russian-layout 'К'/'к' 0xCA/0xEA restart,
    // 'L'/'l' and 'Д'/'д' 0xC4/0xE4 load; single player, Failed or ConnectionLost only). A key that acts
    // switches to Leaving: draw one more frame (the "Exiting..." screen), then act().
    EndAction on_escape();
    EndAction on_char(unsigned char ch);
    EndAction pending_action() const { return action_; }

private:
    void leave(EndAction a) { action_ = a, state_ = GameState::Leaving; }

    bool multiplayer_, host_, autodemo_;
    GameState state_ = GameState::Running;
    bool won_ = false;
    EndAction action_ = EndAction::None;
};

inline constexpr const char* kVoiceMissionComplete = "VOICE_MISSION_COMPLETE"; // vc_mis_cpl.wav
inline constexpr const char* kVoiceMissionFail = "VOICE_MISSION_FAIL";         // vc_mis_fail.wav

// ui/game_resources.cfg `object voices`: key → voices.lib entry (e.g. VOICE_MISSION_FAIL → vc_mis_fail.wav).
std::map<std::string, std::string> parse_voice_table(std::string_view game_resources_cfg);

} // namespace parkan::ui
