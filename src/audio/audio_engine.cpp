#include "audio/audio_engine.hpp"

#include "audio/spatial.hpp"
#include "core/paths.hpp"
#include "res/fx.hpp"
#include "res/library.hpp"

#include <SDL3/SDL.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <unordered_set>

namespace parkan::audio {

Sound fx_sound(const FxSound& fx) {
    Sound s;
    s.lib = fx.archive;
    s.entry = fx.entry;
    s.positional = true;
    s.min_distance = fx.min_distance;
    s.max_distance = fx.max_distance;
    s.pitch = fx.freq_control() ? fx.pitch0 : 1; // the ratio only reaches DS on CTRLFREQUENCY (bit9) channels
    return s;
}

namespace {

constexpr int kMixRate = 44100; // mixing rate; SDL converts to the device format
constexpr int kChunkFrames = 512;

struct Listener {
    Vec3 pos, forward{1, 0, 0}, up{0, 0, 1}, vel;
};

struct Voice {
    Handle id = 0;
    std::shared_ptr<const Pcm> pcm;
    bool loop = false, positional = false, fresh = true;
    float min_distance = 1, max_distance = 1e9f, pitch = 1;
    Vec3 pos, vel;
    double cursor = 0;      // in source frames
    StereoGain gain{0, 0};  // applied at the end of the last chunk; gains ramp across a chunk
};

struct Command {
    enum Kind { Play, Stop, StopAll, Move, Pitch, SetListener } kind = Play;
    Handle id = 0;
    Voice voice;     // Play
    Vec3 a, b, c, d; // Move: pos, vel; SetListener: pos, forward, up, vel
    float ratio = 1; // Pitch
};

std::string cache_key(std::string_view lib, std::string_view entry) {
    std::string k;
    for (char ch : lib) k += char(std::tolower(static_cast<unsigned char>(ch)));
    k += '|';
    for (char ch : entry) k += char(std::tolower(static_cast<unsigned char>(ch)));
    return k;
}

float sample(const std::int16_t* s, std::size_t i0, std::size_t i1, float frac) {
    return (float(s[i0]) + (float(s[i1]) - float(s[i0])) * frac) * (1.f / 32768);
}

} // namespace

struct AudioEngine::Impl {
    AssetLibrary& lib;
    SDL_AudioDeviceID device = 0;
    SDL_AudioStream* sfx = nullptr;   // the effects mixer
    SDL_AudioStream* music = nullptr; // Ogg decoder

    // Main thread.
    std::map<std::string, std::shared_ptr<const Pcm>> cache;
    std::unordered_set<Handle> live;
    Handle next_id = 1;
    std::vector<int> tracks;
    int track = -1;
    std::minstd_rand rng{std::random_device{}()};

    // Shared with the SFX callback.
    std::mutex mu;
    std::vector<Command> queue;
    std::vector<Handle> ended;

    // SFX callback only.
    std::vector<Command> work;
    std::vector<Voice> voices;
    Listener listener;
    std::vector<float> mix;
    std::vector<Handle> ended_here;

    // Music, guarded by the music stream's lock (SDL holds it during the callback).
    stb_vorbis* vorbis = nullptr;
    int vorbis_channels = 0;
    bool music_loop = false;
    std::atomic<bool> music_done{false};
    std::vector<short> decode_buf;
    std::FILE* wav = nullptr;          // a CD-ripped MUSIC/TrackNN.wav (instead of the Ogg decoder)
    std::uint32_t wav_size = 0, wav_pos = 0; // data bytes, read position (from byte 44)

    explicit Impl(AssetLibrary& l) : lib(l) {}

    void post(Command c) {
        std::lock_guard lk(mu);
        queue.push_back(std::move(c));
    }
    void reap() {
        std::lock_guard lk(mu);
        for (Handle h : ended) live.erase(h);
        ended.clear();
    }
    Voice* find(Handle h) {
        for (auto& v : voices)
            if (v.id == h) return &v;
        return nullptr;
    }

