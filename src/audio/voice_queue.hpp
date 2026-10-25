// The voice FIFO of services' CSoundServer (vtable 1003a4a4): every VOICE_* key and every mission-message
// voice goes through iron3d's QueueUnitVoice (10061fb0) → QueueSound, so voices play one after another
// and never overlap each other (they do overlap the sound effects and PlaySound's 2D sounds: briefing
// waypoints, interface sounds). Spec: notes/audio.md §2.5.
//
// - QueueSound (10011ab0): a key the resource manager does not resolve is dropped; when the queue is
//   empty the sound starts at once; it is appended either way (the head is the one playing);
// - UpdateQueue (100119e0, iron3d MainLoop every frame, also after the mission's end): when the head no
//   longer plays it is dropped and the next one starts;
// - ClearQueue (10011980): forgets every entry, the head included, without stopping it (so the next
//   queued voice starts at once over it);
// - while the server is disabled (Disable 10011c30, e.g. PopToState) nothing is queued.
// Header-only; the owner supplies the device calls.
#pragma once

#include "audio/audio_engine.hpp"

#include <deque>
#include <functional>

namespace parkan::audio {

class VoiceQueue {
public:
    std::function<bool(const Sound&)> exists;   // GetSound: the entry resolves
    std::function<Handle(const Sound&)> start;  // INgiWave slot 3 (play from the start)
    std::function<bool(Handle)> playing;        // INgiWave slot 0xb (IsPlaying)
    bool enabled = true;                        // CSoundServer +bEnabled

    void queue(const Sound& s) {
        if (!enabled || s.entry.empty() || (exists && !exists(s))) return;
        const bool idle = items_.empty();
        items_.push_back({s, 0});
        if (idle) items_.front().handle = start ? start(s) : 0;
    }
    void update() {
        if (items_.empty() || (playing && playing(items_.front().handle))) return;
        items_.pop_front();
        if (!items_.empty()) items_.front().handle = start ? start(items_.front().sound) : 0;
    }
    void clear() { items_.clear(); }
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }

private:
    struct Item {
        Sound sound;
        Handle handle = 0;
    };
    std::deque<Item> items_;
};

} // namespace parkan::audio
