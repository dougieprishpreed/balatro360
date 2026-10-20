// love.audio: a small software mixer.
//
// All sources are mixed on the CPU into 16-bit stereo at plat::AUDIO_RATE and
// handed to the platform's output (XAudio2 on the 360). Ogg Vorbis files
// are decoded with stb_vorbis: "static" sources are decoded once up front,
// "stream" sources are decoded incrementally while they play.
#include "modules.h"
#include "filesystem.h"

#include <math.h>
#include <string.h>
#include <algorithm>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace rt {

namespace {

// Decoded PCM shared by static sources and their clones.
class SoundBuffer : public Object {
public:
    std::vector<i16> samples;   // interleaved
    int channels;
    int rate;
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// Encoded file data shared by stream sources.
class StreamData : public Object {
public:
    std::vector<u8> data;
    int channels;
    int rate;
    double duration;
};

plat::Mutex& mixMutex() {
    static plat::Mutex m;
    return m;
}

class Source : public Object {
public:
    bool isStatic;
    Ref<SoundBuffer> buffer;
    Ref<StreamData> stream;
    stb_vorbis* vorbis;
    // Streaming window: decoded frames [winStart, winStart + winFrames).
    std::vector<i16> win;
    u64 winStart;
    size_t winFrames;
    bool streamEOF;

    double pos;   // playback position in source frames
    float volume;
    float pitch;
    bool looping;
    bool playing;
    bool paused;
    int channels;
    int rate;

    Source()
        : isStatic(true), vorbis(NULL), winStart(0), winFrames(0), streamEOF(false), pos(0),
          volume(1), pitch(1), looping(false), playing(false), paused(false), channels(1), rate(44100) {}

    ~Source() {
        if (vorbis) stb_vorbis_close(vorbis);
    }

    bool openStream() {
        if (vorbis) stb_vorbis_close(vorbis);
        int err = 0;
        vorbis = stb_vorbis_open_memory(&stream->data[0], (int)stream->data.size(), &err, NULL);
        winStart = 0;
        winFrames = 0;
        win.clear();
        streamEOF = false;
        return vorbis != NULL;
    }

    double durationSeconds() const {
        if (isStatic) return buffer ? (double)buffer->frames() / rate : 0;
        return stream ? stream->duration : 0;
    }

    // Fetch frame f (absolute) into out[2] as floats -1..1.
    // Returns false at end of data.
    bool frameAt(u64 f, float* out) {
        if (isStatic) {
            if (f >= buffer->frames()) return false;
            const i16* s = &buffer->samples[(size_t)f * channels];
            if (channels == 1) {
                out[0] = out[1] = s[0] * (1.0f / 32768.0f);
            } else {
                out[0] = s[0] * (1.0f / 32768.0f);
                out[1] = s[1] * (1.0f / 32768.0f);
            }
            return true;
        }
        if (!vorbis) return false;
        if (f < winStart) {
            // Seeking backwards (only on loop / seek): restart.
            if (f == 0 || !stb_vorbis_seek(vorbis, (unsigned int)f)) {
                openStream();
                if (f > 0 && !stb_vorbis_seek(vorbis, (unsigned int)f)) return false;
            }
            winStart = f;
            winFrames = 0;
            streamEOF = false;
        }
        while (f >= winStart + winFrames) {
            if (streamEOF) return false;
            // Keep the last frame of the old window for interpolation.
            u64 newStart = winStart + winFrames;
            if (winFrames > 0 && f > newStart + 4096) {
                // Large forward jump: seek.
                if (stb_vorbis_seek(vorbis, (unsigned int)f)) newStart = f;
            }
            const int chunk = 4096;
            win.resize(chunk * channels);
            int got = stb_vorbis_get_samples_short_interleaved(vorbis, channels, &win[0], chunk * channels);
            winStart = newStart;
            winFrames = got > 0 ? (size_t)got : 0;
            if (got <= 0) {
                streamEOF = true;
                return false;
            }
        }
        const i16* s = &win[(size_t)(f - winStart) * channels];
        if (channels == 1) {
            out[0] = out[1] = s[0] * (1.0f / 32768.0f);
        } else {
            out[0] = s[0] * (1.0f / 32768.0f);
            out[1] = s[1] * (1.0f / 32768.0f);
        }
        return true;
    }