    void apply(Command& c) {
        switch (c.kind) {
        case Command::Play: voices.push_back(std::move(c.voice)); break;
        case Command::Stop: std::erase_if(voices, [&](const Voice& v) { return v.id == c.id; }); break;
        case Command::StopAll: voices.clear(); break;
        case Command::Move:
            if (Voice* v = find(c.id)) v->pos = c.a, v->vel = c.b;
            break;
        case Command::Pitch:
            if (Voice* v = find(c.id)) v->pitch = c.ratio;
            break;
        case Command::SetListener: listener = {c.a, c.b, c.c, c.d}; break;
        }
    }

    // Mixes one voice into `out`; false when it ended (finished or cancelled out of range).
    bool render(Voice& v, float* out, int frames) {
        const Pcm& p = *v.pcm;
        const std::size_t n = p.frames();
        if (!n) return false;
        float ratio = v.pitch;
        StereoGain target{1, 1};
        if (v.positional) {
            const Vec3 to = v.pos - listener.pos;
            const float dist = std::sqrt(dot(to, to));
            if (dist > v.max_distance) {
                // UpdateBankChannels3D: out of range one-shots are cancelled, loops keep virtual time.
                if (!v.loop) return false;
                const double rate = std::clamp(double(p.rate) * ratio, 100.0, 100000.0);
                v.cursor = std::fmod(v.cursor + frames * rate / kMixRate, double(n));
                v.gain = {0, 0};
                v.fresh = false;
                return true;
            }
            const float g = distance_gain(dist, v.min_distance, v.max_distance);
            const StereoGain pan = pan_gain(to, listener.forward, listener.up);
            target = {g * pan.left, g * pan.right};
            ratio *= doppler_ratio(v.pos, v.vel, listener.pos, listener.vel);
        }
        if (v.fresh) v.gain = target, v.fresh = false;
        // SetFrequency clamp of the original: 100..100000 Hz.
        const double step = std::clamp(double(p.rate) * ratio, 100.0, 100000.0) / kMixRate;
        const float dl = (target.left - v.gain.left) / float(frames), dr = (target.right - v.gain.right) / float(frames);
        float gl = v.gain.left, gr = v.gain.right;
        const std::int16_t* s = p.samples.data();
        bool alive = true;
        for (int i = 0; i < frames; ++i) {
            const std::size_t i0 = std::size_t(v.cursor);
            const std::size_t i1 = i0 + 1 < n ? i0 + 1 : (v.loop ? 0 : i0);
            const float frac = float(v.cursor - double(i0));
            float l, r;
            if (p.channels == 1) {
                l = r = sample(s, i0, i1, frac);
            } else {
                l = sample(s, 2 * i0, 2 * i1, frac);
                r = sample(s, 2 * i0 + 1, 2 * i1 + 1, frac);
                if (v.positional) l = r = (l + r) * 0.5f; // DS3D buffers are mono
            }
            gl += dl, gr += dr;
            out[2 * i] += l * gl;
            out[2 * i + 1] += r * gr;
            v.cursor += step;
            if (v.cursor >= double(n)) {
                if (!v.loop) {
                    alive = false;
                    break;
                }
                v.cursor = std::fmod(v.cursor, double(n));
            }
        }
        v.gain = target;
        return alive;
    }

    static void SDLCALL sfx_callback(void* user, SDL_AudioStream* stream, int additional, int) {
        auto& im = *static_cast<Impl*>(user);
        {
            std::lock_guard lk(im.mu);
            im.work.swap(im.queue);
        }
        for (auto& c : im.work) im.apply(c);
        im.work.clear();
        while (additional > 0) {
            const int frames = std::min(kChunkFrames, (additional + 7) / 8);
            im.mix.assign(std::size_t(frames) * 2, 0.f);
            std::size_t kept = 0;
            for (std::size_t i = 0; i < im.voices.size(); ++i) {
                if (im.render(im.voices[i], im.mix.data(), frames)) {
                    if (kept != i) im.voices[kept] = std::move(im.voices[i]);
                    ++kept;
                } else {
                    im.ended_here.push_back(im.voices[i].id);
                }
            }
            im.voices.resize(kept);
            for (float& x : im.mix) x = std::clamp(x, -1.f, 1.f);
            SDL_PutAudioStreamData(stream, im.mix.data(), frames * 8);
            additional -= frames * 8;
        }
        if (!im.ended_here.empty()) {
            std::lock_guard lk(im.mu);
            im.ended.insert(im.ended.end(), im.ended_here.begin(), im.ended_here.end());
            im.ended_here.clear();
        }
    }

