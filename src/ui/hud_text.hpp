// In-mission text: the message panel ("pager") and the objectives screen, drawn with the game's TFT
// fonts on the 640×480 virtual screen. Spec: notes/rsli-font.md §2.3–2.4 and §3.
//
// Port decisions:
// - The original never scales glyphs: it picks the font set nearest the display size. The port picks
//   by the window's size in points and scales glyphs by the pixels-per-point factor (Retina), so text
//   keeps its 2001 proportions on a high-density screen.
// - The D3D half-texel offset (−0.5) is not applied: Metal samples texel centres at pixel centres.
#pragma once

#include "res/tft.hpp"
#include "ui/overlay.hpp"

#include <string>

namespace parkan::ui {

struct HudFont {
    const TftFont* font = nullptr;
    std::int32_t texture = -1; // overlay texture slot of the font's atlas
};

// The virtual screen on the drawable: CGUIServer::bScaleTo640 scales positions by width/640, height/480.
struct VirtualScreen {
    float width = 640, height = 480; // drawable pixels
    float glyph_scale = 1;           // drawable pixels per font pixel
    float sx() const { return width / 640.f; }
    float sy() const { return height / 480.f; }
};

// ResolveFontSubstitute 10008590: the 640/800/1024 set nearest in |Δw| + |Δh|; returns 640, 800 or 1024.
int nearest_font_set(float width, float height);

// Single-byte (cp1251) text at virtual (x, y), top-left; `argb` multiplies the glyph texels.
void draw_text(OverlayFrame& out, const VirtualScreen& vs, HudFont f, float x, float y, std::string_view text, std::uint32_t argb);
// Width in drawable pixels (GetTextWidth × the glyph scale).
float text_width_px(const VirtualScreen& vs, const TftFont& f, std::string_view text);
void fill_rect(OverlayFrame& out, const VirtualScreen& vs, float x0, float y0, float x1, float y1, std::uint32_t argb);

// WrapTextToLines 100933b0: words packed while the line's width ≤ `max_px` drawable pixels. The
// tokenizer is inferred: split on spaces, a newline forces a break, a line drops its leading space.
std::vector<std::string> wrap_text(const VirtualScreen& vs, const TftFont& f, std::string_view text, float max_px);

// The pager (LayoutPagerText 1007fef0, DrawPagerPanel 100802a0). Strings are cp1251.
struct PagerMessage {
    std::string header, body;
    std::string more_footer; // 6208 "Press %s to view" with the key filled in, used when the body is cut
    double shown_ms = 0;
};
inline constexpr double kPagerLifetimeMs = 20000; // UpdateDrawPagerTimeout 1007f910
inline constexpr std::size_t kPagerMaxLines = 6;
// `hq`: the HQ view or the briefing (the panel sits bottom right instead of top centre).
void draw_pager(OverlayFrame& out, const VirtualScreen& vs, HudFont game_font, const PagerMessage& m, bool hq = false);

// The objectives screen (DrawObjectivesScreen 1006adf0). Strings are cp1251.
struct ObjectivesScreen {
    std::string title, bonus_title, footer;
    std::string state_word[3]; // complete (1014), failed (1027), in progress (1015)
    struct Line {
        std::string text;
        int state = 0; // 1 complete, −1 failed, 0 in progress
    };
    std::vector<Line> primary, bonus;
    bool dark = false; // views 3/4/5: 80% backdrop instead of 60%
};
inline constexpr double kObjectivesAutoHideMs = 7000;
void draw_objectives(OverlayFrame& out, const VirtualScreen& vs, HudFont menu_font, const ObjectivesScreen& s);

} // namespace parkan::ui
