// AVI (RIFF 'AVI ') movies: the shell's intro videos (DATA/nikita.avi, DATA/is_logo.avi and, in the ENG
// release, DATA/montecristo.avi). The original hands the file to Video for Windows; the port reads the
// container itself. Spec: notes/shell.md "Intro videos".
//
// What the shipped files use (both releases): one video stream (vids, CRAM = Microsoft Video 1, 16 bit,
// 640×480, 24 or 25 fps) and one audio stream (auds, PCM 16-bit stereo 22050 Hz); chunks are either
// directly in `movi` or grouped in `rec ` lists (montecristo.avi), with an idx1 index. The reader is
// strict: every RIFF size must fit its parent, `hdrl`/`avih`/`movi` must be present and each data chunk
// must name a declared stream. JUNK and unknown chunks outside `movi` are skipped.
//
// Port decisions:
// - The data chunks are taken from the `movi` walk in file order; idx1 is only checked for presence (its
//   keyframe flags are not needed for sequential playback).
// - '##db' (uncompressed DIB) chunks of a CRAM stream are decoded as CRAM: every file starts with one
//   such chunk whose size (38402 bytes = 19 201 two-byte codes) only fits the codec, not a raw 640×480 DIB.
#pragma once

#include "core/bytes.hpp"

#include <array>

namespace parkan {

struct AviStreamHeader {
    std::string type;    // "vids" / "auds"
    std::string handler; // e.g. "msvc", "MSVC"; four NULs for audio
    std::uint32_t flags = 0, scale = 1, rate = 0, start = 0, length = 0, suggested_buffer = 0, sample_size = 0;
};

struct AviVideoFormat { // BITMAPINFOHEADER
    std::int32_t width = 0, height = 0; // height > 0: bottom-up rows
    std::uint16_t bit_count = 0;
    std::string compression;             // e.g. "CRAM"
    std::vector<std::array<std::uint8_t, 4>> palette; // RGBQUAD as R, G, B, 255 (8-bit streams)
};

struct AviAudioFormat { // WAVEFORMATEX
    std::uint16_t format_tag = 0, channels = 0, block_align = 0, bits = 0;
    std::uint32_t sample_rate = 0, bytes_per_second = 0;
};

// One data chunk of the movi list: 'NNdc' compressed frame, 'NNdb' uncompressed frame, 'NNpc' palette
// change, 'NNwb' audio.
struct AviChunk {
    std::uint32_t stream = 0;
    std::string kind; // "dc", "db", "pc", "wb"
    std::size_t offset = 0, size = 0; // of the payload, in the file image
};

struct AviFile {
    std::uint32_t us_per_frame = 0, total_frames = 0, flags = 0, width = 0, height = 0;
    std::vector<AviStreamHeader> streams;
    int video_stream = -1, audio_stream = -1; // first of each type
    AviVideoFormat video;
    AviAudioFormat audio;
    std::vector<AviChunk> video_chunks; // dc / db / pc of the video stream, in file order
    std::vector<AviChunk> audio_chunks; // wb of the audio stream
    bool has_index = false;             // idx1 present

    double fps() const;
    std::size_t frame_count() const; // video dc/db chunks
};

// Throws FormatError on anything malformed.
AviFile parse_avi(std::span<const std::uint8_t> file);

// The audio stream's PCM payload concatenated (PCM 16-bit only; throws FormatError otherwise).
std::vector<std::int16_t> avi_audio_pcm16(const AviFile& avi, std::span<const std::uint8_t> file);

// AVIPALCHANGE (a 'NNpc' chunk): bFirstEntry, bNumEntries (0 = 256), wFlags, PALETTEENTRY[] (R, G, B, flags).
// Applies it to `palette` (grown to 256 entries as needed).
void apply_avi_palette_change(std::vector<std::array<std::uint8_t, 4>>& palette, std::span<const std::uint8_t> chunk);

} // namespace parkan
