#include "res/swav.hpp"

#include <algorithm>
#include <optional>

namespace parkan {

namespace {

constexpr std::int16_t kStdCoefs[] = {256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232};
constexpr int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

struct AdpcmState {
    int c1 = 0, c2 = 0, delta = 0, s1 = 0, s2 = 0;
    std::int16_t step(int nibble) {
        // Division (toward zero), as the reference decoder and ACM do, not an arithmetic shift.
        int pred = (s1 * c1 + s2 * c2) / 256 + (nibble >= 8 ? nibble - 16 : nibble) * delta;
        pred = std::clamp(pred, -32768, 32767);
        s2 = s1, s1 = pred;
        delta = std::clamp(kAdapt[nibble] * delta >> 8, 16, 0x7fffffff / 768); // cap: corrupt data can't overflow
        return std::int16_t(pred);
    }
};

std::int16_t le16(const std::uint8_t* p) { return std::int16_t(p[0] | p[1] << 8); }

} // namespace

std::vector<std::int16_t> decode_ms_adpcm(std::span<const std::uint8_t> data, int channels, int block_align,
                                          std::span<const std::int16_t> coefs, std::size_t frames) {
    if (channels < 1 || channels > 2 || block_align < 7 * channels)
        throw FormatError("MS-ADPCM: bad channels/block align");
    if (coefs.empty()) coefs = kStdCoefs;
    const std::size_t npred = coefs.size() / 2;
    std::vector<std::int16_t> out;
    out.reserve(data.size() * 2 + 4);
    for (std::size_t off = 0; off + std::size_t(7 * channels) <= data.size(); off += std::size_t(block_align)) {
        const std::size_t len = std::min<std::size_t>(std::size_t(block_align), data.size() - off);
        const std::uint8_t* b = data.data() + off;
        AdpcmState st[2];
        for (int c = 0; c < channels; ++c) {
            const std::size_t p = b[c];
            if (p >= npred) throw FormatError("MS-ADPCM: predictor index out of range");
            st[c].c1 = coefs[2 * p], st[c].c2 = coefs[2 * p + 1];
            st[c].delta = le16(b + channels + 2 * c);
            st[c].s1 = le16(b + 3 * channels + 2 * c);
            st[c].s2 = le16(b + 5 * channels + 2 * c);
        }
        // The header carries the first two samples, older one first.
        for (int c = 0; c < channels; ++c) out.push_back(std::int16_t(st[c].s2));
        for (int c = 0; c < channels; ++c) out.push_back(std::int16_t(st[c].s1));
        // Nibbles alternate between channels (mono: consecutive samples), high nibble first.
        int c = 0;
        for (std::size_t i = std::size_t(7 * channels); i < len; ++i) {
            out.push_back(st[c].step(b[i] >> 4));
            c = (c + 1) % channels;
            out.push_back(st[c].step(b[i] & 15));
            c = (c + 1) % channels;
        }
    }
    if (frames && out.size() > frames * std::size_t(channels)) out.resize(frames * std::size_t(channels));
    return out;
}

Pcm decode_swav(std::span<const std::uint8_t> riff) {
    Reader r(riff);
    if (r.str(4) != "RIFF") throw FormatError("SWAV: no RIFF header");
    const std::uint32_t riff_size = r.u32();
    if (r.str(4) != "WAVE") throw FormatError("SWAV: not WAVE");
    const std::size_t end = std::min<std::size_t>(riff.size(), std::size_t(riff_size) + 8);

    std::uint16_t tag = 0, channels = 0, block_align = 0, bits = 0;
    std::uint32_t rate = 0, fact = 0;
    std::vector<std::int16_t> coefs;
    std::optional<std::span<const std::uint8_t>> data;
    while (r.pos() + 8 <= end) {
        const std::string id = r.str(4);
        const std::uint32_t size = r.u32();
        const std::size_t body = r.pos();
        if (size > end - body) {
            if (id != "data") throw FormatError("SWAV: chunk '" + id + "' overruns the file");
            data = riff.subspan(body, end - body); // tolerate a slightly short last chunk
            break;
        }
        if (id == "fmt ") {
            tag = r.read<std::uint16_t>();
            channels = r.read<std::uint16_t>();
            rate = r.u32();
            r.u32(); // nAvgBytesPerSec: ignored, see audio.md §1.2
            block_align = r.read<std::uint16_t>();
            bits = r.read<std::uint16_t>();
            if (tag == 2 && size >= 20) {
                r.read<std::uint16_t>(); // cbSize
                r.read<std::uint16_t>(); // wSamplesPerBlock (derived from block_align instead)
                const std::uint16_t n = r.read<std::uint16_t>();
                if (size < 22u + 4u * n) throw FormatError("SWAV: truncated ADPCM coefficient table");
                for (int i = 0; i < 2 * n; ++i) coefs.push_back(r.read<std::int16_t>());
            }
        } else if (id == "fact" && size >= 4) {
            fact = r.u32();
        } else if (id == "data") {
            data = riff.subspan(body, size);
        }
        r.seek(std::min(end, body + size + (size & 1)));
    }
    if (!tag) throw FormatError("SWAV: no fmt chunk");
    if (!data) throw FormatError("SWAV: no data chunk");
    if (channels < 1 || channels > 2 || !rate) throw FormatError("SWAV: unsupported channel count or rate");

    Pcm pcm;
    pcm.rate = int(rate);
    pcm.channels = channels;
    if (tag == 1 && bits == 16) {
        pcm.samples.resize(data->size() / 2 / channels * channels);
        for (std::size_t i = 0; i < pcm.samples.size(); ++i) pcm.samples[i] = le16(data->data() + 2 * i);
    } else if (tag == 2 && bits == 4) {
        pcm.samples = decode_ms_adpcm(*data, channels, block_align, coefs, fact);
    } else {
        throw FormatError("SWAV: unsupported format tag " + std::to_string(tag) + ", " + std::to_string(bits) +
                          " bits");
    }
    return pcm;
}

} // namespace parkan
