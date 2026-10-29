#include "threaded_device.h"

#include "../platform/platform.h"

#include <map>
#include <stdlib.h>
#include <string.h>

namespace gfx {

namespace {

const size_t kRenderStack = 4 * 1024 * 1024;   // glslang and xost compile shaders here
const double kMaxLerpGap = 0.1;                // packets further apart than this aren't interpolated
const float kMaxLerpMove = 192.0f;             // nor draws whose vertices move more than this (pixels)
const size_t kLookahead = 8;                   // draws of the packet before searched for a match
const size_t kCopyLimit = 1024 * 1024;         // bigger textures are made at once, not copied

class TTexture : public Texture {
public:
    Texture* real;
    TTexture() : real(NULL) {}
};

class TShader : public Shader {
public:
    Shader* real;
    std::vector<int> at, size;   // each uniform's place and float count in values
    std::vector<float> values;   // as the game last set them
    std::vector<bool> timed;     // named after time: averaged in interpolated frames
    u32 version, recorded;       // values changed / last copied into a packet
    u32 recordedIn;              // the packet they were last copied into

    TShader() : real(NULL), version(1), recorded(0), recordedIn(0) {}

    virtual void setUniform(int index, const float* v, int count) {
        if (index < 0 || index >= (int)at.size()) return;
        int n = count < size[index] ? count : size[index];
        if (n > 0) memcpy(&values[at[index]], v, n * sizeof(float));
        ++version;
    }
    virtual void setUniformTexture(int index, Texture* tex) {
        (void)index;
        (void)tex;   // Balatro only samples the main texture.
    }
};

struct Cmd {
    enum Op {
        CREATE_TEXTURE, CREATE_TARGET, UPDATE_TEXTURE, TEXTURE_PARAMS, DESTROY_TEXTURE, DESTROY_SHADER,
        TARGET, CLEAR, SCISSOR, COLOR_MASK, UNIFORMS, DRAW
    };
    int op;
    TTexture* tex;
    TShader* sh;
    TTexture* target;   // DRAW: what it draws into, for matching
    int a, b, c, d, e;
    float f[4];
    size_t at, n;       // into the packet's bytes, floats or vertices
};

// A frame's calls. Resource calls (creating, updating, destroying) run the
// first time the packet is replayed; the rest every time. What it destroys
// goes once the packet won't be replayed again.
struct Packet {
    std::vector<Cmd> cmds;
    std::vector<u8> bytes;
    std::vector<float> floats;
    std::vector<Vertex> verts;
    std::vector<TTexture*> deadTextures;
    std::vector<TShader*> deadShaders;
    double time;
    bool replayed, capture;

    Packet() : time(0), replayed(false), capture(false) {}
    void reset() {
        cmds.clear();
        bytes.clear();
        floats.clear();
        verts.clear();
        deadTextures.clear();
        deadShaders.clear();
        time = 0;
        replayed = capture = false;
    }
};

bool namedAfterTime(const std::string& name) {
    for (size_t i = 0; i + 4 <= name.size(); ++i) {
        if ((name[i] == 't' || name[i] == 'T') && name.compare(i + 1, 3, "ime") == 0) return true;
    }
    return false;
}

class ThreadedDevice : public Device {
public:
    ThreadedDevice(Device* r, int c)
        : real(r), core(c), w(0), h(0), rec(new Packet), recSerial(1), target(NULL), pending(NULL), quit(false),
          job(NULL),
          jobDone(false), interval(1), interpolate(false), shotWanted(false), shotReady(false), thread(NULL),
          prev(NULL), realInterval(1), shootInterpolated(false) {
        memset(&scissor, 0, sizeof(scissor));
        scissor.op = Cmd::SCISSOR;
        mask = 15;
#ifndef __XSDK__
        const char* env = getenv("B360_SHOT_INTERPOLATED");
        shootInterpolated = env && env[0] == '1';
#endif
    }

    virtual ~ThreadedDevice() {
        {
            plat::Lock l(mu);
            quit = true;
            cv.broadcast();
        }
        plat::threadJoin(thread);
        delete real;
    }

    // --------------------------------------------------- the game's thread
    virtual bool init(int width, int height) {
        w = width;
        h = height;
        thread = plat::threadCreate(renderMain, this, core, kRenderStack);
        if (!thread) return false;
        Job j(Job::INIT);
        run(j);
        if (j.ok) name = real->rendererName();
        return j.ok;
    }

    virtual int width() const { return w; }
    virtual int height() const { return h; }

