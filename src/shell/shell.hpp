// The game shell: iron3d's CShell (vtable 100e4660; Init 100078c0, Run 10008f30) with its forms — the
// intro (logo1 → logo2 → splash), the main menu, single game, campaign and mission lists, options
// (video, audio, controls), credits, quit — the history-stack navigation (CShell_ShowForm 10009350 /
// CShell_GoBackForm 100094d0), the launch block the shell hands to the game, and the return after a game
// (CShell_ShowFormAfterGame 100098e0). SDL-free: the app supplies input, a clock and resources, draws
// the frame, plays the queued sounds and acts on the effects. Spec: notes/shell.md.
#pragma once

#include "shell/gui.hpp"
#include "shell/movie.hpp"
#include "shell/rich_text.hpp"
#include "ui/campaign.hpp"

#include <filesystem>
#include <map>
#include <optional>

namespace parkan::shell {

class Shell;

// Form ids (CShell+0x20 map).
enum FormId : int {
    kSplash = 0, kLogo1 = 1, kLogo2 = 2, kMainMenu = 10, kSingleGame = 20, kLoadGame = 21, kCampaignList = 22,
    kMissionList = 23, kMultiLogin = 30, kOptions = 40, kVideoOptions = 42, kAudioOptions = 43, kRedefineControls = 44,
    kControlsTuning = 45, kReportError = 51, kAskQuit = 52, kCredits = 53,
};

// CForm (CForm_Construct 10027c90): the six background pieces, the children, CForm_OnKeyDown and the
// container mouse routing (hover = focus).
class Form : public Widget {
public:
    Form(Shell& shell, int form_id, std::string background) : shell_(shell), form_id_(form_id), background_(std::move(background)) {
        x = y = 0, w = 640, h = 480;
    }
    int form_id() const { return form_id_; }
    Shell& shell() { return shell_; }

    template <class T>
    T* add(std::unique_ptr<T> c) { // CGuiForm_RegisterChild 100c9130: id = index; focus index = the new child
        T* p = c.get();
        p->id = int(children_.size()), p->parent = this;
        children_.push_back(std::move(c));
        focus_ = p->id;
        return p;
    }
    Widget* child(int i) { return i >= 0 && std::size_t(i) < children_.size() ? children_[std::size_t(i)].get() : nullptr; }
    int focus() const { return focus_; }
    void set_focus_index(GuiEnv& env, int i); // 100c9740
    void focus_step(GuiEnv& env, int dir);    // FocusPrev / FocusNext: tab stops that are enabled, wrapping

    virtual void on_command(GuiEnv&, const Command&) {}
    virtual void on_show(GuiEnv&) {}  // slot 27
    virtual void update(GuiEnv&) {}   // slot 26
    virtual bool shows_cursor() const { return true; }
    void draw(OverlayFrame& out, GuiEnv& env) override;
    void draw_background(OverlayFrame& out, GuiEnv& env);

    bool key_down(GuiEnv& env, int vk) override; // CForm_OnKeyDown 100281f0
    bool key_up(GuiEnv& env, int vk) override;
    bool char_in(GuiEnv& env, unsigned char c) override;
    bool lbutton_down(GuiEnv& env, float px, float py) override;
    bool lbutton_up(GuiEnv& env, float px, float py) override;
    bool mouse_move(GuiEnv& env, float px, float py) override;
    bool rbutton_down(GuiEnv& env, float px, float py) override;

protected:
    void update_focus_from_mouse(GuiEnv& env, float px, float py); // 100c9670
    Shell& shell_;
    int form_id_;
    std::string background_;
    std::vector<std::unique_ptr<Widget>> children_;
    int focus_ = -1;
};

// The launch block (iron_3d.exe 0x406550) as far as single player uses it (re_forms §15).
struct LaunchBlock {
    int game_type = 0;      // +0x000: 0 none / quit, 1 single or campaign 01–05, 2 multiplayer, 3 autodemo, 4 campaign 00
    std::string path;       // +0x004: "missions/campaign/campaign.01/mission.02/" (lower case, trailing '/')
    int row = 0;            // +0x148: mission row / save slot
    int mode = 0;           // +0x14c: 1 run the game; after a game 1 Esc / exit, 2 restart, 3 load
    int difficulty = 1;     // +0x150: GAME_LEVEL 0..2
    bool new_mission = true; // +0x154 (0 = load a save)
    bool won = false;       // +0x157
};

// The shell's screen wipe (ScreenTransition_* 1005c2e0–1005c760, table 101034e8): an 8×8 grid of black cells
// whose alpha nibble steps every 25 ms; effects 0–7 reveal (black → clear), 8–15 cover (clear → black).
class ScreenTransition {
public:
    // Start(n): 0x10 = a random reveal from {1,0,2..7}, 0x11 = a random cover from {9,8,10..15}, else n.
    int start(int n, int random_pick);
    static int inverse(int n); // GetInverse 1005c760
    // ScreenTransition_Draw / _Update: the grid with the current levels, then (after 25 ms) the next step.
    void draw(OverlayFrame& out, const GuiEnv& env);
    bool done() const { return done_; }
    bool active() const { return active_; }
    void deactivate() { active_ = false; }
    int effect() const { return effect_; }
    int level(int row, int col) const; // the alpha nibble (0..15) of a cell at the current step

private:
    int effect_ = 0, base_ = 0;
    double last_tick_ = -1e9;
    bool done_ = true, active_ = false;
};

// What the app must do after a frame (the shell's side effects outside the GUI).
struct ShellEffects {
    enum class Music { None, MenuTrack, Stop } music = Music::None; // StartShellMusicTrack("main_menu_track") / MusicStop
    std::optional<float> sfx_volume, cd_volume;                    // live volume (audio options), 0..1
    bool settings_changed = false;                                  // iron_3d.ini text changed: save it
    bool apply_video = false;                                       // video options OK: resize the window
    const Movie* start_movie = nullptr;                             // a movie started: play its audio now
    bool stop_movie_audio = false;                                  // the movie ended or was skipped
    bool change_data_source = false;                                // port: the "game data" button
};

class Shell {
public:
    struct Config {
        ShellControls ctrls;
        std::filesystem::path game_dir;
        std::function<std::string(int)> str;      // iron3d.dll string → cp1251
        std::string ini;                          // iron_3d.ini text ([CS] settings)
        std::string dispatcher;                   // missions/dispatcher.ini text (progress)
        std::string credits;                      // data/credits.txt (cp1251)
        std::vector<std::string> save_slots;      // SAVE/saveslots.cfg slot names ("" = empty)
        std::optional<ui::HudSprite> cursor;      // ui/cursor.cfg ARROW
        float cursor_phase = 0.15f;               // PHASE_DELAY / 1000
        std::vector<std::string> video_modes;     // port: the window sizes offered ("1024x768x32")
        std::string data_source_label;            // port: the extra options button ("" = none)
        bool intro = true;                        // mode 0 plays the intro (port: --skip-intro clears it)
        std::filesystem::path user_dir;           // the user's files (key bindings), never the game folder
    };
    // `mode`: 0 the first start (intro), 1 the main menu, 2 back from a game with `last` (CShell::Init).
    Shell(Config cfg, int mode, const LaunchBlock& last = {});
    ~Shell();

