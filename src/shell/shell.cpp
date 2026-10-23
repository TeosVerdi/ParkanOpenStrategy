#include "shell/shell.hpp"

#include "core/paths.hpp"
#include "shell/ini.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace parkan::shell {

namespace {

constexpr int kStrMainMenu = 6135, kStrDemo = 6136, kStrStartGame = 6115, kStrLoadGame = 6116, kStrOptions = 6118,
              kStrCredits = 6138, kStrExit = 6139, kStrCampaigns = 6113, kStrSingle = 6114, kStrMulti = 6137,
              kStrCampaignFmt = 6144, kStrVideo = 6119, kStrAudio = 6120, kStrControls = 6121, kStrDevices = 6154,
              kStrOk = 6226, kStrCancel = 6227, kStrBack = 6228, kStrDefaults = 6229, kStrAskQuit = 6151, kStrYes = 6152,
              kStrNo = 6153;

std::unique_ptr<TextButton> text_button(float x0, float y0, float x1, float y1, std::string text) {
    auto b = std::make_unique<TextButton>();
    b->set_rect(x0, y0, x1, y1);
    b->text = std::move(text);
    return b;
}

std::unique_ptr<Title> title(float x0, float y0, float x1, float y1, std::string text) {
    auto t = std::make_unique<Title>();
    t->set_rect(x0, y0, x1, y1);
    t->text = std::move(text);
    return t;
}

std::unique_ptr<ImageButton> image_button(std::string prefix, std::string tip) {
    auto b = std::make_unique<ImageButton>();
    b->prefix = std::move(prefix);
    b->tooltip = std::move(tip);
    b->set_rect(0, 0, 34, 25);
    return b;
}

std::unique_ptr<ButtonBar> bar(float x0, float y0, float x1, float y1) {
    auto b = std::make_unique<ButtonBar>();
    b->set_rect(x0, y0, x1, y1);
    return b;
}

std::unique_ptr<CheckBox> checkbox(float x0, float y0, float x1, float y1, std::string text) {
    auto c = std::make_unique<CheckBox>();
    c->set_rect(x0, y0, x1, y1);
    c->text = std::move(text);
    return c;
}

std::unique_ptr<Slider> slider(float x0, float y0, float x1, float y1, std::string text, Slider::Kind kind) {
    auto s = std::make_unique<Slider>();
    s->set_rect(x0, y0, x1, y1);
    s->text = std::move(text);
    s->kind = kind;
    if (kind == Slider::Kind::Percent) s->min = 0, s->max = 1, s->step = 0.01f;
    if (kind == Slider::Kind::Float) s->min = 0.5f, s->max = 2.f, s->step = 0.05f, s->value = 1;
    if (kind == Slider::Kind::Items) s->min = 0, s->max = 0, s->step = 1;
    return s;
}

void set_items(Slider& s, std::vector<std::string> items) {
    s.items = std::move(items);
    s.min = 0, s.max = float(std::max<std::size_t>(s.items.size(), 1) - 1);
    s.value = std::clamp(s.value, s.min, s.max);
}

// CGuiScrollList with a back button (mission / campaign / save lists): list, bar, title are children n, n+1, n+2.
ScrollList* add_scroll_list(Form& f, float x0, float y0, float x1, float y1, std::string title_text, std::string back_tip) {
    auto list = std::make_unique<ScrollList>();
    list->set_rect(x0, y0, x1, y1);
    ScrollList* l = f.add(std::move(list));
    auto b = bar(x0, y0, x1, y1);
    auto up = std::make_unique<ScrollArrow>();
    up->prefix = "scroll_up_button";
    auto down = std::make_unique<ScrollArrow>();
    down->prefix = "scroll_down_button";
    l->up = up.get(), l->down = down.get();
    b->add_button(std::move(up), 1);
    b->add_button(std::move(down), 1);
    b->add_button(image_button("back_button", std::move(back_tip)), 0);
    l->bar = f.add(std::move(b));
    l->title = f.add(title(x0, y0, x1, y0 + 25, std::move(title_text)));
    l->update_arrows();
    return l;
}

// ---- Logo / splash (CFormLogo_Construct 1000b880; OnShow 1000bc00, Update 1000bb40, Draw 1000baa0) ----

class LogoForm : public Form {
public:
    LogoForm(Shell& s, int id, std::string bg, std::string param, int next) : Form(s, id, std::move(bg)), param_(std::move(param)), next_(next) {}