    virtual Texture* createTexture(int tw, int th, const u8* pixels, bool mipmaps) {
        TTexture* t = new TTexture;
        t->width = tw;
        t->height = th;
        if (pixels && (size_t)tw * th * 4 >= kCopyLimit) {
            // Made now rather than copied into the packet: the card atlases
            // come to tens of MB while the game loads.
            Job j(Job::CREATE_TEXTURE);
            j.tex = t;
            j.pixels = pixels;
            j.mipmaps = mipmaps;
            run(j);
            return t;
        }
        Cmd& c = add(Cmd::CREATE_TEXTURE);
        c.tex = t;
        c.a = tw;
        c.b = th;
        c.c = mipmaps ? 1 : 0;
        if (pixels) copyBytes(c, pixels, (size_t)tw * th * 4);
        return t;
    }

    virtual Texture* createRenderTarget(int tw, int th) {
        TTexture* t = new TTexture;
        t->width = tw;
        t->height = th;
        t->renderTarget = true;
        Cmd& c = add(Cmd::CREATE_TARGET);
        c.tex = t;
        c.a = tw;
        c.b = th;
        return t;
    }

    virtual void updateTexture(Texture* tex, int x, int y, int tw, int th, const u8* pixels) {
        Cmd& c = add(Cmd::UPDATE_TEXTURE);
        c.tex = (TTexture*)tex;
        c.a = x;
        c.b = y;
        c.c = tw;
        c.d = th;
        copyBytes(c, pixels, (size_t)tw * th * 4);
    }

    virtual void destroyTexture(Texture* tex) {
        Cmd& c = add(Cmd::DESTROY_TEXTURE);
        c.tex = (TTexture*)tex;
        if (target == c.tex) target = NULL;
    }

    virtual void setTextureParams(Texture* tex, Filter minF, Filter magF, Wrap ws, Wrap wt) {
        tex->minFilter = minF;
        tex->magFilter = magF;
        tex->wrapS = ws;
        tex->wrapT = wt;
        Cmd& c = add(Cmd::TEXTURE_PARAMS);
        c.tex = (TTexture*)tex;
        c.a = (int)minF;
        c.b = (int)magF;
        c.c = (int)ws;
        c.d = (int)wt;
    }

    virtual Shader* createShader(const std::string& source, std::string& err) {
        Job j(Job::CREATE_SHADER);
        j.source = source;
        run(j);
        err = j.err;
        if (!j.shader) return NULL;
        TShader* s = new TShader;
        s->real = j.shader;
        s->uniforms = j.shader->uniforms;
        int n = 0;
        for (size_t i = 0; i < s->uniforms.size(); ++i) {
            const UniformInfo& u = s->uniforms[i];
            int k = u.type == UNIFORM_SAMPLER ? 0 : u.components * (u.type == UNIFORM_MAT ? u.components : 1);
            k *= u.arraySize < 1 ? 1 : u.arraySize;
            s->at.push_back(n);
            s->size.push_back(k);
            s->timed.push_back(namedAfterTime(u.name));
            n += k;
        }
        s->values.assign(n, 0.0f);
        return s;
    }

    virtual void destroyShader(Shader* sh) {
        Cmd& c = add(Cmd::DESTROY_SHADER);
        c.sh = (TShader*)sh;
    }

    virtual void setRenderTarget(Texture* rt) {
        Cmd& c = add(Cmd::TARGET);
        c.tex = (TTexture*)rt;
        target = c.tex;
    }

    virtual void clear(float r, float g, float b, float a) {
        Cmd& c = add(Cmd::CLEAR);
        c.f[0] = r;
        c.f[1] = g;
        c.f[2] = b;
        c.f[3] = a;
    }

    virtual void setScissor(bool enabled, int x, int y, int sw, int sh) {
        Cmd& c = add(Cmd::SCISSOR);
        c.a = enabled ? 1 : 0;
        c.b = x;
        c.c = y;
        c.d = sw;
        c.e = sh;
        scissor = c;
    }

    virtual void setColorMask(bool r, bool g, bool b, bool a) {
        Cmd& c = add(Cmd::COLOR_MASK);
        c.a = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
        mask = c.a;
    }

