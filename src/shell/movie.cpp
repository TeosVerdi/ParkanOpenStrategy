#include "shell/movie.hpp"

#include <algorithm>
#include <cmath>

namespace parkan::shell {

Movie::Movie(const std::filesystem::path& path) : file_(read_file(path)), avi_(parse_avi(file_)) {
    if (avi_.video_stream < 0) throw FormatError("movie: no video stream");
    const std::string& c = avi_.video.compression;
    if (c != "CRAM" && c != "cram" && c != "MSVC" && c != "msvc") throw FormatError("movie: unsupported codec " + c);
    decoder_ = std::make_unique<MsVideo1Decoder>(avi_.video.width, std::abs(avi_.video.height), avi_.video.bit_count);
    decoder_->palette = avi_.video.palette;
    std::vector<const AviChunk*> pending;
    for (const AviChunk& ch : avi_.video_chunks) {
        if (ch.kind == "pc") {
            pending.push_back(&ch);
            continue;
        }
        frames_.push_back(&ch);
        palettes_.push_back(std::move(pending));
        pending.clear();
    }
    if (avi_.audio_stream >= 0 && avi_.audio.format_tag == 1 && avi_.audio.bits == 16) audio_ = avi_audio_pcm16(avi_, file_);
}

bool Movie::advance(double seconds) {
    if (frames_.empty()) return false;
    const int want = std::min(int(frames_.size()) - 1, std::max(0, int(std::floor(seconds * fps()))));
    if (want <= shown_) return false;
    const std::span<const std::uint8_t> all(file_);
    while (shown_ < want) {
        ++shown_;
        for (const AviChunk* p : palettes_[std::size_t(shown_)])
            apply_avi_palette_change(decoder_->palette, all.subspan(p->offset, p->size));
        const AviChunk* f = frames_[std::size_t(shown_)];
        if (f->size) decoder_->decode(all.subspan(f->offset, f->size)); // an empty chunk repeats the last frame
    }
    decoder_->to_rgba(rgba_);
    return true;
}

} // namespace parkan::shell
