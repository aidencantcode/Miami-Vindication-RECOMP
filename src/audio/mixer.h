#pragma once
// Software audio mixer on SDL2 (float stereo), portable to Android.
//   - clips: whole files decoded to PCM up front (short effects, like Android's SoundPool)
//   - streams: Ogg Vorbis decoded while playing (music, radio, speech, like MediaPlayer)
// Every voice has its own volume, playback rate (pitch, resampled with linear interpolation),
// loop flag and pause state, and belongs to a group with a user volume (music / effects / voice).
// All functions are called from the game thread; the SDL audio thread only mixes.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {

enum Group { kMusic, kSfx, kVoice, kGroups };

struct Clip {
    std::vector<float> pcm;  // interleaved
    int channels = 0, rate = 0;
    size_t frames = 0;
};
// Decodes an Ogg Vorbis file; nullptr if it is missing or broken.
std::shared_ptr<const Clip> load_clip(const std::string& path);

bool init(int sample_rate = 48000);
void shutdown();
bool ready();

using VoiceId = uint32_t;  // 0 = none
VoiceId play_clip(std::shared_ptr<const Clip> clip, float volume, float rate, bool loop, Group g);
// Opens the file and returns a paused voice (start it with set_paused(id, false)); 0 on failure.
VoiceId open_stream(const std::string& path, float volume, bool loop, Group g);

void stop(VoiceId v);
void set_paused(VoiceId v, bool paused);
void set_volume(VoiceId v, float volume);
void set_rate(VoiceId v, float rate);
void set_loop(VoiceId v, bool loop);
void rewind(VoiceId v);
bool alive(VoiceId v);    // exists and has not finished
bool playing(VoiceId v);  // alive and not paused
bool paused(VoiceId v);

void set_group_volume(Group g, float volume);  // user settings, 0..1
void set_master_volume(float volume);
void set_global_pause(bool paused);            // e.g. while the settings menu is open
int voice_count();

// Record the mixed output (for tests): everything mixed from now on is kept in memory and
// written as a 16-bit stereo WAV by write_capture().
void start_capture();
bool write_capture(const std::string& path);

}  // namespace audio