    void on_show(GuiEnv& env) override {
        done_ = false, fader_ = std::nullopt, movie_.reset();
        if (param_.empty()) return; // no player: one black frame, then the next item
        // CMoviePlayer_Start: the file (relative to the game folder); any failure → the fader of that name.
        std::string rel = param_;
        for (char& c : rel)
            if (c == '\\') c = '/';
        if (auto p = resolve_path(shell_.config().game_dir, rel)) {
            try {
                movie_ = std::make_unique<Movie>(*p);
                start_ms_ = env.now_ms;
                shell_.effects().start_movie = movie_.get();
                return;
            } catch (const std::exception&) {
                movie_.reset();
            }
        }
        // CLogoFader (10023060): the cfg object named by the parameter; none → done at once.
        std::string key = param_;
        if (key.size() > 6 && key.ends_with("_fader")) key.resize(key.size() - 6);
        const auto& faders = shell_.config().ctrls.faders;
        if (auto it = faders.find(key); it != faders.end()) {
            const Fader& f = it->second;
            if (!f.sound.empty()) env.play(f.sound);
            if (f.in != 0 && f.out != 0 && f.show != 0) fader_ = FaderState{f, 0, env.now_ms, 0, false};
        }
    }
    void update(GuiEnv& env) override {
        if (movie_) {
            const double t = (env.now_ms - start_ms_) / 1000.0;
            if (skip_ || movie_->finished(t)) done_ = true;
            else movie_->advance(t);
        } else if (fader_) {
            FaderState& f = *fader_;
            const double t = (env.now_ms - f.t0) / 1000.0;
            switch (f.phase) { // CLogoFader_Update 100233c0
            case 0:
                f.a = float(t / f.cfg.in);
                if (f.a >= 1) f.a = 1, f.phase = 1, f.t0 = env.now_ms;
                break;
            case 1:
                f.a = 1;
                if (t > f.cfg.show) f.phase = 2, f.t0 = env.now_ms;
                break;
            case 2:
                f.a = 1 - float(t / f.cfg.out);
                if (f.a <= 0) f.a = 0, f.phase = 3, f.t0 = env.now_ms;
                break;
            default: f.a = 0;
            }
            if (f.skip && f.phase == 1) f.phase = 2, f.t0 = env.now_ms;
            if (f.phase == 3) done_ = true;
        } else {
            done_ = true;
        }
        if (done_) {
            if (movie_) shell_.effects().stop_movie_audio = true;
            movie_.reset();
            fader_.reset();
            skip_ = false;
            shell_.show_form(env, next_);
        }
    }
    void draw(OverlayFrame& out, GuiEnv& env) override {
        if (movie_) {
            // CMoviePlayer: the DirectShow window fits the client area keeping the aspect, centred (here: the
            // 4:3 virtual screen).
            if (movie_->shown_frame() >= 0) {
                const float mw = float(movie_->width()), mh = float(movie_->height());
                float w = 640, h = mh * 640 / mw;
                if (h > 480) h = 480, w = mw * 480 / mh;
                OverlayQuad q{(320 - w / 2) * env.sx(), (240 - h / 2) * env.sy(), (320 + w / 2) * env.sx(), (240 + h / 2) * env.sy()};
                q.texture = env.texture("movie");
                if (q.texture >= 0) out.quads.push_back(q);
            }
            return;
        }
        if (fader_) {
            draw_background(out, env);
            // CLogoFader_Draw 10023380: black over the background, alpha = 255·(1 − a).
            ui::fill_rect(out, env.vs, 0, 0, 640, 480, std::uint32_t(std::nearbyint((1 - fader_->a) * 255)) << 24);
            return;
        }
        ui::fill_rect(out, env.vs, 0, 0, 640, 480, 0xff000000u);
    }
    // CFormLogo_OnSkipInput 1000bb00: any key or the left button skips the current item only.
    bool key_down(GuiEnv&, int) override { return skip(); }
    bool lbutton_down(GuiEnv&, float, float) override { return skip(); }
    bool mouse_move(GuiEnv&, float, float) override { return true; }
    bool shows_cursor() const override { return false; }
    const Movie* movie() const { return movie_.get(); }

private:
    bool skip() {
        if (movie_) skip_ = true;
        if (fader_) fader_->skip = true;
        return true;
    }
    struct FaderState {
        Fader cfg;
        int phase = 0;
        double t0 = 0;
        float a = 0;
        bool skip = false;
    };
    std::string param_;
    int next_;
    std::unique_ptr<Movie> movie_;
    std::optional<FaderState> fader_;
    double start_ms_ = 0;
    bool done_ = false, skip_ = false;
};

// ---- Main menu (CFormMainMenu 100062e0; listener 10006c80; OnShow 10006f10) ----

class MainMenuForm : public Form {
public:
    explicit MainMenuForm(Shell& s) : Form(s, kMainMenu, "Main") {
        add(title(62, 77, 398, 102, s.str(kStrMainMenu)));
        const int ids[6] = {kStrDemo, kStrStartGame, kStrLoadGame, kStrOptions, kStrCredits, kStrExit};
        float y0 = 108;
        for (int i = 0; i < 6; ++i) {
            if (i == 5) y0 += 3; // the exit button sits 31 below the credits
            add(text_button(62, y0, 398, y0 + 25, s.str(ids[i])));
            y0 += 28;
        }
        // Port: the autodemo (CAutoDemo, launch type 3) is not ported: "Демо" is disabled and the idle
        // timer does not start it.
        child(1)->enable(false);
    }
    void on_show(GuiEnv& env) override {
        set_focus_index(env, 2);
        shell_.effects().music = ShellEffects::Music::MenuTrack;
        shell_.start_menu_wipe();
        env.play("MISSION_START");
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.show_form(env, kAskQuit);
        switch (c.sender) {
        case 2: shell_.show_form(env, kSingleGame); break;
        case 3: shell_.show_form(env, kLoadGame); break;
        case 4: shell_.show_form(env, kOptions); break;
        case 5: shell_.show_form(env, kCredits); break;
        case 6: shell_.show_form(env, kAskQuit); break;
        default: break;
        }
    }
};

// ---- Mission list (1000b2a0; listener 1000b520; OnShow 1000b720) ----

class MissionListForm : public Form {
public:
    explicit MissionListForm(Shell& s) : Form(s, kMissionList, "Pl00") {
        list_ = add_scroll_list(*this, 42, 42, 488, 269, "", s.str(kStrBack));
    }
    void setup(std::string base, bool campaign, int initial, std::string title_text) {
        base_ = std::move(base), campaign_ = campaign, initial_ = initial;
        list_->title->text = std::move(title_text);
    }
    void on_show(GuiEnv& env) override {
        // Fill (1001d890): the rows are rebuilt on every show; a row is disabled when not available.
        list_->clear_rows();
        if (campaign_) {
            for (const auto& c : shell_.campaigns())
                if (base_.rfind(c.path, 0) == 0)
                    for (const auto& m : c.missions) list_->add_row(&env, m.title, m.path, m.available);
        } else {
            for (const auto& m : shell_.single_missions()) list_->add_row(&env, m.title, m.path, m.available);
        }
        set_focus_index(env, 0);
        list_->select(env, initial_);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender == list_->id) {
            if (c.value < 0 || std::size_t(c.value) >= list_->paths.size()) return;
            LaunchBlock& l = shell_.launch();
            l.new_mission = true, l.row = c.value, l.path = list_->paths[std::size_t(c.value)];
            shell_.exit(env, 1);
        } else if (c.sender == list_->bar->id) {
            if (c.value == 0) list_->scroll(-1);
            else if (c.value == 1) list_->scroll(1);
            else shell_.go_back(env);
        }
    }

private:
    ScrollList* list_ = nullptr;
    std::string base_;
    bool campaign_ = true;
    int initial_ = 0;
};

// ---- Campaign list (10003430; listener 10003710; OnShow 10003960) ----