    // Mix `frames` output frames into acc (stereo float). Returns false when
    // the source finished.
    bool mix(float* acc, int frames, float master) {
        double step = (double)rate / plat::AUDIO_RATE * pitch;
        float gain = volume * master;
        if (gain > 1.0f) gain = 1.0f;
        for (int i = 0; i < frames; ++i) {
            u64 f0 = (u64)pos;
            float frac = (float)(pos - (double)f0);
            float a[2], b[2];
            if (!frameAt(f0, a)) {
                if (looping && f0 > 0) {
                    pos = 0;
                    if (!isStatic) openStream();
                    --i;
                    continue;
                }
                return false;
            }
            if (!frameAt(f0 + 1, b)) {
                b[0] = a[0];
                b[1] = a[1];
            }
            if (gain > 0.0f) {
                acc[i * 2] += (a[0] + (b[0] - a[0]) * frac) * gain;
                acc[i * 2 + 1] += (a[1] + (b[1] - a[1]) * frac) * gain;
            }
            pos += step;
        }
        return true;
    }

    void rewind() {
        pos = 0;
        if (!isStatic && stream) openStream();
    }
};

// ------------------------------------------------------------------- mixer
std::vector<Source*> g_playing;   // each holds a reference
float g_masterVolume = 1.0f;
std::vector<float> g_acc;
bool g_audioStarted = false;

void mixCallback(i16* out, int frames, void* user) {
    (void)user;
    plat::Lock lock(mixMutex());
    if ((int)g_acc.size() < frames * 2) g_acc.resize(frames * 2);
    memset(&g_acc[0], 0, sizeof(float) * frames * 2);
    for (size_t i = 0; i < g_playing.size();) {
        Source* s = g_playing[i];
        if (s->paused) {
            ++i;
            continue;
        }
        if (!s->mix(&g_acc[0], frames, g_masterVolume)) {
            s->playing = false;
            s->rewind();
            g_playing.erase(g_playing.begin() + i);
            s->release();
            continue;
        }
        ++i;
    }
    for (int i = 0; i < frames * 2; ++i) {
        float v = g_acc[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = (i16)(v * 32767.0f);
    }
}

void ensureAudio() {
    if (g_audioStarted) return;
    g_audioStarted = true;
    if (!plat::audioStart(mixCallback, NULL)) plat::log("[audio] could not start audio output");
}

void playSource(Source* s) {
    plat::Lock lock(mixMutex());
    if (s->playing) {
        s->paused = false;
        return;
    }
    s->playing = true;
    s->paused = false;
    s->retain();
    g_playing.push_back(s);
}

void stopSource(Source* s) {
    plat::Lock lock(mixMutex());
    if (!s->playing) return;
    std::vector<Source*>::iterator it = std::find(g_playing.begin(), g_playing.end(), s);
    s->playing = false;
    s->paused = false;
    s->rewind();
    if (it != g_playing.end()) {
        g_playing.erase(it);
        s->release();
    }
}

// --------------------------------------------------------------- decoding

bool decodeStatic(const std::vector<u8>& file, SoundBuffer* buf) {
    int channels = 0, rate = 0;
    short* out = NULL;
    int frames = stb_vorbis_decode_memory(&file[0], (int)file.size(), &channels, &rate, &out);
    if (frames < 0 || !out) return false;
    buf->channels = channels;
    buf->rate = rate;
    buf->samples.assign(out, out + (size_t)frames * channels);
    free(out);
    return true;
}

Source* newSourceFromFile(lua_State* L, const std::string& path, bool isStatic) {
    std::vector<u8> file;
    if (!fs::read(path, file) || file.empty()) {
        luaL_error(L, "Could not open file %s. Does not exist.", path.c_str());
        return NULL;
    }
    Source* s = new Source;
    s->isStatic = isStatic;
    if (isStatic) {
        SoundBuffer* buf = new SoundBuffer;
        if (!decodeStatic(file, buf)) {
            buf->release();
            s->release();
            luaL_error(L, "Could not decode audio file %s", path.c_str());
            return NULL;
        }
        s->buffer = buf;
        buf->release();
        s->channels = buf->channels > 1 ? 2 : 1;
        if (buf->channels > 2) {
            // Downmix anything unusual to stereo by taking the first two channels.
            std::vector<i16> st(buf->frames() * 2);
            for (size_t f = 0; f < buf->frames(); ++f) {
                st[f * 2] = buf->samples[f * buf->channels];
                st[f * 2 + 1] = buf->samples[f * buf->channels + 1];
            }
            buf->samples.swap(st);
            buf->channels = 2;
        }
        s->rate = buf->rate;
    } else {
        StreamData* sd = new StreamData;
        sd->data.swap(file);
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(&sd->data[0], (int)sd->data.size(), &err, NULL);
        if (!v) {
            sd->release();
            s->release();
            luaL_error(L, "Could not decode audio file %s", path.c_str());
            return NULL;
        }
        stb_vorbis_info info = stb_vorbis_get_info(v);
        sd->channels = info.channels;
        sd->rate = info.sample_rate;
        unsigned int total = stb_vorbis_stream_length_in_samples(v);
        sd->duration = info.sample_rate ? (double)total / info.sample_rate : 0;
        stb_vorbis_close(v);
        s->stream = sd;
        sd->release();
        s->channels = sd->channels > 1 ? 2 : 1;
        s->rate = sd->rate;
        if (sd->channels > 2) {
            s->release();
            luaL_error(L, "Streaming sources with more than 2 channels are not supported (%s)", path.c_str());
            return NULL;
        }
        s->channels = sd->channels;
        s->openStream();
    }
    return s;
}

// ------------------------------------------------------------ Lua wrappers
Source* checkSource(lua_State* L, int idx) { return (Source*)luax_checkobject(L, idx, "Source"); }

int s_play(lua_State* L) {
    ensureAudio();
    playSource(checkSource(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

int s_stop(lua_State* L) {
    stopSource(checkSource(L, 1));
    return 0;
}

int s_pause(lua_State* L) {
    Source* s = checkSource(L, 1);
    plat::Lock lock(mixMutex());
    if (s->playing) s->paused = true;
    return 0;
}

int s_isPlaying(lua_State* L) {
    Source* s = checkSource(L, 1);
    plat::Lock lock(mixMutex());
    lua_pushboolean(L, s->playing && !s->paused);
    return 1;
}

int s_setVolume(lua_State* L) {
    Source* s = checkSource(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (v < 0) v = 0;
    plat::Lock lock(mixMutex());
    s->volume = v;
    return 0;
}

int s_getVolume(lua_State* L) {
    lua_pushnumber(L, checkSource(L, 1)->volume);
    return 1;
}

int s_setPitch(lua_State* L) {
    Source* s = checkSource(L, 1);
    float p = (float)luaL_checknumber(L, 2);
    if (!(p > 0.0f)) return luaL_error(L, "Pitch has to be non-zero, positive, finite number.");
    plat::Lock lock(mixMutex());
    s->pitch = p;
    return 0;
}

int s_getPitch(lua_State* L) {
    lua_pushnumber(L, checkSource(L, 1)->pitch);
    return 1;
}

int s_setLooping(lua_State* L) {
    Source* s = checkSource(L, 1);
    plat::Lock lock(mixMutex());
    s->looping = lua_toboolean(L, 2) != 0;
    return 0;
}

int s_isLooping(lua_State* L) {
    lua_pushboolean(L, checkSource(L, 1)->looping);
    return 1;
}

int s_seek(lua_State* L) {
    Source* s = checkSource(L, 1);
    double v = luaL_checknumber(L, 2);
    const char* unit = luaL_optstring(L, 3, "seconds");
    plat::Lock lock(mixMutex());
    s->pos = strcmp(unit, "samples") == 0 ? v : v * s->rate;
    if (s->pos < 0) s->pos = 0;
    return 0;
}

int s_tell(lua_State* L) {
    Source* s = checkSource(L, 1);
    const char* unit = luaL_optstring(L, 2, "seconds");
    plat::Lock lock(mixMutex());
    lua_pushnumber(L, strcmp(unit, "samples") == 0 ? floor(s->pos) : s->pos / s->rate);
    return 1;
}

int s_getDuration(lua_State* L) {
    Source* s = checkSource(L, 1);
    const char* unit = luaL_optstring(L, 2, "seconds");
    double d = s->durationSeconds();
    lua_pushnumber(L, strcmp(unit, "samples") == 0 ? floor(d * s->rate) : d);
    return 1;
}

int s_getChannelCount(lua_State* L) {
    lua_pushinteger(L, checkSource(L, 1)->channels);
    return 1;
}

int s_getType(lua_State* L) {
    lua_pushstring(L, checkSource(L, 1)->isStatic ? "static" : "stream");
    return 1;
}

int s_clone(lua_State* L) {
    Source* s = checkSource(L, 1);
    Source* c = new Source;
    c->isStatic = s->isStatic;
    c->buffer = s->buffer.get();
    c->stream = s->stream.get();
    c->volume = s->volume;
    c->pitch = s->pitch;
    c->looping = s->looping;
    c->channels = s->channels;
    c->rate = s->rate;
    if (!c->isStatic) c->openStream();
    luax_pushobject(L, "Source", c);
    c->release();
    return 1;
}

int s_noop(lua_State* L) {
    (void)L;
    return 0;
}

int s_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg kSourceMethods[] = {
    {"play", s_play}, {"stop", s_stop}, {"pause", s_pause}, {"isPlaying", s_isPlaying},
    {"setVolume", s_setVolume}, {"getVolume", s_getVolume}, {"setPitch", s_setPitch},
    {"getPitch", s_getPitch}, {"setLooping", s_setLooping}, {"isLooping", s_isLooping},
    {"seek", s_seek}, {"tell", s_tell}, {"getDuration", s_getDuration},
    {"getChannelCount", s_getChannelCount}, {"getType", s_getType}, {"clone", s_clone},
    {"setPosition", s_noop}, {"setRelative", s_noop}, {"setAttenuationDistances", s_noop},
    {"setVolumeLimits", s_noop}, {"isRelative", s_true}, {NULL, NULL}};

int w_newSource(lua_State* L) {
    std::string path = luax_checkstring(L, 1);
    const char* type = luaL_optstring(L, 2, "stream");
    bool isStatic = strcmp(type, "static") == 0;
    Source* s = newSourceFromFile(L, path, isStatic);
    luax_pushobject(L, "Source", s);
    s->release();
    return 1;
}

int w_play(lua_State* L) {
    ensureAudio();
    if (lua_istable(L, 1)) {
        int n = (int)lua_objlen(L, 1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            playSource(checkSource(L, -1));
            lua_pop(L, 1);
        }
    } else {
        for (int i = 1; i <= lua_gettop(L); ++i) playSource(checkSource(L, i));
    }
    lua_pushboolean(L, 1);
    return 1;
}

int w_stop(lua_State* L) {
    if (lua_gettop(L) == 0) {
        plat::Lock lock(mixMutex());
        for (size_t i = 0; i < g_playing.size(); ++i) {
            g_playing[i]->playing = false;
            g_playing[i]->paused = false;
            g_playing[i]->rewind();
            g_playing[i]->release();
        }
        g_playing.clear();
        return 0;
    }
    for (int i = 1; i <= lua_gettop(L); ++i) stopSource(checkSource(L, i));
    return 0;
}

int w_pause(lua_State* L) {
    plat::Lock lock(mixMutex());
    if (lua_gettop(L) == 0) {
        for (size_t i = 0; i < g_playing.size(); ++i) g_playing[i]->paused = true;
    } else {
        for (int i = 1; i <= lua_gettop(L); ++i) checkSource(L, i)->paused = true;
    }
    return 0;
}

int w_setVolume(lua_State* L) {
    plat::Lock lock(mixMutex());
    g_masterVolume = (float)luaL_checknumber(L, 1);
    return 0;
}

int w_getVolume(lua_State* L) {
    lua_pushnumber(L, g_masterVolume);
    return 1;
}

int w_getActiveSourceCount(lua_State* L) {
    plat::Lock lock(mixMutex());
    lua_pushinteger(L, (int)g_playing.size());
    return 1;
}

int w_getActiveEffects(lua_State* L) {
    lua_newtable(L);
    return 1;
}

const luaL_Reg kFuncs[] = {
    {"newSource", w_newSource}, {"play", w_play}, {"stop", w_stop}, {"pause", w_pause},
    {"setVolume", w_setVolume}, {"getVolume", w_getVolume},
    {"getActiveSourceCount", w_getActiveSourceCount}, {"setDistanceModel", s_noop},
    {"setPosition", s_noop}, {"setOrientation", s_noop}, {"setVelocity", s_noop},
    {"setDopplerScale", s_noop}, {"getActiveEffects", w_getActiveEffects}, {NULL, NULL}};

}  // namespace

void open_audio(lua_State* L) {
    TypeDef td = {"Source", NULL, kSourceMethods};
    luax_registertype(L, td);
    luax_registermodule(L, "audio", kFuncs);
}

void shutdownAudio() {
    if (g_audioStarted) plat::audioStop();
    g_audioStarted = false;
}

}  // namespace rt
