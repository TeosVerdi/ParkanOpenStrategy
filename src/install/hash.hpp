// SHA-256 (edition fingerprints, file manifests) and MD5 (InstallShield 5 stores one per file).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace parkan::install {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    std::array<std::uint8_t, 32> final();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::uint64_t len_ = 0;
};

class Md5 {
public:
    Md5();
    void update(std::span<const std::uint8_t> data);
    std::array<std::uint8_t, 16> final();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[4];
    std::uint8_t buf_[64];
    std::uint64_t len_ = 0;
};

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);
std::string to_hex(std::span<const std::uint8_t> data);
// The first 8 bytes of a digest as a big-endian number (the manifests' short hash).
std::uint64_t digest_prefix(std::span<const std::uint8_t> digest);

} // namespace parkan::install