class CampaignListForm : public Form {
public:
    explicit CampaignListForm(Shell& s) : Form(s, kCampaignList, "Main") {
        list_ = add_scroll_list(*this, 42, 42, 488, 269, s.str(kStrCampaigns), s.str(kStrBack));
        refill();
    }
    // CCampaignListBox_FillRows 1001ddf0: once, when the shell is built (after the win is recorded).
    void refill() {
        list_->clear_rows();
        for (const auto& c : shell_.campaigns()) list_->add_row(nullptr, c.title, c.path, c.available);
    }
    void on_show(GuiEnv& env) override {
        set_focus_index(env, 0);
        list_->select(env, 0);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender == list_->id) {
            const int cc = c.value;
            if (cc < 0 || std::size_t(cc) >= list_->paths.size()) return;
            shell_.launch().game_type = cc != 0 ? 1 : 4;
            char buf[512];
            std::snprintf(buf, sizeof buf, shell_.str(kStrCampaignFmt).c_str(), list_->items[std::size_t(cc)]->text.c_str());
            auto* ml = static_cast<MissionListForm*>(shell_.form(kMissionList));
            ml->setup(list_->paths[std::size_t(cc)] + "/mission.", true, c.extra, buf);
            shell_.show_form(env, kMissionList);
        } else if (c.sender == list_->bar->id) {
            if (c.value == 0) list_->scroll(-1);
            else if (c.value == 1) list_->scroll(1);
            else shell_.go_back(env);
        }
    }

private:
    ScrollList* list_ = nullptr;
};

// ---- Single game (100286e0; listener 10028f90; OnShow 10029330) ----

class SingleGameForm : public Form {
public:
    explicit SingleGameForm(Shell& s) : Form(s, kSingleGame, "Main") {
        add(title(42, 42, 488, 67, s.str(kStrStartGame)));
        add(text_button(42, 73, 488, 98, s.str(kStrCampaigns)));
        add(text_button(42, 101, 488, 126, s.str(kStrSingle)));
        add(text_button(42, 129, 488, 154, s.str(kStrMulti)));
        child(3)->enable(false); // port: multiplayer is not ported
        auto lv = slider(42, 160, 488, 185, "", Slider::Kind::Items);
        set_items(*lv, {s.str(6256), s.str(6257), s.str(6258)});
        level_ = add(std::move(lv));
        auto b = bar(42, 191, 488, 216);
        b->add_button(image_button("back_button", s.str(kStrBack)), 0);
        add(std::move(b));
    }
    void on_show(GuiEnv& env) override {
        set_focus_index(env, 1);
        // CFormSingleGame_LoadGameLevel 10029350: [CS] GAME_LEVEL (1 when missing; 2 when out of range).
        int lv = shell_.setting("GAME_LEVEL", 1);
        if (lv < 0 || lv > 2) lv = 2;
        level_->value = float(lv);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        auto store = [&] {
            shell_.set_setting("GAME_LEVEL", level_->index());
            shell_.launch().difficulty = level_->index();
        };
        if (c.code == 1) return store(), shell_.go_back(env);
        switch (c.sender) {
        case 1: store(), shell_.show_form(env, kCampaignList); break;
        case 2: {
            store();
            shell_.launch().game_type = 1;
            auto* ml = static_cast<MissionListForm*>(shell_.form(kMissionList));
            ml->setup("missions/single.", false, c.extra, shell_.str(kStrSingle));
            shell_.show_form(env, kMissionList);
            break;
        }
        case 5: store(), shell_.go_back(env); break;
        default: break;
        }
    }

private:
    Slider* level_ = nullptr;
};

// ---- Load game (10012ec0; listener 10013140) — port: loading saves is not ported ----

class LoadGameForm : public Form {
public:
    explicit LoadGameForm(Shell& s) : Form(s, kLoadGame, "Pl00") {
        list_ = add_scroll_list(*this, 42, 42, 488, 269, s.str(kStrLoadGame), s.str(kStrBack));
    }
    void on_show(GuiEnv& env) override {
        // CSaveListPanel_Populate 1001e350: one row per slot, disabled when the slot is empty.
        list_->clear_rows();
        for (const std::string& n : shell_.config().save_slots) list_->add_row(&env, n.empty() ? shell_.str(6101) : n, "", !n.empty());
        set_focus_index(env, 0);
        list_->select(env, 0);
        if (list_->selected < 0) set_focus_index(env, list_->bar->id);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender == list_->bar->id) {
            if (c.value == 0) list_->scroll(-1);
            else if (c.value == 1) list_->scroll(1);
            else shell_.go_back(env);
        }
        // Port: a filled slot would start CMission::LoadSavedGame (launch +0x154 = 0); saves are not ported.
    }

private:
    ScrollList* list_ = nullptr;
};

// ---- Options (1000fba0; listener 100103c0) ----

class OptionsForm : public Form {
public:
    explicit OptionsForm(Shell& s) : Form(s, kOptions, "Opts") {
        add(title(62, 77, 398, 102, s.str(kStrOptions)));
        add(text_button(62, 108, 398, 133, s.str(kStrVideo)));
        add(text_button(62, 136, 398, 161, s.str(kStrAudio)));
        add(text_button(62, 164, 398, 189, s.str(kStrControls)));
        add(text_button(62, 192, 398, 217, s.str(kStrDevices)));
        auto b = bar(62, 223, 398, 248);
        b->add_button(image_button("back_button", s.str(kStrBack)), 0);
        add(std::move(b));
        // Port addition: change the game data folder or disc image (not in the original).
        if (!s.config().data_source_label.empty()) add(text_button(62, 254, 398, 279, s.config().data_source_label));
    }
    void on_show(GuiEnv& env) override { set_focus_index(env, 1); }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        switch (c.sender) {
        case 1: shell_.show_form(env, kVideoOptions); break;
        case 2: shell_.show_form(env, kAudioOptions); break;
        case 3: shell_.show_form(env, kRedefineControls); break;
        case 4: shell_.show_form(env, kControlsTuning); break;
        case 5: shell_.go_back(env); break;
        case 6: shell_.effects().change_data_source = true; break;
        default: break;
        }
    }
};

// ---- Video options (1002ad70; listener 1002b9b0; LoadSettings 1002bf50; Apply 1002bb00) ----

