#include "shell/rich_text.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace parkan::shell {

namespace {

bool iprefix(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)p[i])) return false;
    return true;
}

} // namespace

std::vector<RichLine> layout_rich_text(std::string_view text, const TftFont& font, int wrap_px) {
    std::vector<RichLine> out;
    std::vector<std::uint32_t> colours;
    std::vector<int> aligns, spacings;
    std::size_t p = 0;
    while (p < text.size()) {
        RichLine line;
        line.argb = colours.empty() ? 0xffffffffu : colours.back();
        line.align = aligns.empty() ? 1 : aligns.back();
        line.spacing = spacings.empty() ? 100 : spacings.back();
        int width = 0;
        bool end_line = false;
        while (p < text.size() && !end_line) {
            const std::size_t start = p;
            if (text[p] == '\n') { // kind 4: ignored
                ++p;
                continue;
            }
            if (text.compare(p, 2, "<<") == 0) { // kind 5: a tag
                const std::size_t close = text.find(">>", p + 2);
                const std::size_t e = close == std::string_view::npos ? text.size() : close;
                const std::string_view tag = text.substr(p + 2, e - p - 2);
                p = close == std::string_view::npos ? text.size() : close + 2;
                // ParseRichTextTag: case-sensitive prefixes; values split on " ,".
                if (tag.rfind("color=", 0) == 0) {
                    int rgb[3] = {0, 0, 0}, n = 0;
                    std::string v(tag.substr(6));
                    for (char* tok = std::strtok(v.data(), " ,"); tok && n < 3; tok = std::strtok(nullptr, " ,")) rgb[n++] = std::atoi(tok);
                    colours.push_back(0xff000000u | std::uint32_t(rgb[0] & 255) << 16 | std::uint32_t(rgb[1] & 255) << 8 | std::uint32_t(rgb[2] & 255));
                    line.argb = colours.back();
                } else if (tag == "/color") {
                    if (!colours.empty()) colours.pop_back();
                } else if (tag.rfind("align=", 0) == 0) {
                    const std::string_view v = tag.substr(6);
                    aligns.push_back(iprefix(v, "center") ? 2 : iprefix(v, "right") ? 3 : 1);
                    line.align = aligns.back();
                } else if (tag == "/align") {
                    if (!aligns.empty()) aligns.pop_back();
                } else if (tag.rfind("interleave=", 0) == 0) {
                    spacings.push_back(std::atoi(std::string(tag.substr(11)).c_str()));
                    line.spacing = spacings.back();
                } else if (tag == "/interleave") {
                    if (!spacings.empty()) spacings.pop_back();
                } else if (iprefix(tag, "br") && tag.size() == 2) {
                    end_line = true;
                }
                continue;
            }
            std::size_t e = p;
            if (text[p] == ' ') { // kind 2: a run of spaces
                while (e < text.size() && text[e] == ' ') ++e;
                if (line.text.empty()) { // leading spaces are dropped
                    p = e;
                    continue;
                }
            } else { // kind 1: a word, up to the next space or tag (an embedded '\n' stays in it)
                while (e < text.size() && text[e] != ' ' && text.compare(e, 2, "<<") != 0) ++e;
            }
            const std::string_view tok = text.substr(p, e - p);
            const int w = font.text_width(tok);
            if (width + w > wrap_px && !line.text.empty()) {
                p = start; // rewind: the token starts the next line
                break;
            }
            width += w;
            line.text += tok;
            p = e;
        }
        if (line.text.empty()) line.text = " ";
        out.push_back(std::move(line));
    }
    return out;
}

void TextScroller::step() {
    --offset_;
    if (first_ < int(lines_.size()) && offset_ < -row_height(lines_[std::size_t(first_)])) {
        if (first_ + 1 <= int(lines_.size()) - drawn_) ++first_;
        offset_ = 0;
    }
}

std::vector<TextScroller::Placed> TextScroller::layout(int width, int height) {
    std::vector<Placed> out;
    drawn_ = 0;
    int y = offset_;
    for (int i = first_; i < int(lines_.size()) && y < height; ++i) {
        const RichLine& l = lines_[std::size_t(i)];
        const int tw = font_->text_width(l.text);
        int off = 0;
        if (l.align == 2) off = int(std::nearbyint(float(width / 2) - float(tw) * 0.5f));
        else if (l.align == 3) off = width - tw;
        out.push_back({&l, off, y});
        y += row_height(l);
        ++drawn_;
    }
    return out;
}

} // namespace parkan::shell
