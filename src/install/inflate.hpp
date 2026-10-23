// Raw DEFLATE (RFC 1951) decoder for InstallShield cabinets, which compress each file as a series of
// independent raw-deflate chunks that usually end with a sync flush instead of a final block.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace parkan::install {

// Appends the decompressed `in` to `out`. Decoding stops after a final block, or when the input runs
// out at a block boundary (a flushed, unterminated stream). Throws FormatError on corrupt data or
// when this call would add more than `max_out` bytes. Back-references never reach before the
// existing contents of `out` (each call is an independent stream).
void inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t max_out = SIZE_MAX);

} // namespace parkan::install
