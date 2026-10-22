// FXID effects (effects.rlb) and EXPL explosion definitions (*.exp in the object archives).
// Spec: notes/effects.md (sound commands also in notes/audio.md §3.1).
//
// An FXID entry is a 0x3c-byte header followed by `command_count` commands. The low byte of each
// command word is the opcode and fixes the record size; bit 8 marks a visibility-tested command
// (Effect CFxInstance_BuildCommands 10007650 builds one runtime object per command). Every offset
// below is relative to the command word.
#pragma once

#include "core/bytes.hpp"
#include "core/math.hpp"

#include <array>
#include <variant>

namespace parkan {

class AssetLibrary;

// A (library, entry) pair stored as two char[32] fields; bytes after the NUL are editor garbage.
struct FxResourceRef {
    std::string archive, name;
    bool empty() const { return archive.empty() || name.empty(); }
};

// Header (0x3c bytes).
struct FxHeader {
    std::uint32_t command_count = 0;
    std::uint32_t time_mode = 0;    // +0x04: CFxManager_EvalTimeParam (10005c60) mode, 0..17
    float duration = 0;             // +0x08: seconds (instance end = start + 1000 × duration ms)
    float t_jitter = 0;             // +0x0c: flag 0x1: random ± t_jitter/2 added to t each update
    std::uint32_t flags = 0;        // +0x10: see FxFlag
    std::uint32_t settings_key = 0; // +0x14: low byte detail category, high byte LOD-threshold table
    Vec3 pos_jitter;                // +0x18: flag 0x8: random walk of the position (all zero in the data)
    Vec3 probe_point;               // +0x24: local point the occlusion ray aims at (flag 0x400)
    Vec3 scale{1, 1, 1};            // +0x30: template scale, multiplied by SetEffectScale
};

// Header flags [confirmed by code unless noted].
enum FxFlag : std::uint32_t {
    kFxJitterT = 0x1,       // random offset on t (header t_jitter), not for sound commands
    kFxOneShot = 0x2,       // the instance is destroyed once t >= 1
    kFxDetach = 0x4,        // baked into world space and detached from the owner on the first update
    kFxJitterPos = 0x8,     // random walk added to the position every update
    kFxAlwaysShown = 0x10,  // shown even when the owner's part is hidden
    kFxTriangle = 0x20,     // t = 2t below 0.5, 2(1 − t) above (ping-pong)
    kFxStartDisabled = 0x40,
    kFxNeedsMgrFlag = 0x80, // t = 0 unless manager flag 1 (SendMsg 0x17/0x18) is set
    kFxHideOnMgrFlag = 0x100,
    kFxScaleByLinear = 0x200, // t ×= linear progress
    kFxOcclusionTest = 0x400, // ray-tested every ~50 ms; hidden glows are skipped
    kFxForcedRenderOnly = 0x800,
    kFxDrawFlag4 = 0x2000,   // passed to the sprite draw as flag 4 (meaning open)
};

// Opcode 1: dynamic light (224 bytes; +0x88..0xdf unused). Effect FxLight_Init/Update 1000f4b0/1000f6e0.
struct FxLightCmd {
    std::uint32_t kind = 0;   // +0x04: 1..7 → ILightManager type and flags (notes/effects.md)
    float t0 = 0, t1 = 1;     // +0x08: active window in effect time
    Vec3 pos0, pos1;          // +0x10, +0x1c: effect-local position, lerped over the window
    Vec3 dir0, dir1;          // +0x28, +0x34: direction
    std::array<float, 4> color0{}, color1{}, color_jitter{}; // +0x40, +0x50, +0x60 (RGBA, overbright)
    float range0 = 0, range1 = 0, range_jitter = 0;          // +0x70, +0x74, +0x78
    std::array<float, 3> attenuation{};                      // +0x7c
};

// Opcode 2 record (148 bytes from the command word); spec notes/audio.md §3.1.
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

// Opcodes 3, 4 and 9: one billboard sprite (200 / 204 / 208 bytes). FxSprite_Init 100103b0.
struct FxSpriteCmd {
    std::uint32_t align = 0;             // +0x04: 0 camera-facing, 1 around `axis`, 2 in the plane ⟂ `axis`
    float frame0 = 0, frame1 = 1;        // +0x08: material phase; frame0 < 0: driven by age in seconds
    float frame_exp = 1;                 // +0x10
    float alpha0 = 1, alpha1 = 1;        // +0x14: opacity (overrides the material's)
    float alpha_exp = 1;                 // +0x1c
    float t0 = 0, t1 = 1;                // +0x20: active window
    Vec3 pos0, pos1;                     // +0x28, +0x34
    std::array<float, 3> pos_exp{1, 1, 1}; // +0x40: per-axis power curve
    Vec3 axis0, axis1;                   // +0x4c, +0x58: linear
    Vec3 size0, size1;                   // +0x64, +0x70
    std::array<float, 3> size_exp{1, 1, 1}; // +0x7c
    FxResourceRef material;              // +0x88
    float distance_norm = 0;             // op 4 +0xc8: u ×= |camera| / distance_norm
    std::uint32_t shade_mode = 0;        // op 9 +0xc8: 0..2 → CShade_RenderEffect mode 3/5/6
    std::uint32_t shade_flag = 0;        // op 9 +0xcc
};

// Opcode 5: a beam from the effect's first position to its current one (112 bytes). FxBeam_* 100028a0.
struct FxBeamCmd {
    float alpha0 = 1, alpha1 = 1;    // +0x04
    float t0 = 0, t1 = 1;            // +0x0c
    std::uint32_t max_segments = 1;  // +0x14
    float width0 = 1, width1 = 1;    // +0x18
    float uv_length = 1;             // +0x20: world length of one texture repeat
    float segment_length = 1;        // +0x24
    float frame0 = 0, frame1 = 0;    // +0x28: frame0 < 0: driven by age; frame1 < 0: random frame
    FxResourceRef material;          // +0x30
};

// Opcodes 7 (sprite burst) and 10 (debris pieces), 208 bytes. FxParticleCmd_* 10001000/10001230/10001300.
struct FxBurstCmd {
    std::uint32_t align = 0;          // +0x04: op 7: 0/2 camera-facing, 1 velocity-aligned; op 10: 0x10/0x20 shape set
    float alpha0 = 1, alpha1 = 0, alpha_exp = 1; // +0x08
    float t0 = 0, t1 = 1;             // +0x14: active window
    float life = 1;                   // +0x1c: particle life as a fraction of the window
    float frame_rate = -1;            // +0x20: material phase speed (≤ 1: divisor, > 1: cycles)
    std::uint32_t emissions = 1;      // +0x24
    std::uint32_t per_emission = 1;   // +0x28
    Vec3 pos0, pos1, pos_rand;        // +0x2c, +0x38, +0x44 (end = pos1 ± pos_rand/2)
    std::array<float, 3> pos_exp{1, 1, 1}; // +0x50
    Vec3 size0, size1, size_rand;     // +0x5c, +0x68, +0x74
    std::array<float, 3> size_exp{1, 1, 1}; // +0x80
    float gravity = 0;                // +0x8c: × world gravity (10 m/s²); negative rises
    FxResourceRef material;           // +0x90
};

// Opcode 8: trail, a ring of sprites spawned at intervals along the effect's path (248 bytes).
// FxTrail_Init/Update/Render 10011230/100115c0/10012030.
struct FxTrailCmd {
    float alpha0 = 1, alpha1 = 0, alpha_exp = 1; // +0x04
    float t0 = 0, t1 = 1;            // +0x10: spawn window
    float interval0 = 0.1f, interval1 = 0.1f; // +0x18: seconds between spawns over the window
    float frame_exp = 1;             // +0x20: < 0: each particle keeps a random phase
    std::uint32_t count = 1;         // +0x24: ring size
    Vec3 drift0, drift1, drift_rand; // +0x28, +0x34, +0x40: offset of the spawn target
    std::array<float, 3> drift_exp{1, 1, 1}; // +0x4c
    Vec3 pos0, pos1, pos_rand;       // +0x58, +0x64, +0x70: particle motion in its spawn frame
    std::array<float, 3> pos_exp{1, 1, 1};   // +0x7c
    Vec3 size0, size1, size_rand;    // +0x88, +0x94, +0xa0
    std::array<float, 3> size_exp{1, 1, 1};  // +0xac
    FxResourceRef material;          // +0xb8
};

struct FxCommand {
    std::uint32_t opcode = 0;
    bool tested = false; // command word bit 8: visibility-tested (drawn on top when the ray is clear)
    std::variant<FxLightCmd, FxSound, FxSpriteCmd, FxBeamCmd, FxBurstCmd, FxTrailCmd> data;
};

struct FxEffect {
    FxHeader header;
    std::size_t commands = 0;
    std::vector<FxCommand> list; // every command, in file order
    std::vector<FxSound> sounds; // the opcode-2 commands (kept for the audio code)
};

// Record size per opcode including the command word; 0 = not a shipped opcode (6 exists in the
// factory as a 4-byte no-op command but never occurs).
std::size_t fx_command_size(std::uint32_t opcode);

// Throws FormatError on an unknown opcode, unknown command-word bits or a size mismatch.
FxEffect decode_fx(std::span<const std::uint8_t> data);

// The sound commands of effects.rlb entry `name`; empty when the effect does not exist.
std::vector<FxSound> load_fx_sounds(AssetLibrary& lib, std::string_view name);

// EXPL (792 bytes): what a destroyed part or an exploding bullet spawns. Read by Control
// (CControlBase::UpdatePartStage 10011220) through the NDPR / bullet definitions.
struct ExplosionDef {
    std::uint32_t kind = 0;      // +0x00: 1 (26), 2 (54), 3 (64) [meaning inferred: damage class]
    float damage = 0;            // +0x04: × the object's hit-point multiplier
    float radius = 0;            // +0x08: damage radius (× object scale except for bullets)
    float unknown0c = 1, unknown10 = 1; // +0x0c, +0x10: always 1
    std::uint32_t orient_mode = 0;      // +0x14: 0 or 7 in the data (switch in UpdatePartStage)
    FxResourceRef effect;               // +0x18: default effect
    // +0x58 + 64·k: per ground-surface effect (k = surface index 0..10); falls back to `effect` when
    // the slot does not resolve. Slots past the authored ones hold editor garbage.
    std::array<FxResourceRef, 11> surface;
    std::array<bool, 11> surface_valid{}; // printable, non-empty strings (authored)
};

ExplosionDef decode_expl(std::span<const std::uint8_t> data);

} // namespace parkan