    // Streams the current Ogg on demand. End of track: replay when looping (the CD watchdog's
    // Play(resume|loop)), else report finished (IsFinished).
    static void SDLCALL music_callback(void* user, SDL_AudioStream* stream, int additional, int) {
        auto& im = *static_cast<Impl*>(user);
        if (im.wav && !im.music_done) {
            // PCM 44.1 kHz stereo 16-bit straight from the file, whole frames.
            int want = additional > 0 ? (additional + 3) & ~3 : 0;
            bool rewound = false;
            while (want > 0) {
                if (im.wav_pos >= im.wav_size) {
                    if (im.music_loop && !rewound && im.wav_size > 0 && std::fseek(im.wav, 44, SEEK_SET) == 0) {
                        im.wav_pos = 0, rewound = true;
                        continue;
                    }
                    im.music_done = true;
                    return;
                }
                const std::uint32_t n = std::min<std::uint32_t>(std::uint32_t(std::min(want, 16384)), im.wav_size - im.wav_pos);
                im.decode_buf.resize(n / 2);
                const std::size_t got = std::fread(im.decode_buf.data(), 1, n, im.wav) & ~std::size_t(3);
                if (got == 0) {
                    im.wav_pos = im.wav_size; // truncated file: treat as the end
                    continue;
                }
                rewound = false;
                im.wav_pos += std::uint32_t(got);
                SDL_PutAudioStreamData(stream, im.decode_buf.data(), int(got));
                want -= int(got);
            }
            return;
        }
        if (!im.vorbis || im.music_done) return;
        const int ch = im.vorbis_channels;
        bool rewound = false;
        while (additional > 0) {
            const int frames = std::min(4096, additional / (2 * ch) + 1);
            im.decode_buf.resize(std::size_t(frames * ch));
            const int got = stb_vorbis_get_samples_short_interleaved(im.vorbis, ch, im.decode_buf.data(), frames * ch);
            if (got <= 0) {
                if (im.music_loop && !rewound && stb_vorbis_seek_start(im.vorbis)) {
                    rewound = true; // guards an empty file
                    continue;
                }
                im.music_done = true;
                return;
            }
            rewound = false;
            SDL_PutAudioStreamData(stream, im.decode_buf.data(), got * ch * 2);
            additional -= got * ch * 2;
        }
    }
};

AudioEngine::AudioEngine(AssetLibrary& lib) : impl_(std::make_unique<Impl>(lib)) {
    // Tracks present on disk (the ogg-winmm wrapper reports the files it finds).
    for (int i = 0; i < 99; ++i) {
        char name[32];
        // An installed or GOG folder has TrackNN.ogg; a disc imported by src/install has TrackNN.wav.
        std::snprintf(name, sizeof name, "MUSIC/Track%02d.ogg", i + 1);
        bool present = resolve_path(lib.game_dir(), name).has_value();
        std::snprintf(name, sizeof name, "MUSIC/Track%02d.wav", i + 1);
        present = present || resolve_path(lib.game_dir(), name).has_value();
        if (present) impl_->tracks.push_back(i);
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: SDL_INIT_AUDIO failed: %s", SDL_GetError());
        return;
    }
    // One logical device with two bound streams; SDL mixes them (a device opened through
    // SDL_OpenAudioDeviceStream cannot take a second binding).
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kMixRate};
    impl_->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!impl_->device) {
        SDL_Log("audio: no output device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    impl_->sfx = SDL_CreateAudioStream(&spec, nullptr);
    if (!impl_->sfx || !SDL_SetAudioStreamGetCallback(impl_->sfx, &Impl::sfx_callback, impl_.get()) ||
        !SDL_BindAudioStream(impl_->device, impl_->sfx)) {
        SDL_Log("audio: sfx stream unavailable: %s", SDL_GetError());
        SDL_DestroyAudioStream(impl_->sfx);
        impl_->sfx = nullptr;
        SDL_CloseAudioDevice(impl_->device);
        impl_->device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    const SDL_AudioSpec music_spec{SDL_AUDIO_S16, 2, 44100}; // replaced per track
    impl_->music = SDL_CreateAudioStream(&music_spec, nullptr);
    if (!impl_->music || !SDL_SetAudioStreamGetCallback(impl_->music, &Impl::music_callback, impl_.get()) ||
        !SDL_BindAudioStream(impl_->device, impl_->music)) {
        SDL_Log("audio: music stream unavailable: %s", SDL_GetError());
        SDL_DestroyAudioStream(impl_->music);
        impl_->music = nullptr;
    }
    SDL_ResumeAudioDevice(impl_->device);
}

AudioEngine::~AudioEngine() {
    if (!impl_->device) return;
    SDL_DestroyAudioStream(impl_->music); // unbinds; no callback runs after this
    SDL_DestroyAudioStream(impl_->sfx);
    SDL_CloseAudioDevice(impl_->device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (impl_->vorbis) stb_vorbis_close(impl_->vorbis);
    if (impl_->wav) std::fclose(impl_->wav);
}

bool AudioEngine::ok() const { return impl_->device != 0; }

std::shared_ptr<const Pcm> AudioEngine::load(std::string_view lib, std::string_view entry) {
    const std::string key = cache_key(lib, entry);
    if (auto it = impl_->cache.find(key); it != impl_->cache.end()) return it->second;
    std::shared_ptr<const Pcm> pcm;
    try {
        if (const NResEntry* e = impl_->lib.find(lib, entry)) pcm = std::make_shared<const Pcm>(decode_swav(e->data));
        else SDL_Log("audio: sound not found: %s/%s", std::string(lib).c_str(), std::string(entry).c_str());
    } catch (const std::exception& ex) {
        SDL_Log("audio: %s/%s: %s", std::string(lib).c_str(), std::string(entry).c_str(), ex.what());
    }
    impl_->cache.emplace(key, pcm); // misses are cached too, so they are reported once
    return pcm;
}

Handle AudioEngine::play_pcm(std::shared_ptr<const Pcm> pcm, bool loop) {
    Sound s;
    return play_samples(s, std::move(pcm), {}, {}, loop);
}

Handle AudioEngine::play(const Sound& sound, Vec3 position, Vec3 velocity, bool loop) {
    if (!ok()) return 0;
    return play_samples(sound, load(sound.lib, sound.entry), position, velocity, loop);
}

Handle AudioEngine::play_samples(const Sound& sound, std::shared_ptr<const Pcm> pcm, Vec3 position, Vec3 velocity, bool loop) {
    if (!ok() || !pcm || !pcm->frames()) return 0;
    Command c;
    c.kind = Command::Play;
    c.voice.id = impl_->next_id++;
    if (!impl_->next_id) impl_->next_id = 1;
    c.voice.pcm = std::move(pcm);
    c.voice.loop = loop;
    c.voice.positional = sound.positional;
    c.voice.min_distance = sound.min_distance;
    c.voice.max_distance = sound.max_distance;
    c.voice.pitch = sound.pitch;
    c.voice.pos = position;
    c.voice.vel = velocity;
    const Handle h = c.voice.id;
    impl_->live.insert(h);
    impl_->post(std::move(c));
    return h;
}

void AudioEngine::stop(Handle h) {
    if (!h || !impl_->live.erase(h)) return;
    Command c;
    c.kind = Command::Stop;
    c.id = h;
    impl_->post(std::move(c));
}

void AudioEngine::stop_all() {
    if (!ok()) return;
    impl_->live.clear();
    Command c;
    c.kind = Command::StopAll;
    impl_->post(std::move(c));
}

void AudioEngine::set_position(Handle h, Vec3 position, Vec3 velocity) {
    if (!impl_->live.count(h)) return;
    Command c;
    c.kind = Command::Move;
    c.id = h;
    c.a = position, c.b = velocity;
    impl_->post(std::move(c));
}

void AudioEngine::set_pitch(Handle h, float ratio) {
    if (!impl_->live.count(h)) return;
    Command c;
    c.kind = Command::Pitch;
    c.id = h;
    c.ratio = ratio;
    impl_->post(std::move(c));
}

bool AudioEngine::is_playing(Handle h) {
    impl_->reap();
    return impl_->live.count(h) != 0;
}

void AudioEngine::set_listener(Vec3 position, Vec3 forward, Vec3 up, Vec3 velocity) {
    if (!ok()) return;
    Command c;
    c.kind = Command::SetListener;
    c.a = position, c.b = forward, c.c = up, c.d = velocity;
    impl_->post(std::move(c));
}

void AudioEngine::set_gain(Bus bus, float gain) {
    if (SDL_AudioStream* s = bus == Bus::Music ? impl_->music : impl_->sfx) SDL_SetAudioStreamGain(s, std::max(gain, 0.f));
}

const std::vector<int>& AudioEngine::music_tracks() const { return impl_->tracks; }

bool AudioEngine::play_music(int index, bool loop) {
    if (!impl_->music) return false;
    char name[32];
    std::snprintf(name, sizeof name, "MUSIC/Track%02d.ogg", index + 1);
    stb_vorbis* v = nullptr;
    std::FILE* wav = nullptr;
    std::uint32_t wav_size = 0;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    if (const auto path = resolve_path(impl_->lib.game_dir(), name)) {
        int err = 0;
        v = stb_vorbis_open_filename(path->string().c_str(), &err, nullptr);
        if (!v) {
            SDL_Log("audio: %s: stb_vorbis error %d", name, err);
            return false;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(v);
        spec = {SDL_AUDIO_S16, info.channels, int(info.sample_rate)};
    } else {
        std::snprintf(name, sizeof name, "MUSIC/Track%02d.wav", index + 1);
        const auto wpath = resolve_path(impl_->lib.game_dir(), name);
        if (!wpath) return false;
        wav = std::fopen(wpath->string().c_str(), "rb");
        std::uint8_t header[44] = {};
        std::error_code ec;
        const auto size = std::filesystem::file_size(*wpath, ec);
        const auto data = wav && !ec && std::fread(header, 1, 44, wav) == 44 ? music_wav_data_size(header, size) : std::nullopt;
        if (!data) {
            SDL_Log("audio: %s: not a 44.1 kHz stereo 16-bit PCM WAV", name);
            if (wav) std::fclose(wav);
            return false;
        }
        wav_size = *data;
    }
    SDL_LockAudioStream(impl_->music);
    stb_vorbis* old = impl_->vorbis;
    std::FILE* old_wav = impl_->wav;
    impl_->vorbis = v;
    impl_->vorbis_channels = v ? spec.channels : 0;
    impl_->wav = wav, impl_->wav_size = wav_size, impl_->wav_pos = 0;
    impl_->music_loop = loop;
    impl_->music_done = false;
    SDL_ClearAudioStream(impl_->music);
    SDL_SetAudioStreamFormat(impl_->music, &spec, nullptr);
    SDL_UnlockAudioStream(impl_->music);
    if (old) stb_vorbis_close(old);
    if (old_wav) std::fclose(old_wav);
    impl_->track = index;
    return true;
}

int AudioEngine::play_random_music(bool loop) {
    const int t = pick_random_track(impl_->tracks, impl_->track, std::uint32_t(impl_->rng()));
    return t >= 0 && play_music(t, loop) ? t : -1;
}

void AudioEngine::stop_music() {
    if (!impl_->music) return;
    SDL_LockAudioStream(impl_->music);
    stb_vorbis* old = impl_->vorbis;
    std::FILE* old_wav = impl_->wav;
    impl_->vorbis = nullptr;
    impl_->wav = nullptr;
    impl_->music_done = false;
    SDL_ClearAudioStream(impl_->music);
    SDL_UnlockAudioStream(impl_->music);
    if (old) stb_vorbis_close(old);
    if (old_wav) std::fclose(old_wav);
}

int AudioEngine::music_track() const { return impl_->track; }

bool AudioEngine::music_finished() const {
    // Done decoding and everything queued has been played.
    return impl_->music && impl_->music_done && SDL_GetAudioStreamQueued(impl_->music) == 0;
}

} // namespace parkan::audio
