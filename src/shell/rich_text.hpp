// The game's rich-text lines and the credits scroller: LayoutRichTextLines 10093630 (tokens
// NextRichTextToken 10092d20, tags ParseRichTextTag 10092fc0) and CTextScroller (1006bda0: Draw 1006bf30,
// StepPixel 1006c090, IsActive 1006c140) with the credits' aligned-line renderer (100221e0).
// Spec: notes/shell.md "Credits".
#pragma once

#include "res/tft.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace parkan::shell {

struct RichLine {
    std::string text;            // cp1251
    std::uint32_t argb = 0xffffffffu;
    int align = 1;               // 1 left, 2 centre, 3 right
    int spacing = 100;           // <<interleave=N>>: row height in % of the font height
};

// Tags: <<color=r,g,b>> / <</color>>, <<align=left|center|right>> / <</align>>, <<interleave=N>> /
// <</interleave>>, <<br>>; a push also sets the current line, a pop takes effect from the next one.
// `wrap_px` is the layout width in font pixels (the original: round(screenW / 640 · 0.98 · 640)).
std::vector<RichLine> layout_rich_text(std::string_view text, const TftFont& font, int wrap_px);

// CTextScroller as the credits use it, in font pixels (screen pixels in the original).
class TextScroller {
public:
    TextScroller(std::vector<RichLine> lines, const TftFont& font) : lines_(std::move(lines)), font_(&font) {}

    // H(L) = height × spacing / 100 (integer division).
    int row_height(const RichLine& l) const { return font_->height * l.spacing / 100; }
    // StepPixel: the text moves up one pixel.
    void step();
    // IsActive: first < n − drawn (the rows drawn by the last layout() call).
    bool active() const { return first_ < int(lines_.size()) - drawn_; }

    struct Placed {
        const RichLine* line;
        int x, y; // top-left, font pixels from the area's origin
    };
    // Draw: the lines from the first visible one while y < height, aligned in `width`; also records the
    // count for active().
    std::vector<Placed> layout(int width, int height);

private:
    std::vector<RichLine> lines_;
    const TftFont* font_;
    int first_ = 0, offset_ = 0, drawn_ = 0;
};

} // namespace parkan::shell
