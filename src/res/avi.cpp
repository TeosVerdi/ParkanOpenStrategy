#include "res/avi.hpp"

namespace parkan {

namespace {

std::string fourcc(Reader& r) { return r.str(4); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

struct Walker {
    std::span<const std::uint8_t> file;
    AviFile& out;
    bool saw_avih = false, saw_movi = false;
    int strl_index = 0;
    std::string pending_type; // strh type of the strl being read (for its strf)

    // Calls `chunk(id, payload offset, size)` for each chunk in [begin, end), `list(type, begin, end)` for lists.
    template <class Chunk, class List>
    void each(std::size_t begin, std::size_t end, Chunk&& chunk, List&& list) {
        std::size_t p = begin;
        while (p < end) {
            if (end - p < 8) throw FormatError("avi: truncated chunk header at " + std::to_string(p));
            Reader r(file.subspan(p, 8));
            const std::string id = fourcc(r);
            const std::uint32_t size = r.u32();
            const std::size_t body = p + 8;
            if (size > end - body) throw FormatError("avi: chunk " + id + " at " + std::to_string(p) + " overruns its parent");
            if (id == "LIST" || id == "RIFF") {
                if (size < 4) throw FormatError("avi: list without a type at " + std::to_string(p));
                Reader t(file.subspan(body, 4));
                list(fourcc(t), body + 4, body + size);
            } else {
                chunk(id, body, std::size_t(size));
            }
            // Chunks are word-aligned; a final odd chunk may lack its pad byte.
            p = body + size + (size & 1);
            if (p > end) p = end;
        }
    }

    void header_chunk(const std::string& id, std::size_t off, std::size_t size) {
        Reader r(file.subspan(off, size));
        if (id == "avih") {
            if (size < 40) throw FormatError("avi: short avih");
            out.us_per_frame = r.u32();
            r.u32(); // max bytes per second
            r.u32(); // padding granularity
            out.flags = r.u32();
            out.total_frames = r.u32();
            r.u32(); // initial frames
            r.u32(); // streams
            r.u32(); // suggested buffer
            out.width = r.u32();
            out.height = r.u32();
            saw_avih = true;
        } else if (id == "strh") {
            if (size < 48) throw FormatError("avi: short strh");
            AviStreamHeader h;
            h.type = fourcc(r);
            h.handler = fourcc(r);
            h.flags = r.u32();
            r.u32(); // priority, language
            r.u32(); // initial frames
            h.scale = r.u32();
            h.rate = r.u32();
            h.start = r.u32();
            h.length = r.u32();
            h.suggested_buffer = r.u32();
            r.u32(); // quality
            h.sample_size = r.u32();
            if (!h.scale || !h.rate) throw FormatError("avi: stream with a zero rate or scale");
            pending_type = h.type;
            out.streams.push_back(h);
        } else if (id == "strf") {
            if (out.streams.size() != std::size_t(strl_index) + 1) throw FormatError("avi: strf before strh");
            const int index = strl_index;
            if (pending_type == "vids" && out.video_stream < 0) {
                if (size < 40) throw FormatError("avi: short BITMAPINFOHEADER");
                const std::uint32_t hdr = r.u32();
                if (hdr < 40 || hdr > size) throw FormatError("avi: bad BITMAPINFOHEADER size");
                out.video.width = r.i32();
                out.video.height = r.i32();
                r.read<std::uint16_t>(); // planes
                out.video.bit_count = r.read<std::uint16_t>();
                out.video.compression = fourcc(r);
                r.u32(); // image size
                r.u32();
                r.u32(); // pixels per metre
                std::uint32_t used = r.u32();
                r.u32(); // important colours
                if (out.video.bit_count <= 8) {
                    if (!used) used = 1u << out.video.bit_count;
                    r.seek(hdr);
                    if (std::size_t(used) * 4 > r.remaining()) throw FormatError("avi: truncated palette");
                    for (std::uint32_t i = 0; i < used; ++i) {
                        const auto q = r.bytes(4); // B, G, R, reserved
                        out.video.palette.push_back({q[2], q[1], q[0], 255});
                    }
                }
                out.video_stream = index;
            } else if (pending_type == "auds" && out.audio_stream < 0) {
                if (size < 16) throw FormatError("avi: short WAVEFORMAT");
                out.audio.format_tag = r.read<std::uint16_t>();
                out.audio.channels = r.read<std::uint16_t>();
                out.audio.sample_rate = r.u32();
                out.audio.bytes_per_second = r.u32();
                out.audio.block_align = r.read<std::uint16_t>();
                out.audio.bits = r.read<std::uint16_t>();
                out.audio_stream = index;
            }
        }
        // strn (stream name), strd, vprp, JUNK: ignored.
    }

    void movi_chunk(const std::string& id, std::size_t off, std::size_t size) {
        if (id == "JUNK") return;
        if (id.size() != 4 || !is_digit(id[0]) || !is_digit(id[1]))
            throw FormatError("avi: unexpected chunk '" + id + "' in movi");
        const std::uint32_t stream = std::uint32_t((id[0] - '0') * 10 + (id[1] - '0'));
        if (stream >= out.streams.size()) throw FormatError("avi: chunk " + id + " names an undeclared stream");
        AviChunk c{stream, id.substr(2), off, size};
        if (int(stream) == out.video_stream) {
            if (c.kind != "dc" && c.kind != "db" && c.kind != "pc") throw FormatError("avi: video chunk " + id);
            out.video_chunks.push_back(std::move(c));
        } else if (int(stream) == out.audio_stream) {
            if (c.kind != "wb") throw FormatError("avi: audio chunk " + id);
            out.audio_chunks.push_back(std::move(c));
        }
        // Other streams' chunks are skipped.
    }

    void movi(std::size_t begin, std::size_t end) {
        each(
            begin, end, [&](const std::string& id, std::size_t off, std::size_t size) { movi_chunk(id, off, size); },
            [&](const std::string& type, std::size_t b, std::size_t e) {
                if (type != "rec ") throw FormatError("avi: unexpected list '" + type + "' in movi");
                movi(b, e);
            });
    }
};

} // namespace

double AviFile::fps() const {
    if (video_stream >= 0) {
        const AviStreamHeader& h = streams[std::size_t(video_stream)];
        return double(h.rate) / double(h.scale);
    }
    return us_per_frame ? 1e6 / us_per_frame : 0;
}

std::size_t AviFile::frame_count() const {
    std::size_t n = 0;
    for (const AviChunk& c : video_chunks) n += c.kind != "pc";
    return n;
}

AviFile parse_avi(std::span<const std::uint8_t> file) {
    if (file.size() < 12) throw FormatError("avi: too short");
    Reader r(file);
    if (fourcc(r) != "RIFF") throw FormatError("avi: not a RIFF file");
    const std::uint32_t riff_size = r.u32();
    if (fourcc(r) != "AVI ") throw FormatError("avi: RIFF form is not 'AVI '");
    if (riff_size < 4 || riff_size > file.size() - 8) throw FormatError("avi: RIFF size exceeds the file");
    AviFile out;
    Walker w{file, out, false, false, 0, {}};
    w.each(
        12, 8 + std::size_t(riff_size),
        [&](const std::string& id, std::size_t, std::size_t) {
            if (id == "idx1") out.has_index = true;
        },
        [&](const std::string& type, std::size_t b, std::size_t e) {
            if (type == "hdrl") {
                w.each(
                    b, e, [&](const std::string& id, std::size_t off, std::size_t size) { w.header_chunk(id, off, size); },
                    [&](const std::string& t, std::size_t sb, std::size_t se) {
                        if (t != "strl") return; // odml and others
                        w.pending_type.clear();
                        w.each(
                            sb, se, [&](const std::string& id, std::size_t off, std::size_t size) { w.header_chunk(id, off, size); },
                            [](const std::string&, std::size_t, std::size_t) {});
                        if (out.streams.size() != std::size_t(w.strl_index) + 1) throw FormatError("avi: strl without strh");
                        ++w.strl_index;
                    });
            } else if (type == "movi") {
                if (!w.saw_avih) throw FormatError("avi: movi before hdrl");
                w.saw_movi = true;
                w.movi(b, e);
            }
            // INFO, PRMI, odml: skipped.
        });
    if (!w.saw_avih) throw FormatError("avi: no avih");
    if (!w.saw_movi) throw FormatError("avi: no movi list");
    return out;
}

std::vector<std::int16_t> avi_audio_pcm16(const AviFile& avi, std::span<const std::uint8_t> file) {
    if (avi.audio_stream < 0) return {};
    if (avi.audio.format_tag != 1 || avi.audio.bits != 16 || !avi.audio.channels)
        throw FormatError("avi: audio is not 16-bit PCM");
    std::size_t total = 0;
    for (const AviChunk& c : avi.audio_chunks) total += c.size / 2;
    std::vector<std::int16_t> out(total);
    std::size_t at = 0;
    for (const AviChunk& c : avi.audio_chunks) {
        std::memcpy(out.data() + at, file.data() + c.offset, c.size / 2 * 2);
        at += c.size / 2;
    }
    // Whole frames only.
    out.resize(out.size() / avi.audio.channels * avi.audio.channels);
    return out;
}

void apply_avi_palette_change(std::vector<std::array<std::uint8_t, 4>>& palette, std::span<const std::uint8_t> chunk) {
    Reader r(chunk);
    const unsigned first = r.u8();
    unsigned count = r.u8();
    r.read<std::uint16_t>(); // flags
    if (!count) count = 256;
    if (first + count > 256) throw FormatError("avi: palette change past entry 255");
    if (palette.size() < first + count) palette.resize(first + count, {0, 0, 0, 255});
    for (unsigned i = 0; i < count; ++i) {
        const auto e = r.bytes(4); // R, G, B, flags
        palette[first + i] = {e[0], e[1], e[2], 255};
    }
}

} // namespace parkan
