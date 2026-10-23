// An intro movie: an AVI file (res/avi) with a Microsoft Video 1 stream (res/msvideo1) and a PCM audio
// track, stepped by a clock. The app uploads frame() whenever advance() reports a new frame and plays
// audio() once at the start. Spec: notes/shell.md "Intro videos".
#pragma once

#include "res/avi.hpp"
#include "res/msvideo1.hpp"

#include <filesystem>
#include <memory>

namespace parkan::shell {

class Movie {
public:
    // Reads and parses the whole file; throws on a malformed file or an unsupported codec.
    explicit Movie(const std::filesystem::path& path);

    int width() const { return decoder_->width(); }
    int height() const { return decoder_->height(); }
    double fps() const { return avi_.fps(); }
    std::size_t frame_count() const { return frames_.size(); }
    double duration() const { return fps() > 0 ? double(frames_.size()) / fps() : 0; }
    int audio_rate() const { return int(avi_.audio.sample_rate); }
    int audio_channels() const { return avi_.audio.channels; }
    // The audio track, interleaved 16-bit (empty without one).
    const std::vector<std::int16_t>& audio() const { return audio_; }

    // Decodes every frame up to the one shown at `seconds` (frame ⌊seconds × fps⌋); true when the shown
    // frame changed. Frames are never skipped in decoding (each one is a delta on the last).
    bool advance(double seconds);
    bool finished(double seconds) const { return seconds >= duration(); }
    int shown_frame() const { return shown_; }
    // The shown frame, top-down RGBA8.
    const Bytes& frame() const { return rgba_; }

private:
    Bytes file_;
    AviFile avi_;
    std::unique_ptr<MsVideo1Decoder> decoder_;
    std::vector<const AviChunk*> frames_;      // dc / db chunks
    std::vector<std::vector<const AviChunk*>> palettes_; // pc chunks before each frame
    std::vector<std::int16_t> audio_;
    int shown_ = -1;
    Bytes rgba_;
};

} // namespace parkan::shell
