// FXID effects (effects.rlb): a 0x3c-byte header (u32 command count at +0) followed by the commands. The
// low byte of each command word is the opcode and fixes the record size (sizes recovered by walking all
// 923 entries to their exact end). Only opcode 2, the sound command (Effect InitFxSoundCommand
// 10012d10 / UpdateFxSoundCommand 10012eb0), is decoded. Spec: notes/audio.md §3.1.
#pragma once

#include "core/bytes.hpp"
#include "core/math.hpp"

namespace parkan {

class AssetLibrary;

// Opcode-2 record (148 bytes from the command word).
struct FxSound {
    std::uint32_t mode = 0;      // bit0 frequency control, bit1 loop (data: 0 or 2)
    float t0 = 0, t1 = 0;        // active window in effect-normalised time
    Vec3 pos0, pos1;             // effect-local position at t0 / t1
    Vec3 dir0, dir1;             // always (1,0,0); unused by the update
    float min_distance = 0, max_distance = 0;
    float pitch0 = 1, pitch1 = 1; // frequency ratio at t0 / t1 (honoured on mode-bit0 channels only)
    std::uint32_t user = 0;      // channel param 0x200, always 0
    std::string archive, entry;  // archive is always sounds.lib
    bool loop() const { return mode & 2; }
    bool freq_control() const { return mode & 1; }
};

struct FxEffect {
    std::size_t commands = 0;
    std::vector<FxSound> sounds;
};

// Throws FormatError on an unknown opcode or a size mismatch.
FxEffect decode_fx(std::span<const std::uint8_t> data);

// The sound commands of effects.rlb entry `name`; empty when the effect does not exist.
std::vector<FxSound> load_fx_sounds(AssetLibrary& lib, std::string_view name);

} // namespace parkan
