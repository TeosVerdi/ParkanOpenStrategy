#include "ui/hud_text.hpp"

#include <cmath>
#include <cstdlib>

namespace parkan::ui {

namespace {

void set_argb(OverlayQuad& q, std::uint32_t argb) {
    q.rgba[0] = std::uint8_t(argb >> 16), q.rgba[1] = std::uint8_t(argb >> 8), q.rgba[2] = std::uint8_t(argb);
    q.rgba[3] = std::uint8_t(argb >> 24);
}

} // namespace

int nearest_font_set(float width, float height) {
    int best = 640;
    float best_d = 1e30f;
    for (const auto [w, h] : {std::pair{640, 480}, {800, 600}, {1024, 768}}) {
        const float d = std::abs(width - float(w)) + std::abs(height - float(h));
        if (d < best_d) best_d = d, best = w;
    }
    return best;
}

float text_width_px(const VirtualScreen& vs, const TftFont& f, std::string_view text) {
    return float(f.text_width(text)) * vs.glyph_scale;
}

void draw_text(OverlayFrame& out, const VirtualScreen& vs, HudFont hf, float x, float y, std::string_view text, std::uint32_t argb) {
    if (!hf.font) return;
    const TftFont& f = *hf.font;
    // CNgiFont::DrawText: each glyph is a (w+1) × (h+1) quad; the pen advances by w + spacing. The
    // GUI server always passes one flat colour, made opaque by SetColors.
    const float s = vs.glyph_scale;
    float pen = std::floor(x * vs.sx());
    const float top = std::floor(y * vs.sy()), bottom = top + float(f.height + 1) * s;
    for (unsigned char c : text) {
        const TftFont::Glyph& g = f.glyphs[c];
        OverlayQuad q{pen, top, pen + float(g.w + 1) * s, bottom};
        q.u0 = g.u0, q.u1 = g.u1, q.v0 = g.v0, q.v1 = g.v0 + f.v_height;
        q.texture = hf.texture;
        set_argb(q, argb | 0xff000000u);
        out.quads.push_back(q);
        pen += float(g.w + f.spacing) * s;
    }
}

void fill_rect(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb) {
    OverlayQuad q{std::floor(x0 * vs.sx()), std::floor(y0 * vs.sy()), std::floor(x1 * vs.sx()), std::floor(y1 * vs.sy())};
    set_argb(q, argb);
    out.quads.push_back(q);
}

std::vector<std::string> wrap_text(const VirtualScreen& vs, const TftFont& f, std::string_view text, float max_px) {
    std::vector<std::string> lines;
    std::string line;
    bool open = false;
    auto flush = [&] {
        lines.push_back(line);
        line.clear();
        open = false;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\r') {
            ++i;
            continue;
        }
        if (text[i] == '\n') {
            flush();
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '\r') ++j;
        const std::string_view word = text.substr(i, j - i);
        i = j < text.size() && text[j] == ' ' ? j + 1 : j;
        if (word.empty()) continue;
        const std::string candidate = open ? line + " " + std::string(word) : std::string(word);
        if (open && text_width_px(vs, f, candidate) > max_px) {
            flush();
            line = std::string(word);
        } else {
            line = candidate;
        }
        open = true;
    }
    if (open || lines.empty()) flush();
    return lines;
}

void draw_pager(OverlayFrame& out, const VirtualScreen& vs, HudFont gf, const PagerMessage& m, bool hq) {
    if (!gf.font) return;
    const TftFont& f = *gf.font;
    const float X0 = hq ? 374.f : 230.f, Y0 = hq ? 352.f : 0.f, W = hq ? 266.f : 182.f;
    // Line step in virtual pixels: (GetHeight() + 2) screen pixels.
    const float L = std::round(float(f.height + 2) * vs.glyph_scale / vs.sy());
    std::vector<std::string> lines = wrap_text(vs, f, m.body, std::round(vs.sx() * 0.98f * W));
    bool cut = false;
    if (lines.size() > kPagerMaxLines) {
        lines.resize(kPagerMaxLines);
        std::string& last = lines.back();
        while (!last.empty() && last.back() == ' ') last.pop_back();
        while (!last.empty() && text_width_px(vs, f, last + "...") > vs.sx() * 0.98f * W) last.pop_back();
        last += "...";
        cut = true;
    }
    const bool footer = cut && !m.more_footer.empty();
    const float k = footer ? 2.f : 1.f, n = float(lines.size());
    // DrawFramedPanel(rect, 0x80008000): UI frame style 10 around a 50% dark-green fill. The frame art
    // is not decoded; a one-pixel lighter border stands in for it.
    const float x1 = X0 + W, y1 = Y0 + k * L / 2 + 16 + (k + n) * L;
    fill_rect(out, vs, X0, Y0, x1, y1, 0x80008000u);
    const float bx = 1.f / vs.sx(), by = 1.f / vs.sy();
    fill_rect(out, vs, X0, Y0, x1, Y0 + by, 0xc040c040u);
    fill_rect(out, vs, X0, y1 - by, x1, y1, 0xc040c040u);
    fill_rect(out, vs, X0, Y0, X0 + bx, y1, 0xc040c040u);
    fill_rect(out, vs, x1 - bx, Y0, x1, y1, 0xc040c040u);
    draw_text(out, vs, gf, X0 + 8, Y0 + 9, m.header, 0xffffffffu);
    float y = Y0 + 9 + 3 * L / 2;
    for (const auto& line : lines) {
        draw_text(out, vs, gf, X0 + 8, y, line, 0xffdcdcdcu);
        y += L;
    }
    if (footer) draw_text(out, vs, gf, X0 + 8, y + L / 2, m.more_footer, 0xffffffffu);
}

void draw_objectives(OverlayFrame& out, const VirtualScreen& vs, HudFont mf, const ObjectivesScreen& s) {
    if (!mf.font) return;
    const TftFont& f = *mf.font;
    fill_rect(out, vs, 0, 0, 640, 480, s.dark ? 0xcc000000u : 0x99000000u);
    // Centred on x = 320; GetTextWidth is in screen pixels, hence the division by ScaleX.
    auto centred = [&](float y, const std::string& text, std::uint32_t argb) {
        draw_text(out, vs, mf, 320 - text_width_px(vs, f, text) / vs.sx() / 2, y, text, argb);
    };
    // FormatObjectiveLineColor 1006b3b0: "%s : %s", coloured by state. The bonus lines are assumed to
    // follow the same rule (notes/rsli-font.md §5).
    auto line = [&](float y, const ObjectivesScreen::Line& l) {
        const int i = l.state > 0 ? 0 : l.state < 0 ? 1 : 2;
        static constexpr std::uint32_t colour[3] = {0xffebebebu, 0xffff6464u, 0xff787878u};
        centred(y, l.text + " : " + s.state_word[i], colour[i]);
    };
    centred(80, s.title, 0xff00ff00u);
    float y = 110;
    for (const auto& l : s.primary) line(y, l), y += 20;
    if (!s.bonus.empty()) {
        centred(260, s.bonus_title, 0xff00ff00u);
        y = 290;
        for (const auto& l : s.bonus) line(y, l), y += 20;
    }
    centred(450, s.footer, 0xffffffffu);
}

} // namespace parkan::ui
