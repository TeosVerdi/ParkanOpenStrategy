#include "install/inflate.hpp"

#include "core/bytes.hpp"

#include <cstring>

namespace parkan::install {

namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;

// Canonical Huffman code: `count`/`sym` drive the bit-serial decoder (RFC 1951 §3.2.2, as in zlib's
// puff), `fast` resolves codes of up to kFastBits bits in one lookup ((symbol << 4) | length, 0 = slow).
struct Huffman {
    std::uint16_t count[kMaxBits + 1];
    std::uint16_t sym[320];
    std::uint16_t fast[1 << kFastBits];
};

void build(Huffman& h, const std::uint8_t* len, int n) {
    std::memset(h.count, 0, sizeof h.count);
    std::memset(h.fast, 0, sizeof h.fast);
    for (int s = 0; s < n; ++s) ++h.count[len[s]];
    if (h.count[0] == n) return; // no codes (a distance tree with only literals)
    int left = 1;
    for (int l = 1; l <= kMaxBits; ++l) {
        left = (left << 1) - h.count[l];
        if (left < 0) throw FormatError("inflate: over-subscribed code");
    }
    std::uint16_t offs[kMaxBits + 2] = {0, 0};
    for (int l = 1; l <= kMaxBits; ++l) offs[l + 1] = std::uint16_t(offs[l] + h.count[l]);
    std::uint16_t next[kMaxBits + 1];
    int code = 0;
    for (int l = 1; l <= kMaxBits; ++l) {
        code = (code + h.count[l - 1] * (l > 1)) << 1;
        next[l] = std::uint16_t(code);
    }
    for (int s = 0; s < n; ++s) {
        const int l = len[s];
        if (!l) continue;
        h.sym[offs[l]++] = std::uint16_t(s);
        const int c = next[l]++;
        if (l > kFastBits) continue;
        int rev = 0;
        for (int i = 0; i < l; ++i) rev |= ((c >> i) & 1) << (l - 1 - i);
        for (int j = rev; j < (1 << kFastBits); j += 1 << l) h.fast[j] = std::uint16_t(s << 4 | l);
    }
}

class Inflater {
public:
    Inflater(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t max_out)
        : p_(in.data()), n_(in.size()), out_(out), start_(out.size()), max_out_(max_out) {}

    void run() {
        for (;;) {
            const bool last = bits(1);
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: throw FormatError("inflate: bad block type");
            }
            if (last || (consumed() + 7) / 8 >= n_) return;
        }
    }

private:
    std::uint64_t consumed() const { return std::uint64_t(pos_) * 8 - cnt_; }

    void refill() {
        while (cnt_ <= 56) {
            buf_ |= std::uint64_t(pos_ < n_ ? p_[pos_] : 0) << cnt_;
            ++pos_;
            cnt_ += 8;
        }
    }
    void check_overrun() const {
        if (consumed() > std::uint64_t(n_) * 8) throw FormatError("inflate: truncated input");
    }
    std::uint32_t bits(int n) {
        if (cnt_ < n) refill();
        const auto v = std::uint32_t(buf_ & ((std::uint64_t(1) << n) - 1));
        buf_ >>= n;
        cnt_ -= n;
        check_overrun();
        return v;
    }

    int decode(const Huffman& h) {
        if (cnt_ < kMaxBits) refill();
        if (const std::uint16_t e = h.fast[buf_ & ((1u << kFastBits) - 1)]) {
            buf_ >>= e & 15;
            cnt_ -= e & 15;
            check_overrun();
            return e >> 4;
        }
        std::uint64_t b = buf_;
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= kMaxBits; ++l) {
            code |= int(b & 1);
            b >>= 1;
            const int count = h.count[l];
            if (code - count < first) {
                buf_ >>= l;
                cnt_ -= l;
                check_overrun();
                return h.sym[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw FormatError("inflate: bad code");
    }

    void grow(std::size_t n) {
        if (out_.size() - start_ + n > max_out_) throw FormatError("inflate: output larger than expected");
    }

    void stored() {
        // Drop to a byte boundary, then rewind the bit buffer to the byte stream.
        buf_ >>= cnt_ % 8;
        cnt_ -= cnt_ % 8;
        pos_ -= cnt_ / 8;
        buf_ = 0, cnt_ = 0;
        if (pos_ + 4 > n_) throw FormatError("inflate: truncated stored block");
        const unsigned len = p_[pos_] | p_[pos_ + 1] << 8, nlen = p_[pos_ + 2] | p_[pos_ + 3] << 8;
        if (len != (~nlen & 0xffff)) throw FormatError("inflate: stored block length mismatch");
        pos_ += 4;
        if (pos_ + len > n_) throw FormatError("inflate: truncated stored block");
        grow(len);
        out_.insert(out_.end(), p_ + pos_, p_ + pos_ + len);
        pos_ += len;
    }

    void codes(const Huffman& lit, const Huffman& dist) {
        static constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                        33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int s = decode(lit);
            if (s < 256) {
                grow(1);
                out_.push_back(std::uint8_t(s));
                continue;
            }
            if (s == 256) return;
            s -= 257;
            if (s >= 29) throw FormatError("inflate: bad length symbol");
            const std::size_t len = kLenBase[s] + bits(kLenExtra[s]);
            const int d = decode(dist);
            if (d >= 30) throw FormatError("inflate: bad distance symbol");
            const std::size_t back = kDistBase[d] + bits(kDistExtra[d]);
            if (back > out_.size() - start_) throw FormatError("inflate: distance too far back");
            grow(len);
            const std::size_t o = out_.size();
            out_.resize(o + len);
            std::uint8_t* w = out_.data() + o;
            const std::uint8_t* r = w - back;
            for (std::size_t i = 0; i < len; ++i) w[i] = r[i];
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            std::uint8_t len[288];
            for (int s = 0; s < 288; ++s) len[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
            build(t.first, len, 288);
            std::memset(len, 5, 30);
            build(t.second, len, 30);
            return t;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = int(bits(5)) + 257, ndist = int(bits(5)) + 1, ncode = int(bits(4)) + 4;
        if (nlen > 286 || ndist > 30) throw FormatError("inflate: bad code counts");
        std::uint8_t len[320] = {};
        for (int i = 0; i < ncode; ++i) len[kOrder[i]] = std::uint8_t(bits(3));
        Huffman lencode, lit, dist;
        build(lencode, len, 19);
        std::memset(len, 0, sizeof len);
        for (int i = 0; i < nlen + ndist;) {
            const int s = decode(lencode);
            if (s < 16) {
                len[i++] = std::uint8_t(s);
                continue;
            }
            int rep, value = 0;
            if (s == 16) {
                if (i == 0) throw FormatError("inflate: repeat with no previous length");
                value = len[i - 1];
                rep = 3 + int(bits(2));
            } else if (s == 17) {
                rep = 3 + int(bits(3));
            } else {
                rep = 11 + int(bits(7));
            }
            if (i + rep > nlen + ndist) throw FormatError("inflate: too many lengths");
            while (rep--) len[i++] = std::uint8_t(value);
        }
        if (len[256] == 0) throw FormatError("inflate: no end-of-block code");
        build(lit, len, nlen);
        build(dist, len + nlen, ndist);
        codes(lit, dist);
    }

    const std::uint8_t* p_;
    std::size_t n_, pos_ = 0;
    std::uint64_t buf_ = 0;
    int cnt_ = 0;
    std::vector<std::uint8_t>& out_;
    std::size_t start_, max_out_;
};

} // namespace

void inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t max_out) {
    Inflater(in, out, max_out).run();
}

} // namespace parkan::install