class VideoOptionsForm : public Form {
public:
    explicit VideoOptionsForm(Shell& s) : Form(s, kVideoOptions, "Opts") {
        auto b = bar(42, 300, 590, 325);
        b->add_button(image_button("ok_button", s.str(kStrOk)), 0);
        b->add_button(image_button("cancel_button", s.str(kStrCancel)), 0);
        add(std::move(b));
        card_ = add(slider(42, 73, 590, 98, "", Slider::Kind::Items));
        mode_ = add(slider(42, 101, 590, 126, "", Slider::Kind::Items));
        detail_ = add(slider(42, 129, 590, 154, "", Slider::Kind::Items));
        reflections_ = add(checkbox(42, 157, 590, 182, s.str(6107)));
        embm_ = add(checkbox(42, 185, 590, 210, s.str(6108)));
        emboss_ = add(checkbox(42, 213, 590, 238, s.str(6109)));
        cursor_ = add(checkbox(42, 241, 590, 266, s.str(6111)));
        subtitles_ = add(checkbox(42, 269, 590, 294, s.str(6160)));
        add(title(42, 42, 590, 67, s.str(6106)));
        // Port: one renderer (SDL GPU) stands for the D3D card list; the video modes are window sizes.
        set_items(*card_, {"SDL GPU"});
        set_items(*mode_, s.config().video_modes);
        set_items(*detail_, {s.str(6103), s.str(6104), s.str(6105)});
    }
    void on_show(GuiEnv& env) override {
        set_focus_index(env, 1);
        int q = shell_.setting("RENDER_QUALITY", 1);
        detail_->value = float(q < 0 || q > 2 ? 1 : q);
        char m[64];
        std::snprintf(m, sizeof m, "%dx%dx%d", shell_.setting("DISPLAY_WIDTH", 1024), shell_.setting("DISPLAY_HEIGHT", 768),
                      shell_.setting("BITDEPTH", 32));
        const auto& modes = shell_.config().video_modes;
        const auto it = std::find(modes.begin(), modes.end(), m);
        mode_->value = it == modes.end() ? 0.f : float(it - modes.begin());
        reflections_->checked = shell_.setting("REFLECTIONS", 1) != 0;
        // Port: environment and emboss bump mapping are not rendered: shown unchecked and disabled (the
        // original disables them when the card lacks the shader mode).
        embm_->checked = false, embm_->enable(false);
        emboss_->checked = false, emboss_->enable(false);
        cursor_->checked = shell_.setting("FORCE_SOFTWARE_CURSOR", 1) == 0;
        subtitles_->checked = shell_.setting("SUBTITLES", 0) != 0;
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender != 0) return;
        if (c.value == 0) {
            int w = 1024, h = 768, d = 32;
            const int i = mode_->index();
            const auto& modes = shell_.config().video_modes;
            if (i >= 0 && std::size_t(i) < modes.size()) std::sscanf(modes[std::size_t(i)].c_str(), "%dx%dx%d", &w, &h, &d);
            shell_.set_setting("CURRENT_D3DCARD", 0);
            shell_.set_setting("DISPLAY_WIDTH", w);
            shell_.set_setting("DISPLAY_HEIGHT", h);
            shell_.set_setting("BITDEPTH", d);
            shell_.set_setting("RENDER_QUALITY", detail_->index());
            shell_.set_setting("REFLECTIONS", reflections_->checked);
            shell_.set_setting("FORCE_SOFTWARE_CURSOR", !cursor_->checked);
            shell_.set_setting("SUBTITLES", subtitles_->checked);
            shell_.effects().apply_video = true;
        }
        shell_.go_back(env);
    }

private:
    Slider *card_, *mode_, *detail_;
    CheckBox *reflections_, *embm_, *emboss_, *cursor_, *subtitles_;
};

// ---- Audio options (10001040; listener 100018b0; OnShow 10001e20; Revert 10001ce0) ----

class AudioOptionsForm : public Form {
public:
    explicit AudioOptionsForm(Shell& s) : Form(s, kAudioOptions, "Opts") {
        auto b = bar(42, 163, 590, 188);
        b->add_button(image_button("ok_button", s.str(kStrOk)), 0);
        b->add_button(image_button("cancel_button", s.str(kStrCancel)), 0);
        add(std::move(b));
        sfx_ = add(slider(42, 73, 590, 98, s.str(6143), Slider::Kind::Percent));
        cd_ = add(slider(42, 101, 590, 126, s.str(6142), Slider::Kind::Percent));
        music_ = add(checkbox(42, 132, 590, 157, s.str(6141)));
        add(title(42, 42, 590, 67, s.str(6140)));
        sfx_->on_change = [this](GuiEnv&) { shell_.sfx_volume = sfx_->value, shell_.effects().sfx_volume = sfx_->value; };
        cd_->on_change = [this](GuiEnv&) { shell_.cd_volume = cd_->value, shell_.effects().cd_volume = cd_->value; };
    }
    void on_show(GuiEnv& env) override {
        saved_ = {shell_.sfx_volume, shell_.cd_volume, shell_.music_on};
        sfx_->value = std::clamp(shell_.sfx_volume, 0.f, 1.f);
        cd_->value = std::clamp(shell_.cd_volume, 0.f, 1.f);
        music_->checked = shell_.music_on;
        set_focus_index(env, 1);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return revert(), shell_.go_back(env);
        if (c.sender == 3) {
            shell_.music_on = music_->checked;
            shell_.effects().music = music_->checked ? ShellEffects::Music::MenuTrack : ShellEffects::Music::Stop;
        } else if (c.sender == 0) {
            if (c.value == 0) {
                shell_.set_setting("SFX_VOLUME", int(std::nearbyint(shell_.sfx_volume * 100)));
                shell_.set_setting("CD_VOLUME", int(std::nearbyint(shell_.cd_volume * 100)));
                shell_.set_setting("PLAY_CD_MUSIC", music_->checked);
            } else {
                revert();
            }
            shell_.go_back(env);
        }
    }

private:
    void revert() {
        const bool music_changed = shell_.music_on != saved_.music;
        shell_.sfx_volume = saved_.sfx, shell_.cd_volume = saved_.cd, shell_.music_on = saved_.music;
        shell_.effects().sfx_volume = saved_.sfx, shell_.effects().cd_volume = saved_.cd;
        if (music_changed) shell_.effects().music = saved_.music ? ShellEffects::Music::MenuTrack : ShellEffects::Music::Stop;
    }
    struct {
        float sfx = 0, cd = 0;
        bool music = true;
    } saved_;
    Slider *sfx_, *cd_;
    CheckBox* music_;
};

