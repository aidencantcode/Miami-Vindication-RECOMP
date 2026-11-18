#include "audio/mixer.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "core/log.h"

// stb_vorbis (public domain), compiled into this file only.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#undef L
#undef C
#undef R
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace audio {

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// A streamed Ogg file: compressed bytes in memory, decoded in small chunks into a sliding
// window of frames so the resampler can read frame i and i+1.
struct Stream {
    std::vector<uint8_t> file;
    stb_vorbis* vorbis = nullptr;
    int channels = 0, rate = 0;
    std::vector<float> win;   // interleaved frames [win_start, win_start + win_frames)
    uint64_t win_start = 0;
    size_t win_frames = 0;
    bool eof = false;
    ~Stream() { if (vorbis) stb_vorbis_close(vorbis); }

    void restart() {
        stb_vorbis_seek_start(vorbis);
        win.clear();
        win_start = 0;
        win_frames = 0;
        eof = false;
    }
    // Make sure frames [f, f+1] are in the window if the file has them. Sequential access only.
    void need(uint64_t f) {
        while (!eof && f + 1 >= win_start + win_frames) {
            // keep the last frame (for interpolation), drop the rest
            if (win_frames > 1) {
                const size_t keep = 1;
                std::memmove(win.data(), win.data() + (win_frames - keep) * channels, keep * channels * sizeof(float));
                win_start += win_frames - keep;
                win_frames = keep;
            }
            float tmp[4096];
            const int got = stb_vorbis_get_samples_float_interleaved(vorbis, channels, tmp, 4096 - 4096 % channels);
            if (got <= 0) { eof = true; break; }
            win.resize((win_frames + got) * channels);
            std::memcpy(win.data() + win_frames * channels, tmp, size_t(got) * channels * sizeof(float));
            win_frames += got;
        }
    }
};

struct Voice {
    VoiceId id = 0;
    std::shared_ptr<const Clip> clip;
    std::unique_ptr<Stream> stream;
    Group group = kSfx;
    double pos = 0;  // in source frames
    float volume = 1, rate = 1;
    bool loop = false, paused = false, done = false;
    int channels() const { return clip ? clip->channels : stream->channels; }
    int src_rate() const { return clip ? clip->rate : stream->rate; }
};

SDL_AudioDeviceID g_dev = 0;
int g_rate = 48000;
std::vector<std::unique_ptr<Voice>> g_voices;  // guarded by the audio device lock
VoiceId g_next_id = 1;
float g_group[kGroups] = {1, 1, 1};
float g_master = 1;
bool g_global_pause = false;
bool g_capturing = false;
std::vector<int16_t> g_capture;  // interleaved stereo

struct Lock {
    Lock() { if (g_dev) SDL_LockAudioDevice(g_dev); }
    ~Lock() { if (g_dev) SDL_UnlockAudioDevice(g_dev); }
};

Voice* find(VoiceId id) {
    for (auto& v : g_voices)
        if (v->id == id) return v.get();
    return nullptr;
}

// Reads source frame f of voice v into out[2] (stereo). Returns false past the end.
bool frame(Voice& v, uint64_t f, float out[2]) {
    const int ch = v.channels();
    const float* p;
    if (v.clip) {
        if (f >= v.clip->frames) return false;
        p = v.clip->pcm.data() + f * ch;
    } else {
        Stream& s = *v.stream;
        s.need(f);
        if (f < s.win_start || f >= s.win_start + s.win_frames) return false;
        p = s.win.data() + (f - s.win_start) * ch;
    }
    out[0] = p[0];
    out[1] = ch > 1 ? p[1] : p[0];
    return true;
}

void SDLCALL mix(void*, Uint8* bytes, int len) {
    float* out = reinterpret_cast<float*>(bytes);
    const int n = len / int(sizeof(float) * 2);
    std::fill(out, out + n * 2, 0.0f);
    if (g_global_pause) return;
    for (auto& vp : g_voices) {
        Voice& v = *vp;
        if (v.paused || v.done) continue;
        const float gain = v.volume * g_group[v.group] * g_master;
        const double step = double(v.src_rate()) * v.rate / g_rate;
        for (int i = 0; i < n; ++i) {
            uint64_t f = uint64_t(v.pos);
            float a[2], b[2];
            if (!frame(v, f, a)) {
                if (!v.loop) { v.done = true; break; }
                if (v.stream) v.stream->restart();
                v.pos = 0;
                f = 0;
                if (!frame(v, 0, a)) { v.done = true; break; }
            }
            if (!frame(v, f + 1, b)) { b[0] = a[0]; b[1] = a[1]; }
            const float t = float(v.pos - double(f));
            out[2 * i] += (a[0] + (b[0] - a[0]) * t) * gain;
            out[2 * i + 1] += (a[1] + (b[1] - a[1]) * t) * gain;
            v.pos += step;
        }
    }
    for (int i = 0; i < n * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
    if (g_capturing)
        for (int i = 0; i < n * 2; ++i) g_capture.push_back(int16_t(std::lround(out[i] * 32767.0f)));
}

}  // namespace

std::shared_ptr<const Clip> load_clip(const std::string& path) {
    std::vector<uint8_t> file = read_file(path);
    if (file.empty()) return nullptr;
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(file.data(), int(file.size()), &channels, &rate, &pcm);
    if (frames <= 0 || !pcm) {
        LOG("audio: cannot decode %s", path.c_str());
        return nullptr;
    }
    auto c = std::make_shared<Clip>();
    c->channels = channels;
    c->rate = rate;
    c->frames = size_t(frames);
    c->pcm.resize(size_t(frames) * channels);
    for (size_t i = 0; i < c->pcm.size(); ++i) c->pcm[i] = pcm[i] / 32768.0f;
    std::free(pcm);
    return c;
}

bool init(int sample_rate) {
    if (g_dev) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG("audio: SDL audio init failed: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = sample_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_dev) {
        LOG("audio: no output device: %s", SDL_GetError());
        return false;
    }
    g_rate = have.freq;
    SDL_PauseAudioDevice(g_dev, 0);
    LOG("audio: default output device (%s driver), %d Hz stereo float, %d-frame buffer",
        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", g_rate, int(have.samples));
    return true;
}

void shutdown() {
    if (!g_dev) return;
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
    g_voices.clear();
}

bool ready() { return g_dev != 0; }

VoiceId play_clip(std::shared_ptr<const Clip> clip, float volume, float rate, bool loop, Group g) {
    if (!g_dev || !clip || !clip->frames) return 0;
    auto v = std::make_unique<Voice>();
    v->clip = std::move(clip);
    v->volume = volume;
    v->rate = rate;
    v->loop = loop;
    v->group = g;
    Lock lock;
    v->id = g_next_id++;
    const VoiceId id = v->id;
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const auto& x) { return x->done; }), g_voices.end());
    g_voices.push_back(std::move(v));
    return id;
}