    virtual void drawTriangles(const Vertex* verts, int count, Texture* tex, Shader* shader, BlendMode blend) {
        if (count <= 0 || !tex) return;
        TShader* s = (TShader*)shader;
        if (s && (s->version != s->recorded || s->recordedIn != recSerial)) {
            // Changed values, and each shader's at its first draw in a
            // packet: a packet replays the same whatever was replayed before
            // it (an interpolated frame leaves averaged values behind).
            Cmd& u = add(Cmd::UNIFORMS);
            u.sh = s;
            u.at = rec->floats.size();
            u.n = s->values.size();
            rec->floats.insert(rec->floats.end(), s->values.begin(), s->values.end());
            s->recorded = s->version;
            s->recordedIn = recSerial;
        }
        Cmd& c = add(Cmd::DRAW);
        c.tex = (TTexture*)tex;
        c.sh = s;
        c.target = target;
        c.a = (int)blend;
        c.b = count;
        c.c = s && s->halfRes ? 1 : 0;
        c.at = rec->verts.size();
        rec->verts.insert(rec->verts.end(), verts, verts + count);
    }

    // Hands the frame to the render thread, waiting while the one before
    // hasn't been taken yet.
    virtual void present() {
        rec->time = plat::getTime();
        Packet* next = NULL;
        {
            plat::Lock l(mu);
            while (pending && !quit) cv.wait(mu);
            pending = rec;
            cv.broadcast();
            if (!spare.empty()) {
                next = spare.back();
                spare.pop_back();
            }
        }
        rec = next ? next : new Packet;
        ++recSerial;
        // The state the frame starts in, as a replay ends in another.
        Cmd& t = add(Cmd::TARGET);
        t.tex = target;
        Cmd& s = add(Cmd::SCISSOR);
        s = scissor;
        Cmd& m = add(Cmd::COLOR_MASK);
        m.a = mask;
    }

    virtual bool setPresentInterval(int vblanks) {
        plat::Lock l(mu);
        interval = vblanks >= 2 ? 2 : 1;
        return true;
    }

    virtual bool setFrameInterpolation(bool on) {
        plat::Lock l(mu);
        interpolate = on;
        return true;
    }

    virtual bool frameInterpolation() const {
        plat::Lock l(mu);
        return interpolate && interval >= 2;
    }

    // A screenshot is of the frame being recorded when captureNextFrame() is
    // called; readBackbuffer() returns it once the render thread drew it.
    virtual bool readBackbuffer(std::vector<u8>& rgba) {
        plat::Lock l(mu);
        if (!shotReady) return false;
        rgba.swap(shot);
        shot.clear();
        shotReady = shotWanted = false;
        return true;
    }

    virtual bool captureNextFrame() {
        plat::Lock l(mu);
        if (!shotWanted) {
            shotWanted = true;
            rec->capture = true;
        }
        return true;
    }

    virtual bool setGpuProfile(double seconds) {
        Job j(Job::GPU_PROFILE);
        j.seconds = seconds;
        run(j);
        return j.ok;
    }

    virtual const char* rendererName() const { return name.c_str(); }

private:
    struct Job {
        enum Kind { INIT, CREATE_SHADER, CREATE_TEXTURE, GPU_PROFILE };
        Kind kind;
        std::string source, err;
        Shader* shader;
        TTexture* tex;
        const u8* pixels;
        bool mipmaps;
        double seconds;
        bool ok;
        explicit Job(Kind k)
            : kind(k), shader(NULL), tex(NULL), pixels(NULL), mipmaps(false), seconds(0), ok(false) {}
    };

    Device* real;
    int core;
    int w, h;
    std::string name;

    // The game's thread.
    Packet* rec;          // being recorded
    u32 recSerial;        // counts packets recorded
    TTexture* target;     // set by setRenderTarget, for matching draws
    Cmd scissor;          // the last SCISSOR, and colour mask
    int mask;

    // Shared, under mu.
    mutable plat::Mutex mu;
    plat::Cond cv;
    Packet* pending;      // handed over, not taken yet
    std::vector<Packet*> spare;
    bool quit;
    Job* job;
    bool jobDone;
    int interval;         // vblanks a game frame
    bool interpolate;
    bool shotWanted, shotReady;
    std::vector<u8> shot;
    plat::Thread* thread;

    // The render thread.
    Packet* prev;         // the last packet replayed, the source of interpolation
    int realInterval;
    bool shootInterpolated;
    std::vector<Vertex> lerped;
    std::vector<float> lerpedUniforms;

    Cmd& add(int op) {
        rec->cmds.push_back(Cmd());
        Cmd& c = rec->cmds.back();
        memset(&c, 0, sizeof(c));
        c.op = op;
        return c;
    }

    void copyBytes(Cmd& c, const u8* p, size_t n) {
        c.at = rec->bytes.size();
        c.n = n;
        rec->bytes.insert(rec->bytes.end(), p, p + n);
    }