// ---- Controls tuning (10029620; listener 1002a1f0) ----

class ControlsTuningForm : public Form {
public:
    explicit ControlsTuningForm(Shell& s) : Form(s, kControlsTuning, "Opts") {
        auto b = bar(42, 222, 590, 247);
        b->add_button(image_button("ok_button", s.str(kStrOk)), 0);
        b->add_button(image_button("cancel_button", s.str(kStrCancel)), 0);
        b->add_button(image_button("setdef_button", s.str(kStrDefaults)), 1);
        add(std::move(b));
        mouse_ = add(slider(42, 73, 590, 98, s.str(6155), Slider::Kind::Float));
        mouse_rev_ = add(checkbox(42, 101, 590, 126, s.str(6157)));
        joy_ = add(slider(42, 132, 590, 157, s.str(6156), Slider::Kind::Float));
        joy_rev_ = add(checkbox(42, 160, 590, 185, s.str(6158)));
        joy_on_ = add(checkbox(42, 191, 590, 216, s.str(6159)));
        add(title(42, 42, 590, 67, s.str(kStrDevices)));
    }
    void on_show(GuiEnv& env) override {
        mouse_->value = std::clamp(float(shell_.setting("MOUSE_SENS", 100)) / 100.f, mouse_->min, mouse_->max);
        mouse_rev_->checked = shell_.setting("MOUSE_REV_Y", 0) != 0;
        joy_->value = std::clamp(float(shell_.setting("JOY_SENS", 100)) / 100.f, joy_->min, joy_->max);
        joy_rev_->checked = shell_.setting("JOY_REV_Y", 0) != 0;
        joy_on_->checked = shell_.setting("JOY_ENABLE", 0) != 0;
        // No joystick (port: joysticks are not read): the three joystick controls are disabled.
        joy_->enable(false), joy_rev_->enable(false), joy_on_->enable(false);
        set_focus_index(env, 1);
    }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1) return shell_.go_back(env);
        if (c.sender != 0) return;
        if (c.value == 0) {
            shell_.set_setting("MOUSE_SENS", int(std::nearbyint(mouse_->value * 100)));
            shell_.set_setting("MOUSE_REV_Y", mouse_rev_->checked);
            shell_.set_setting("JOY_SENS", int(std::nearbyint(joy_->value * 100)));
            shell_.set_setting("JOY_REV_Y", joy_rev_->checked);
            shell_.go_back(env);
        } else if (c.value == 1) {
            shell_.go_back(env);
        } else {
            mouse_->value = joy_->value = 1;
            mouse_rev_->checked = joy_rev_->checked = false;
        }
    }

private:
    Slider *mouse_, *joy_;
    CheckBox *mouse_rev_, *joy_rev_, *joy_on_;
};

// ---- Ask quit (10011ed0; listener 10012360; WM_CHAR 100123c0) ----

class AskQuitForm : public Form {
public:
    explicit AskQuitForm(Shell& s) : Form(s, kAskQuit, "Main") {
        add(title(92, 163, 540, 188, s.str(kStrAskQuit)));
        add(text_button(92, 222, 540, 247, s.str(kStrNo)));
        add(text_button(92, 194, 540, 219, s.str(kStrYes)));
    }
    void on_show(GuiEnv& env) override { set_focus_index(env, 1); }
    void on_command(GuiEnv& env, const Command& c) override {
        if (c.code == 1 || c.sender == 1) return shell_.go_back(env);
        if (c.sender == 2) shell_.exit(env, 0);
    }
    bool char_in(GuiEnv& env, unsigned char ch) override {
        if (ch == 'Y' || ch == 'y') return env.play("BUTTON_CLICK"), shell_.exit(env, 0), true;
        if (ch == 'N' || ch == 'n') return env.play("BUTTON_CLICK"), shell_.go_back(env), true;
        return Form::char_in(env, ch);
    }
};

// ---- Credits (100107f0; OnShow 10010750, Update 10010f00, Draw 10010dd0, Close 10010fc0) ----

class CreditsForm : public Form {
public:
    explicit CreditsForm(Shell& s) : Form(s, kCredits, "Crdt") {}
    void on_show(GuiEnv& env) override {
        scroller_.reset();
        if (!env.menu_font.font) return;
        // LayoutRichTextLines: wrap width round(screenW / 640 · 0.98 · 640) screen px (here font px).
        const int wrap = int(std::nearbyint(env.vs.width * 0.98f / std::max(env.vs.glyph_scale, 0.01f)));
        scroller_.emplace(layout_rich_text(shell_.config().credits, *env.menu_font.font, wrap), *env.menu_font.font);
        last_step_ = 0;
    }
    void update(GuiEnv& env) override {
        if (!scroller_) return close(env);
        const float iv = shell_.config().ctrls.credits_scroll_interval;
        if ((env.now_ms - last_step_) / 1000.0 > iv) scroller_->step(), last_step_ = env.now_ms;
        if (!scroller_->active()) close(env);
    }
    void draw(OverlayFrame& out, GuiEnv& env) override {
        draw_background(out, env);
        if (scroller_ && env.menu_font.font) {
            // The text area (42, 20)–(589, 460) in screen px; lines in font px × the glyph scale.
            const float gs = std::max(env.vs.glyph_scale, 0.01f);
            const float x0 = std::nearbyint(42 * env.sx()), y0 = std::nearbyint(20 * env.sy());
            const float x1 = std::nearbyint(589 * env.sx()), y1 = std::nearbyint(460 * env.sy());
            for (const auto& p : scroller_->layout(int((x1 - x0) / gs), int((y1 - y0) / gs)))
                ui::draw_text(out, env.vs, env.menu_font, (x0 + float(p.x) * gs) / env.sx(), (y0 + float(p.y) * gs) / env.sy(), p.line->text,
                              p.line->argb);
        }
        const std::int32_t uu = env.texture("crdt_uu"), dd = env.texture("crdt_dd");
        draw_texture(out, env, uu, 0, 0, 256, 128, 0, 0, 256, 120);
        draw_texture(out, env, uu, 0, 128, 256, 128, 256, 0, 512, 120);
        draw_texture(out, env, dd, 0, 0, 256, 128, 0, 360, 256, 480);
        draw_texture(out, env, dd, 0, 128, 256, 128, 256, 360, 512, 480);
    }
    bool key_down(GuiEnv& env, int vk) override {
        if (vk == kVkEscape || vk == kVkSpace) close(env);
        return true;
    }
    bool lbutton_down(GuiEnv& env, float, float) override { return close(env), true; }
    bool mouse_move(GuiEnv&, float, float) override { return true; }
    bool shows_cursor() const override { return false; }

private:
    void close(GuiEnv& env) {
        scroller_.reset();
        if (shell_.current() == kCredits) shell_.go_back(env);
    }
    std::optional<TextScroller> scroller_;
    double last_step_ = 0;
};

} // namespace