VoiceId open_stream(const std::string& path, float volume, bool loop, Group g) {
    if (!g_dev) return 0;
    auto s = std::make_unique<Stream>();
    s->file = read_file(path);
    if (s->file.empty()) return 0;
    int err = 0;
    s->vorbis = stb_vorbis_open_memory(s->file.data(), int(s->file.size()), &err, nullptr);
    if (!s->vorbis) {
        LOG("audio: cannot open %s (stb_vorbis error %d)", path.c_str(), err);
        return 0;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(s->vorbis);
    s->channels = info.channels;
    s->rate = int(info.sample_rate);
    auto v = std::make_unique<Voice>();
    v->stream = std::move(s);
    v->volume = volume;
    v->loop = loop;
    v->group = g;
    v->paused = true;
    Lock lock;
    v->id = g_next_id++;
    const VoiceId id = v->id;
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const auto& x) { return x->done; }), g_voices.end());
    g_voices.push_back(std::move(v));
    return id;
}

void stop(VoiceId id) {
    Lock lock;
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [&](const auto& x) { return x->id == id; }),
                   g_voices.end());
}

void set_paused(VoiceId id, bool p) { Lock lock; if (Voice* v = find(id)) v->paused = p; }
void set_volume(VoiceId id, float vol) { Lock lock; if (Voice* v = find(id)) v->volume = vol; }
void set_rate(VoiceId id, float r) { Lock lock; if (Voice* v = find(id)) v->rate = std::clamp(r, 0.25f, 4.0f); }
void set_loop(VoiceId id, bool l) { Lock lock; if (Voice* v = find(id)) v->loop = l; }

void rewind(VoiceId id) {
    Lock lock;
    if (Voice* v = find(id)) {
        if (v->stream) v->stream->restart();
        v->pos = 0;
        v->done = false;
    }
}

bool alive(VoiceId id) { Lock lock; Voice* v = find(id); return v && !v->done; }
bool playing(VoiceId id) { Lock lock; Voice* v = find(id); return v && !v->done && !v->paused; }
bool paused(VoiceId id) { Lock lock; Voice* v = find(id); return v && !v->done && v->paused; }

void set_group_volume(Group g, float vol) { Lock lock; g_group[g] = std::clamp(vol, 0.0f, 1.0f); }
void set_master_volume(float vol) { Lock lock; g_master = std::clamp(vol, 0.0f, 1.0f); }
void set_global_pause(bool p) { Lock lock; g_global_pause = p; }
int voice_count() { Lock lock; return int(g_voices.size()); }

void start_capture() {
    Lock lock;
    g_capture.clear();
    g_capture.reserve(size_t(g_rate) * 2 * 120);
    g_capturing = true;
}

bool write_capture(const std::string& path) {
    std::vector<int16_t> pcm;
    {
        Lock lock;
        g_capturing = false;
        pcm.swap(g_capture);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(uint32_t(g_rate)); u32(uint32_t(g_rate) * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    LOG("audio: wrote %.1f s of output to %s", pcm.size() / 2.0 / g_rate, path.c_str());
    return bool(f);
}

}  // namespace audio
