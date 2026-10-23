// SWAV entries of sounds.lib / voices.lib: a plain RIFF/WAVE image, PCM 16-bit or MS-ADPCM (tag 2),
// 22050 Hz mono or stereo. The original hands the RIFF to DirectSound as is or converts it through ACM
// (Ngi32 CreateDSBufferFromRiff 1000d620). Spec: notes/audio.md §1.2, §2.3.
#pragma once

#include "core/bytes.hpp"

#include <optional>

namespace parkan {

struct Pcm {
    int rate = 0;     // nSamplesPerSec; nAvgBytesPerSec is ignored (bogus 5578 in 132 ADPCM files)
    int channels = 0; // 1 or 2
    std::vector<std::int16_t> samples; // interleaved
    std::size_t frames() const { return channels ? samples.size() / std::size_t(channels) : 0; }
    double seconds() const { return rate ? double(frames()) / rate : 0; }
};

// Decodes a RIFF/WAVE image; throws FormatError on anything but PCM16 / MS-ADPCM.
Pcm decode_swav(std::span<const std::uint8_t> riff);

// A CD-ripped music track (MUSIC/TrackNN.wav written by src/install): the 44-byte canonical header
// "RIFF" … "WAVE" "fmt " (16 bytes: PCM, 2 channels, 44100 Hz, 16-bit) "data" at offset 36. Returns the
// data size (clamped to what `file_size` holds after the header), or nullopt when the header is not that.
std::optional<std::uint32_t> music_wav_data_size(std::span<const std::uint8_t> header, std::uint64_t file_size);

// MS-ADPCM blocks (7-byte header per channel, then nibbles, high first, channels interleaved).
// `coefs` holds the predictor pairs from the fmt chunk (c1, c2, c1, c2, ...); empty = the 7 standard
// pairs. A short last block is decoded as far as it goes; the result is cut at `frames` when nonzero.
std::vector<std::int16_t> decode_ms_adpcm(std::span<const std::uint8_t> data, int channels, int block_align,
                                          std::span<const std::int16_t> coefs, std::size_t frames = 0);

} // namespace parkan