// ---- Form ----

void Form::set_focus_index(GuiEnv& env, int i) {
    if (Widget* old = child(focus_); old && focus_ != i) old->kill_focus(env);
    focus_ = i;
    if (Widget* n = child(i)) n->set_focus(env);
}

void Form::focus_step(GuiEnv& env, int dir) {
    const int n = int(children_.size());
    if (!n) return;
    for (int k = 1; k <= n; ++k) {
        const int i = ((focus_ < 0 ? (dir > 0 ? -1 : 0) : focus_) + dir * k % n + n * 2) % n;
        Widget* c = children_[std::size_t(i)].get();
        if (c->tab_stop && c->enabled) {
            set_focus_index(env, i);
            return;
        }
    }
}

void Form::draw_background(OverlayFrame& out, GuiEnv& env) {
    // CForm_DrawBackground 10028030: four 256² corners squashed to 256×240, the side texture as two strips.
    std::string bg = background_;
    for (char& c : bg) c = char(std::tolower((unsigned char)c));
    auto t = [&](const char* suffix) { return env.texture(bg + suffix); };
    draw_texture(out, env, t("_lu"), 0, 0, 256, 256, 0, 0, 256, 240);
    draw_texture(out, env, t("_ru"), 0, 0, 256, 256, 256, 0, 512, 240);
    draw_texture(out, env, t("_ld"), 0, 0, 256, 256, 0, 240, 256, 480);
    draw_texture(out, env, t("_rd"), 0, 0, 256, 256, 256, 240, 512, 480);
    draw_texture(out, env, t("_side"), 0, 0, 128, 256, 512, 0, 640, 240);
    draw_texture(out, env, t("_side"), 128, 0, 128, 256, 512, 240, 640, 480);
}

void Form::draw(OverlayFrame& out, GuiEnv& env) {
    draw_background(out, env);
    for (auto& c : children_) c->draw(out, env); // CGuiContainer_DrawChildren 100ca270: no visibility test
}

bool Form::key_down(GuiEnv& env, int vk) {
    Widget* f = child(focus_);
    if (f && f->enabled && f->key_down(env, vk)) return true;
    if (vk == kVkTab) return focus_step(env, 1), true;
    if (vk == kVkEscape) {
        env.play("BUTTON_CLICK");
        on_command(env, {-1, 1, 0, 0});
        return true;
    }
    if (vk == kVkUp) return focus_step(env, -1), true;
    if (vk == kVkDown) return focus_step(env, 1), true;
    return false;
}

bool Form::key_up(GuiEnv& env, int vk) {
    Widget* f = child(focus_);
    return f && f->enabled && f->key_up(env, vk);
}

bool Form::char_in(GuiEnv& env, unsigned char c) {
    Widget* f = child(focus_);
    return f && f->enabled && f->char_in(env, c);
}

void Form::update_focus_from_mouse(GuiEnv& env, float px, float py) {
    if (Widget* f = child(focus_); f && !f->hit(px, py)) {
        f->kill_focus(env);
        focus_ = -1;
    }
    for (std::size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (c->enabled && c->hit(px, py) && int(i) != focus_) {
            if (Widget* old = child(focus_)) old->kill_focus(env);
            focus_ = int(i);
            c->set_focus(env);
        }
    }
}

bool Form::mouse_move(GuiEnv& env, float px, float py) {
    update_focus_from_mouse(env, px, py);
    for (auto& c : children_)
        if (c->mouse_move(env, px, py)) return true;
    return false;
}

bool Form::lbutton_down(GuiEnv& env, float px, float py) {
    update_focus_from_mouse(env, px, py);
    Widget* f = child(focus_);
    return f && f->lbutton_down(env, px, py);
}

bool Form::rbutton_down(GuiEnv& env, float px, float py) {
    Widget* f = child(focus_);
    return f && f->rbutton_down(env, px, py);
}

bool Form::lbutton_up(GuiEnv& env, float px, float py) {
    update_focus_from_mouse(env, px, py);
    Widget* f = child(focus_);
    return f && f->lbutton_up(env, px, py);
}

// ---- ScreenTransition ----

namespace {
// Table 101034e8: rows start, end, step; columns start, end, step; init → target level; level per column, per
// row (in iteration order); level change per tick.
struct WipeRow {
    int r0, r1, rs, c0, c1, cs, init, target, col_lvl, row_lvl, tick;
};
constexpr WipeRow kWipes[16] = {
    {7, -1, -1, 0, 8, 1, 15, 0, 0, 2, -1},   {0, 8, 1, 0, 8, 1, 15, 0, 0, 2, -1},    {0, 8, 1, 0, 8, 1, 15, 0, 2, 0, -1},
    {0, 8, 1, 7, -1, -1, 15, 0, 2, 0, -1},   {0, 8, 1, 0, 8, 1, 15, 0, 2, 2, -1},    {0, 8, 1, 7, -1, -1, 15, 0, 2, 2, -1},
    {7, -1, -1, 0, 8, 1, 15, 0, 2, 2, -1},   {7, -1, -1, 7, -1, -1, 15, 0, 2, 2, -1}, {7, -1, -1, 0, 8, 1, 0, 15, 0, -2, 1},
    {0, 8, 1, 0, 8, 1, 0, 15, 0, -2, 1},     {0, 8, 1, 0, 8, 1, 0, 15, -2, 0, 1},    {0, 8, 1, 7, -1, -1, 0, 15, -2, 0, 1},
    {0, 8, 1, 0, 8, 1, 0, 15, -2, -2, 1},    {0, 8, 1, 7, -1, -1, 0, 15, -2, -2, 1}, {7, -1, -1, 0, 8, 1, 0, 15, -2, -2, 1},
    {7, -1, -1, 7, -1, -1, 0, 15, -2, -2, 1},
};
} // namespace

