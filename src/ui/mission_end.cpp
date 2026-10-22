#include "ui/mission_end.hpp"

#include "core/bytes.hpp"
#include "game/mission_runtime.hpp"
#include "res/textres.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace parkan::ui {

EndStrings load_end_strings(const TextRes& t) {
    auto s = [&](std::uint32_t id) { return utf8_to_cp1251(t.get(id)); };
    return {s(1012), s(1013), s(6225), s(5083), s(5082), s(3075), s(3076), s(3077)};
}

EndLayout layout_end_screen(const VirtualScreen& vs, const TftFont& gf, const TftFont& mf, const EndScreen& s, const EndStrings& str) {
    EndLayout l;
    if (s.state == GameState::Running) return l;
    l.visible = true;
    // DrawGameStateMessage 1009f850: a title (MENU_FONT, green unless failed) and four line slots
    // (GAME_FONT, 0xFFF0F0F0); empty slots are skipped.
    std::string title, line[4];
    std::uint32_t title_argb = kEndTitleGreen, line_argb[4] = {kEndLineColour, kEndLineColour, kEndLineColour, kEndLineColour};
    bool warning = false;
    switch (s.state) {
    case GameState::Complete:
        title = str.complete, line[0] = str.press_esc;
        break;
    case GameState::Failed:
        title = str.failed, line[0] = str.press_esc, line[1] = str.press_r, line[2] = str.press_l;
        title_argb = kEndTitleRed;
        break;
    case GameState::ConnectionLost:
        title = str.connection_lost, line[0] = str.press_esc;
        break;
    case GameState::Leaving:
        title = str.leaving;
        break;
    case GameState::Running:
        break;
    }
    // Complete / Failed in multiplayer: no restart or load; the host is warned in red, with 15 extra pixels
    // between lines.
    if (s.multiplayer && (s.state == GameState::Complete || s.state == GameState::Failed)) {
        line[1].clear(), line[2].clear();
        if (s.host) line[3] = str.server_warning, line_argb[3] = kEndWarningColour, warning = true;
    }
    // L = round((GetHeight(GAME_FONT) + 2) / ScaleY + extra), in virtual pixels.
    l.line_step = std::nearbyint(float(gf.height + 2) * vs.glyph_scale / vs.sy() + (warning ? kEndHostExtraStep : 0.f));
    // Centred on x = 320: GetTextWidth is in screen pixels, hence / (2·ScaleX).
    auto centre_x = [&](const TftFont& f, const std::string& text) {
        return 320.f - std::nearbyint(text_width_px(vs, f, text) / (2 * vs.sx()));
    };
    float min_x = centre_x(mf, title), y = kEndLinesY;
    for (int i = 0; i < 4; ++i) {
        if (line[i].empty()) continue;
        const float x = centre_x(gf, line[i]);
        min_x = std::min(min_x, x);
        l.lines.push_back({line[i], x, y, line_argb[i]});
        y += l.line_step;
    }
    if (s.state == GameState::Leaving) {
        l.rect[0] = 0, l.rect[1] = 0, l.rect[2] = 640, l.rect[3] = 480;
    } else {
        // The box hugs the widest text with 30 px on each side (symmetric about x = 320).
        const float x0 = std::max(0.f, min_x - kEndRectPadX);
        l.rect[0] = x0, l.rect[1] = kEndRectY0, l.rect[2] = 640 - x0, l.rect[3] = y + kEndRectPadBottom;
    }
    if (!title.empty()) l.title = EndText{title, centre_x(mf, title), kEndTitleY, title_argb};
    return l;
}

void draw_end_screen(OverlayFrame& out, const VirtualScreen& vs, HudFont gf, HudFont mf, const EndScreen& s, const EndStrings& str) {
    if (!gf.font || !mf.font) return;
    const EndLayout l = layout_end_screen(vs, *gf.font, *mf.font, s, str);
    if (!l.visible) return;
    fill_rect(out, vs, l.rect[0], l.rect[1], l.rect[2], l.rect[3], l.rect_argb);
    if (l.title) draw_text(out, vs, mf, l.title->x, l.title->y, l.title->text, l.title->argb);
    for (const EndText& t : l.lines) draw_text(out, vs, gf, t.x, t.y, t.text, t.argb);
}

const char* MissionEndController::mission_failed() {
    if (state_ == GameState::Failed) return nullptr;
    state_ = GameState::Failed, won_ = false;
    return kVoiceMissionFail;
}

const char* MissionEndController::mission_complete() {
    if (state_ == GameState::Complete) return nullptr;
    state_ = GameState::Complete, won_ = true;
    return kVoiceMissionComplete;
}

void MissionEndController::briefing_only_finished() {
    won_ = true;
    leave(EndAction::ToShell);
}

EndAction MissionEndController::on_escape() {
    // OnGameKeyDown: autodemo leaves on any Esc; otherwise any state but Running leaves to the shell.
    if (autodemo_ || state_ != GameState::Running) leave(EndAction::ToShell);
    else return EndAction::None;
    return action_;
}

EndAction MissionEndController::on_char(unsigned char ch) {
    if (multiplayer_ || (state_ != GameState::Failed && state_ != GameState::ConnectionLost)) return EndAction::None;
    switch (ch) {
    case 'L': case 'l': case 0xc4: case 0xe4: leave(EndAction::LoadGame); return action_;
    case 'R': case 'r': case 0xca: case 0xea: leave(EndAction::Restart); return action_;
    default: return EndAction::None;
    }
}

std::map<std::string, std::string> parse_voice_table(std::string_view cfg) {
    std::map<std::string, std::string> out;
    for (const auto& o : game::parse_cfg(cfg)) {
        std::string name = o.name;
        for (char& c : name) c = char(std::tolower((unsigned char)c));
        if (name != "voices") continue;
        for (const auto& [k, v] : o.entries)
            if (k != "library" && k != "desc" && k != "libtype" && k != "type") out[k] = v;
    }
    return out;
}

} // namespace parkan::ui