    // Runs j on the render thread and waits for it.
    void run(Job& j) {
        plat::Lock l(mu);
        while (job) cv.wait(mu);
        job = &j;
        jobDone = false;
        cv.broadcast();
        while (!jobDone) cv.wait(mu);
        job = NULL;
        cv.broadcast();
    }

    // -------------------------------------------------- the render thread
    static void renderMain(void* arg) { ((ThreadedDevice*)arg)->renderLoop(); }

    void doJob(Job& j) {
        switch (j.kind) {
        case Job::INIT: j.ok = real->init(w, h); break;
        case Job::CREATE_SHADER:
            j.shader = real->createShader(j.source, j.err);
            j.ok = j.shader != NULL;
            break;
        case Job::CREATE_TEXTURE:
            j.tex->real = real->createTexture(j.tex->width, j.tex->height, j.pixels, j.mipmaps);
            j.ok = j.tex->real != NULL;
            break;
        case Job::GPU_PROFILE: j.ok = real->setGpuProfile(j.seconds); break;
        }
    }

    void renderLoop() {
        for (;;) {
            Packet* p = NULL;
            int every;
            bool lerp;
            {
                plat::Lock l(mu);
                for (;;) {
                    if (job && !jobDone) {
                        doJob(*job);
                        jobDone = true;
                        cv.broadcast();
                        continue;
                    }
                    if (quit) return;
                    if (pending) {
                        p = pending;
                        pending = NULL;
                        cv.broadcast();
                        break;
                    }
                    cv.wait(mu);
                }
                every = interval;
                lerp = interpolate;
            }
            // Without interpolation the real device shows each frame for
            // the game's vblanks; with it, it presents every vblank.
            int want = every >= 2 && !lerp ? 2 : 1;
            if (want != realInterval) {
                real->setPresentInterval(want);
                realInterval = want;
            }
            bool capture = p->capture;
            if (every >= 2 && lerp && prev && p->time - prev->time < kMaxLerpGap) {
                // Host test runs can screenshot this frame instead
                // (B360_SHOT_INTERPOLATED=1).
                bool shootThis = capture && shootInterpolated;
                if (shootThis) real->captureNextFrame();
                replay(*p, prev, 0.5f);
                if (shootThis) {
                    takeShot();
                    capture = false;
                }
                real->present();
            }
            if (capture) real->captureNextFrame();
            replay(*p, NULL, 0.0f);
            if (capture) takeShot();
            real->present();
            retire(prev);
            prev = p;
        }
    }

    void takeShot() {
        std::vector<u8> rgba;
        bool ok = real->readBackbuffer(rgba);
        plat::Lock l(mu);
        if (ok) {
            shot.swap(rgba);
            shotReady = true;
        } else {
            shotWanted = false;
        }
    }

    // A packet that won't be replayed again: what it destroyed goes, and
    // the packet is kept for recording.
    void retire(Packet* p) {
        if (!p) return;
        for (size_t i = 0; i < p->deadTextures.size(); ++i) {
            TTexture* t = p->deadTextures[i];
            if (t->real) real->destroyTexture(t->real);
            delete t;
        }
        for (size_t i = 0; i < p->deadShaders.size(); ++i) {
            TShader* s = p->deadShaders[i];
            if (s->real) real->destroyShader(s->real);
            delete s;
        }
        p->reset();
        plat::Lock l(mu);
        spare.push_back(p);
    }

