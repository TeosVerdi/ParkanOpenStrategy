// Sound and music output: the port's replacement for Ngi32 CNgiSound (DirectSound 7 + DS3D, MCI CD audio)
// and the parts of services CSoundServer / iron3d CMusicPlayer that touch the device. Spec: notes/audio.md.
//
// One SDL audio stream mixes every sound effect in its callback; the main thread only posts commands to
// a queue the callback drains. Music is a second stream bound to the same device that decodes the
// MUSIC/TrackNN.ogg files on demand. All methods are main-thread only.
#pragma once

#include "core/math.hpp"
#include "res/swav.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace parkan {
class AssetLibrary;
struct FxSound;
} // namespace parkan

namespace parkan::audio {

using Handle = std::uint32_t; // 0 = no sound

struct Sound {
    std::string lib, entry;  // e.g. "sounds.lib", "atm_rain1.wav" (case-insensitive)
    bool positional = false; // 3D (FX channels) or 2D (INgiWave: voices, UI, ambience, rain; centred, full volume)
    float min_distance = 1, max_distance = 1e9f;
    float pitch = 1;         // frequency ratio
};

// An effects.rlb sound command as a 3D sound; its pitch at t0 (Ngi32 ignores the ratio unless mode bit0).
Sound fx_sound(const FxSound& fx);

enum class Bus { Sfx, Music }; // the mixer lines SFX_VOLUME / CD_VOLUME (audio.md §2.1), gain = ini / 100

class AudioEngine {
public:
    explicit AudioEngine(AssetLibrary& lib);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // False when no device could be opened; every call is then a silent no-op.
    bool ok() const;

    // Decoded sample, shared by every channel playing it (TryDuplicateChannelBuffer); null if missing.
    std::shared_ptr<const Pcm> load(std::string_view lib, std::string_view entry);

    // Starts at once. Missing entries (e.g. fortif_door_move.wav) give 0. A positional one-shot beyond
    // its max distance is cancelled; a loop there is silent but keeps time (UpdateBankChannels3D).
    Handle play(const Sound& sound, Vec3 position = {}, Vec3 velocity = {}, bool loop = false);
    void stop(Handle h);
    void stop_all();
    void set_position(Handle h, Vec3 position, Vec3 velocity);
    void set_pitch(Handle h, float ratio);
    bool is_playing(Handle h);

    // The rendered camera (BuildDS3DListenerFromCamera 1000d490): forward = matrix column 0, up = column 2,
    // velocity = Δposition / Δt from the caller.
    void set_listener(Vec3 position, Vec3 forward, Vec3 up, Vec3 velocity = {});
    void set_gain(Bus bus, float gain);

    // Music: CD play index i is MUSIC/Track{i+1:02}.ogg (ogg-winmm; Track02..10 → indices 1..9).
    const std::vector<int>& music_tracks() const;
    bool play_music(int index, bool loop);
    // MusicPlayRandomTrack: a present track other than the current one; returns it, or -1.
    int play_random_music(bool loop);
    void stop_music();
    int music_track() const;     // index last started, -1 none
    bool music_finished() const; // a non-looping track ran out (CD player IsFinished)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parkan::audio
