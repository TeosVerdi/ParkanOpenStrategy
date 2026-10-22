// Little-endian byte reading and file loading shared by every loader.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace parkan {

using Bytes = std::vector<std::uint8_t>;

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Bytes read_file(const std::filesystem::path& path);

// Cursor over a byte span; every read is bounds-checked and throws FormatError.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::size_t pos() const { return pos_; }
    std::size_t size() const { return data_.size(); }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool at_end() const { return pos_ == data_.size(); }
    void seek(std::size_t p) {
        if (p > data_.size()) throw FormatError("seek past end");
        pos_ = p;
    }

    template <class T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        need(sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::int32_t i32() { return read<std::int32_t>(); }
    float f32() { return read<float>(); }
    std::uint8_t u8() { return read<std::uint8_t>(); }

    std::span<const std::uint8_t> bytes(std::size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    std::string str(std::size_t n) {
        auto s = bytes(n);
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    }

private:
    void need(std::size_t n) const {
        if (n > data_.size() - pos_)
            throw FormatError("truncated at offset " + std::to_string(pos_));
    }
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

// NUL-terminated string at `off` inside a pool.
std::string_view cstr_at(std::span<const std::uint8_t> pool, std::size_t off);

// Windows-1251 (the game's text encoding) to UTF-8.
std::string cp1251_to_utf8(std::string_view s);

} // namespace parkan