    // Replays p; with from, at t of the way from it to p (interpolated).
    void replay(Packet& p, const Packet* from, float t) {
        bool first = !p.replayed;
        std::vector<size_t> fromDraws;
        std::map<TShader*, size_t> fromUniforms;   // each shader's last values in from
        if (from) {
            for (size_t i = 0; i < from->cmds.size(); ++i) {
                const Cmd& c = from->cmds[i];
                if (c.op == Cmd::DRAW) fromDraws.push_back(i);
                else if (c.op == Cmd::UNIFORMS) fromUniforms[c.sh] = c.at;
            }
        }
        size_t next = 0;   // the first of from's draws not matched yet
        for (size_t i = 0; i < p.cmds.size(); ++i) {
            Cmd& c = p.cmds[i];
            switch (c.op) {
            case Cmd::CREATE_TEXTURE:
                if (first) c.tex->real = real->createTexture(c.a, c.b, c.n ? &p.bytes[c.at] : NULL, c.c != 0);
                break;
            case Cmd::CREATE_TARGET:
                if (first) c.tex->real = real->createRenderTarget(c.a, c.b);
                break;
            case Cmd::UPDATE_TEXTURE:
                if (first && c.tex->real) real->updateTexture(c.tex->real, c.a, c.b, c.c, c.d, &p.bytes[c.at]);
                break;
            case Cmd::TEXTURE_PARAMS:
                if (first && c.tex->real)
                    real->setTextureParams(c.tex->real, (Filter)c.a, (Filter)c.b, (Wrap)c.c, (Wrap)c.d);
                break;
            case Cmd::DESTROY_TEXTURE:
                if (first) p.deadTextures.push_back(c.tex);
                break;
            case Cmd::DESTROY_SHADER:
                if (first) p.deadShaders.push_back(c.sh);
                break;
            case Cmd::TARGET: real->setRenderTarget(c.tex ? c.tex->real : NULL); break;
            case Cmd::CLEAR: real->clear(c.f[0], c.f[1], c.f[2], c.f[3]); break;
            case Cmd::SCISSOR: real->setScissor(c.a != 0, c.b, c.c, c.d, c.e); break;
            case Cmd::COLOR_MASK:
                real->setColorMask((c.a & 1) != 0, (c.a & 2) != 0, (c.a & 4) != 0, (c.a & 8) != 0);
                break;
            case Cmd::UNIFORMS: replayUniforms(p, c, from, fromUniforms, t); break;
            case Cmd::DRAW: {
                TShader* s = c.sh;
                Shader* rs = s ? s->real : NULL;
                if (!c.tex->real || (s && !rs)) break;
                if (rs) rs->halfRes = c.c != 0;
                const Vertex* v = &p.verts[c.at];
                if (from) v = lerpDraw(p, c, *from, fromDraws, next, t);
                real->drawTriangles(v, c.b, c.tex->real, rs, (BlendMode)c.a);
                break;
            }
            }
        }
        p.replayed = true;
    }

    void replayUniforms(const Packet& p, const Cmd& c, const Packet* from,
                        const std::map<TShader*, size_t>& fromUniforms, float t) {
        TShader* s = c.sh;
        if (!s->real || c.n == 0) return;
        if (s->real->name != s->name) s->real->name = s->name;
        const float* v = &p.floats[c.at];
        std::map<TShader*, size_t>::const_iterator it = from ? fromUniforms.find(s) : fromUniforms.end();
        if (from && it != fromUniforms.end()) {
            lerpedUniforms.assign(v, v + c.n);
            const float* a = &from->floats[it->second];
            for (size_t u = 0; u < s->at.size(); ++u) {
                if (!s->timed[u]) continue;
                for (int k = 0; k < s->size[u]; ++k) {
                    float& x = lerpedUniforms[s->at[u] + k];
                    x = a[s->at[u] + k] + (x - a[s->at[u] + k]) * t;
                }
            }
            v = &lerpedUniforms[0];
        }
        for (size_t u = 0; u < s->at.size(); ++u)
            if (s->size[u] > 0) s->real->setUniform((int)u, v + s->at[u], s->size[u]);
    }

    // c's vertices, averaged with its match among from's draws when there's
    // one close enough.
    const Vertex* lerpDraw(const Packet& p, const Cmd& c, const Packet& from, const std::vector<size_t>& fromDraws,
                           size_t& next, float t) {
        const Vertex* b = &p.verts[c.at];
        for (size_t k = next; k < fromDraws.size() && k < next + kLookahead; ++k) {
            const Cmd& o = from.cmds[fromDraws[k]];
            if (o.tex != c.tex || o.sh != c.sh || o.target != c.target || o.a != c.a || o.b != c.b) continue;
            next = k + 1;
            const Vertex* a = &from.verts[o.at];
            for (int i = 0; i < c.b; ++i) {
                float dx = b[i].x - a[i].x, dy = b[i].y - a[i].y;
                if (dx > kMaxLerpMove || dx < -kMaxLerpMove || dy > kMaxLerpMove || dy < -kMaxLerpMove) return b;
            }
            lerped.assign(b, b + c.b);
            for (int i = 0; i < c.b; ++i) {
                Vertex& v = lerped[i];
                v.x = a[i].x + (b[i].x - a[i].x) * t;
                v.y = a[i].y + (b[i].y - a[i].y) * t;
                v.r = a[i].r + (b[i].r - a[i].r) * t;
                v.g = a[i].g + (b[i].g - a[i].g) * t;
                v.b = a[i].b + (b[i].b - a[i].b) * t;
                v.a = a[i].a + (b[i].a - a[i].a) * t;
            }
            return &lerped[0];
        }
        return b;
    }
};

}  // namespace

Device* createThreadedDevice(Device* real, int core) { return new ThreadedDevice(real, core); }

}  // namespace gfx