int ScreenTransition::start(int n, int random_pick) {
    static constexpr int kReveal[8] = {1, 0, 2, 3, 4, 5, 6, 7}, kCover[8] = {9, 8, 10, 11, 12, 13, 14, 15};
    if (n == 0x10) n = kReveal[random_pick & 7];
    else if (n == 0x11) n = kCover[random_pick & 7];
    effect_ = std::clamp(n, 0, 15);
    base_ = kWipes[effect_].init;
    last_tick_ = -1e9, done_ = false, active_ = true;
    return effect_;
}

int ScreenTransition::inverse(int n) {
    static constexpr int kInv[16] = {8, 9, 11, 10, 15, 14, 13, 12, 0, 1, 3, 2, 7, 6, 5, 4};
    return kInv[std::clamp(n, 0, 15)];
}

int ScreenTransition::level(int row, int col) const {
    const WipeRow& w = kWipes[effect_];
    const int ir = w.rs > 0 ? row : 7 - row, ic = w.cs > 0 ? col : 7 - col;
    return std::clamp(base_ + w.row_lvl * ir + w.col_lvl * ic, 0, 15);
}

void ScreenTransition::draw(OverlayFrame& out, const GuiEnv& env) {
    if (!active_) return;
    // The 8×8 cells of the overlay texture over (0,0)-(640,480): 80×60 each, black with alpha a/15.
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            if (const int a = level(r, c))
                ui::fill_rect(out, env.vs, float(c) * 80, float(r) * 60, float(c + 1) * 80, float(r + 1) * 60, std::uint32_t(a * 17) << 24);
    const WipeRow& w = kWipes[effect_];
    if (done_) return;
    if (base_ + w.row_lvl * 7 + w.col_lvl * 8 == w.target) {
        done_ = true; // the final frame stays (a finished cover is solid black)
        return;
    }
    if ((env.now_ms - last_tick_) / 1000.0 > 0.025) last_tick_ = env.now_ms, base_ += w.tick;
}

// ---- Shell ----

void Shell::start_menu_wipe() { exit_wipe_ = ScreenTransition::inverse(wipe_.start(0x10, std::rand())); }

Shell::Shell(Config cfg, int mode, const LaunchBlock& last) : cfg_(std::move(cfg)), mode_(mode), last_(last) {
    sfx_volume = float(setting("SFX_VOLUME", 25)) / 100.f;
    cd_volume = float(setting("CD_VOLUME", 25)) / 100.f;
    music_on = setting("PLAY_CD_MUSIC", 1) != 0;
    launch_.difficulty = setting("GAME_LEVEL", 1);
    build_forms();
}

Shell::~Shell() = default;

void Shell::build_forms() {
    forms_[kMainMenu] = std::make_unique<MainMenuForm>(*this);
    forms_[kSingleGame] = std::make_unique<SingleGameForm>(*this);
    forms_[kLoadGame] = std::make_unique<LoadGameForm>(*this);
    forms_[kCampaignList] = std::make_unique<CampaignListForm>(*this);
    forms_[kMissionList] = std::make_unique<MissionListForm>(*this);
    forms_[kOptions] = std::make_unique<OptionsForm>(*this);
    forms_[kVideoOptions] = std::make_unique<VideoOptionsForm>(*this);
    forms_[kAudioOptions] = std::make_unique<AudioOptionsForm>(*this);
    forms_[kRedefineControls] = make_redefine_controls_form(*this);
    forms_[kControlsTuning] = std::make_unique<ControlsTuningForm>(*this);
    forms_[kAskQuit] = std::make_unique<AskQuitForm>(*this);
    forms_[kCredits] = std::make_unique<CreditsForm>(*this);
    if (mode_ == 0) {
        // CShell::Init mode 0: logo1 → logo2 → splash → the main menu.
        forms_[kLogo1] = std::make_unique<LogoForm>(*this, kLogo1, "FirmR", cfg_.ctrls.logo1_param, kLogo2);
        forms_[kLogo2] = std::make_unique<LogoForm>(*this, kLogo2, "FirmR", cfg_.ctrls.logo2_param, kSplash);
        forms_[kSplash] = std::make_unique<LogoForm>(*this, kSplash, "IS_ER", cfg_.ctrls.splash_param, kMainMenu);
    }
}

void Shell::start(GuiEnv& env) {
    if (mode_ == 0 && cfg_.intro) show_form(env, kLogo1);
    else if (mode_ == 2) after_game(env, last_);
    else show_form(env, kMainMenu);
}

void Shell::after_game(GuiEnv& env, const LaunchBlock& last) {
    // CShell_ShowFormAfterGame 100098e0 (the win is recorded by the game side before the forms are built).
    show_form(env, kMainMenu);
    const int t = last.game_type, m = last.mode;
    std::string p = last.path;
    for (char& c : p) c = char(std::tolower((unsigned char)c));
    auto index_after_last_dot = [](std::string_view s, std::size_t end) -> std::pair<int, std::size_t> {
        const std::size_t dot = s.rfind('.', end);
        if (dot == std::string_view::npos || dot + 2 >= s.size()) return {-1, std::string_view::npos};
        return {(s[dot + 1] - '0') * 10 + (s[dot + 2] - '0'), dot};
    };
    if (last.new_mission && m == 1 && (t == 1 || t == 4)) {
        if (p.find("campaign") != std::string::npos) {
            // CShell_OpenCampaignMissionList 10009b70: "…campaign.CC/mission.MM/" → three scripted commands.
            const auto [mm, dot] = index_after_last_dot(p, p.size());
            const auto [cc, dot2] = dot == std::string::npos ? std::pair{-1, dot} : index_after_last_dot(p, dot - 1);
            if (mm < 1 || cc < 0) return;
            form(kMainMenu)->on_command(env, {2, 0, 0, 0});
            form(kSingleGame)->on_command(env, {1, 0, 0, 0});
            form(kCampaignList)->on_command(env, {0, 0, cc, mm - 1});
        } else if (p.find("single") != std::string::npos) {
            const auto [nn, dot] = index_after_last_dot(p, p.size());
            if (nn < 1) return;
            form(kMainMenu)->on_command(env, {2, 0, 0, 0});
            form(kSingleGame)->on_command(env, {2, 0, 0, nn - 1});
        }
    } else if ((last.new_mission && m == 3 && t == 1) || !last.new_mission) {
        show_form(env, kLoadGame);
    }
}