    void start(GuiEnv& env); // the mode's first form (needs the env for OnShow)
    // One frame: the current form's update (timers, movies, credits).
    void update(GuiEnv& env);
    void draw(OverlayFrame& out, GuiEnv& env);
    void key_down(GuiEnv& env, int vk);
    void key_up(GuiEnv& env, int vk);
    void char_in(GuiEnv& env, unsigned char c);
    void mouse_move(GuiEnv& env, float x, float y);
    void lbutton_down(GuiEnv& env, float x, float y);
    void lbutton_up(GuiEnv& env, float x, float y);
    void rbutton_down(GuiEnv& env, float x, float y);

    int current() const { return current_; }
    const std::vector<int>& history() const { return history_; }
    Form* form(int id);
    void show_form(GuiEnv& env, int id); // CShell_ShowForm
    void go_back(GuiEnv& env);           // CShell_GoBackForm
    void exit(GuiEnv& env, int code);    // CShell_Exit 10008df0: 0 quit, 1 run the game
    // −1 while running; 0 quit, 1 start the game in launch().
    int exit_code() const { return exit_code_; }
    // CShell::Run returns once an exit was requested and the cover wipe has finished.
    bool finished() const { return exit_code_ >= 0 && wipe_.done(); }
    const ScreenTransition& wipe() const { return wipe_; }
    // The main menu's OnShow: a random reveal, and its cover kept for the exit (shell+0x1c).
    void start_menu_wipe();
    LaunchBlock& launch() { return launch_; }
    ShellEffects take_effects() { return std::exchange(effects_, {}); }
    ShellEffects& effects() { return effects_; }
    bool cursor_visible() const;
    const Movie* movie() const;

    const Config& config() const { return cfg_; }
    std::string str(int id) const { return cfg_.str ? cfg_.str(id) : std::string(); }
    // iron_3d.ini [CS].
    int setting(std::string_view key, int fallback) const;
    void set_setting(std::string_view key, int value);
    const std::string& settings_text() const { return cfg_.ini; }
    // Campaign progression (dispatcher.ini), rebuilt from the files and the progress text.
    std::vector<ui::Campaign> campaigns() const;
    std::vector<ui::CampaignMission> single_missions() const;
    // Live audio state (audio options).
    float sfx_volume = 0.25f, cd_volume = 0.25f;
    bool music_on = true;

private:
    void build_forms();
    void after_game(GuiEnv& env, const LaunchBlock& last); // CShell_ShowFormAfterGame
    Config cfg_;
    int mode_;
    LaunchBlock last_, launch_;
    std::map<int, std::unique_ptr<Form>> forms_;
    std::vector<int> history_;
    int current_ = -1;
    int exit_code_ = -1;
    ShellEffects effects_;
    ScreenTransition wipe_;
    int exit_wipe_ = 9; // CShell ctor 10007410: shell+0x1c = 9 (cover from the top)
    struct {
        float x = -1e9f, y = -1e9f;
        double ms = 0;
    } tooltip_;
};

// Form 44 redefine_controls (src/shell/controls_form.cpp).
std::unique_ptr<Form> make_redefine_controls_form(Shell& shell);

} // namespace parkan::shell
