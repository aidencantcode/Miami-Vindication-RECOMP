#pragma once
// The Java GLMediaPlayer class of the Android build, re-implemented on audio::mixer.
// The game only passes sound indices (0..1735) and calls these static Java methods over JNI;
// behaviour follows the decompiled Java (analysis/jadx .../GLMediaPlayer.java):
//   *Sound    = android.media.SoundPool effects: one stream per sound index (playing it again
//               restarts it), no looping, rate = pitch, volume = v * sfxGain, clamped to 1
//   *SoundBig = android.media.MediaPlayer streams: volume = v * categoryGain / 40, where the
//               category comes from the Java file name: m_ music, sfx_ effects, vfx_/cin_ voice
// Deliberate differences from the phone (better behaviour, noted in NOTES.md):
//   - 16 effect voices instead of SoundPool's 5
//   - resuming a paused stream keeps its loop flag and volume (Java restarted it unlooped at 1.0)
//   - resumeAllSounds only resumes streams that pauseAllSounds paused
//   - optional: sounds whose Java name doesn't exist are found by the game's own (native) name (Q11)
#include <filesystem>
#include <functional>
#include <string>

namespace media {

struct Config {
    std::filesystem::path data_dir;     // the game's data folder (host path)
    std::filesystem::path table_file;   // Java sound-name table (tools/extract_sound_table.py)
    bool restore_missing = true;        // fall back to the native name (Q11)
    std::function<std::string(int)> native_name;  // the game's own name for a sound index
};
bool init(const Config& cfg);

// SoundPool side
void load_sound(int i);
void unload_sound(int i);
int is_sound_loaded(int i);  // 0 = loaded, -1 = not (Java convention)
void play_sound(int i, float volume, float rate);
void stop_sound(int i);
void pause_sound(int i);
void resume_sound(int i);
void set_pitch(int i, float rate);
void stop_all_pool(int except);
void reset_pool();

// MediaPlayer side
void load_sound_big(int i);
void unload_sound_big(int i);
int is_sound_loaded_big(int i);
void play_sound_big(int i, float volume, int loop);
void stop_sound_big(int i);
void pause_sound_big(int i);
void resume_sound_big(int i);
void set_volume_big(int i, float volume);
void reset_sound(int i);
int is_media_playing(int i);
void pause_all_sounds();
void resume_all_sounds();
void stop_all_big();

void set_music_gain(float g);
void set_sfx_gain(float g);
void set_vfx_gain(float g);

void report();  // log statistics (sounds played, missing files)

}  // namespace media