Form* Shell::form(int id) {
    auto it = forms_.find(id);
    return it == forms_.end() ? nullptr : it->second.get();
}

void Shell::show_form(GuiEnv& env, int id) {
    Form* f = form(id);
    if (!f) return;
    history_.push_back(current_);
    current_ = -1;
    f->on_show(env);
    current_ = id;
}

void Shell::go_back(GuiEnv&) {
    if (history_.empty()) return;
    const int id = history_.back();
    history_.pop_back();
    current_ = id;
}

void Shell::exit(GuiEnv& env, int code) {
    if (exit_code_ >= 0) return;
    wipe_.start(exit_wipe_, 0); // the cover matching the last reveal
    env.play("MISSION_START");
    if (code == 1) launch_.won = false, launch_.mode = 1;
    else launch_.game_type = 0;
    exit_code_ = code;
}

void Shell::update(GuiEnv& env) {
    if (exit_code_ < 0 && wipe_.done()) wipe_.deactivate(); // CShell::Run: a finished reveal stops drawing
    if (Form* f = form(current_)) f->update(env);
}

void Shell::draw(OverlayFrame& out, GuiEnv& env) {
    env.tooltip_owner = nullptr;
    if (Form* f = form(current_)) f->draw(out, env);
    // Tooltip_UpdateAndDraw 10023ba0: after the mouse rests (within 2 px) for 250 ms.
    if (std::abs(env.mouse_x - tooltip_.x) < 3 && std::abs(env.mouse_y - tooltip_.y) < 3) {
        if (env.tooltip_owner && env.now_ms - tooltip_.ms > 250) env.tooltip_owner->draw_tooltip(out, env);
    } else {
        tooltip_.x = env.mouse_x, tooltip_.y = env.mouse_y, tooltip_.ms = env.now_ms;
    }
    // ScreenTransition_Draw 1005c460: above the GUI, below the cursor.
    wipe_.draw(out, env);
    // The software cursor (Cursor_DrawCurrent 10058a50): ui/cursor.cfg ARROW, 4 frames.
    if (env.software_cursor && cursor_visible() && cfg_.cursor && env.texture) {
        const ui::HudSprite& c = *cfg_.cursor;
        const int frame = int(env.now_ms / 1000.0 / std::max(cfg_.cursor_phase, 0.01f)) % 4;
        draw_texture(out, env, env.texture(c.texture), float(c.x + frame * c.w), float(c.y), float(c.w), float(c.h), env.mouse_x, env.mouse_y,
                     env.mouse_x + float(c.w), env.mouse_y + float(c.h));
    }
}

bool Shell::cursor_visible() const {
    const Form* f = const_cast<Shell*>(this)->form(current_);
    return f && f->shows_cursor();
}

const Movie* Shell::movie() const {
    for (int id : {kLogo1, kLogo2, kSplash})
        if (id == current_)
            if (auto* l = dynamic_cast<const LogoForm*>(const_cast<Shell*>(this)->form(id))) return l->movie();
    return nullptr;
}

void Shell::key_down(GuiEnv& env, int vk) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->key_down(env, vk);
}
void Shell::key_up(GuiEnv& env, int vk) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->key_up(env, vk);
}
void Shell::char_in(GuiEnv& env, unsigned char c) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->char_in(env, c);
}
void Shell::mouse_move(GuiEnv& env, float x, float y) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->mouse_move(env, x, y);
}
void Shell::lbutton_down(GuiEnv& env, float x, float y) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->lbutton_down(env, x, y);
}
void Shell::lbutton_up(GuiEnv& env, float x, float y) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->lbutton_up(env, x, y);
}
void Shell::rbutton_down(GuiEnv& env, float x, float y) {
    if (exit_code_ >= 0) return;
    if (Form* f = form(current_)) f->rbutton_down(env, x, y);
}

int Shell::setting(std::string_view key, int fallback) const { return ini_get_int(cfg_.ini, "CS", key, fallback); }

void Shell::set_setting(std::string_view key, int value) {
    const std::string v = std::to_string(value);
    if (ini_get(cfg_.ini, "CS", key) == v) return;
    cfg_.ini = ini_set(cfg_.ini, "CS", key, v);
    effects_.settings_changed = true;
}

std::vector<ui::Campaign> Shell::campaigns() const {
    auto c = ui::scan_campaigns(cfg_.game_dir);
    ui::apply_progress(c, ui::parse_dispatcher_ini(cfg_.dispatcher));
    return c;
}

std::vector<ui::CampaignMission> Shell::single_missions() const {
    // BuildMissionList("missions/single."): single.01, .02 … up to the first folder without descr; the
    // same unlock rule as a campaign's missions.
    std::vector<ui::CampaignMission> out;
    const auto progress = ui::parse_dispatcher_ini(cfg_.dispatcher);
    for (int n = 1; n < 100; ++n) {
        char rel[64];
        std::snprintf(rel, sizeof rel, "missions/single.%02d/", n);
        const auto dir = resolve_path(cfg_.game_dir, rel);
        const auto descr = dir ? resolve_path(*dir, "descr") : std::nullopt;
        if (!descr) break;
        ui::CampaignMission m;
        m.path = rel;
        const Bytes b = read_file(*descr);
        m.title.assign(b.begin(), std::find(b.begin(), b.end(), '\n'));
        if (m.title.size() > 127) m.title.resize(127);
        while (!m.title.empty() && (m.title.back() == '\r' || m.title.back() == '\0')) m.title.pop_back();
        m.available = n == 1 || (out.back().complete);
        m.complete = m.available && (progress.all_available || progress.complete.count(ui::mission_key(m.path)));
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace parkan::shell
