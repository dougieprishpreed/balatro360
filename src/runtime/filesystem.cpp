#include "filesystem.h"

#include <algorithm>

namespace rt {
namespace fs {

namespace {
// Serialises writers (Balatro's save thread vs. deletes from the game
// thread). Reads, stats and listings take no lock: the platform calls are
// thread safe, and a read must never wait for another thread's file I/O.
// (A global lock here made the game thread wait whenever the sound thread
// loaded a multi-megabyte music track from USB.)
plat::Mutex& writeMutex() {
    static plat::Mutex m;
    return m;
}
}  // namespace

void init() { plat::dirCreate(plat::saveRoot()); }

bool normalize(const std::string& in, std::string& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= in.size(); ++i) {
        char c = i < in.size() ? in[i] : '/';
        if (c == '\\') c = '/';
        if (c == '/') {
            if (cur == "..") return false;
            if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.clear();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return true;
}

static std::string savePath(const std::string& p) { return plat::joinPath(plat::saveRoot(), p); }
static std::string sourcePath(const std::string& p) { return plat::joinPath(plat::sourceRoot(), p); }

// Logs file operations slow enough to cause a visible hitch if they happen
// on the game thread (the log names the file, so hitches can be traced).
struct SlowOpLog {
    const char* what;
    const std::string& path;
    double t0;
    SlowOpLog(const char* w, const std::string& p) : what(w), path(p), t0(plat::getTime()) {}
    ~SlowOpLog() {
        double ms = (plat::getTime() - t0) * 1000.0;
        if (ms >= 50.0) plat::log("[fs] %s %s took %.0f ms", what, path.c_str(), ms);
    }
};

bool read(const std::string& path, std::vector<u8>& out) {
    std::string p;
    if (!normalize(path, p) || p.empty()) return false;
    SlowOpLog slow("reading", p);
    if (plat::fileRead(savePath(p), out)) return true;
    return plat::fileRead(sourcePath(p), out);
}

bool read(const std::string& path, std::string& out) {
    std::vector<u8> data;
    if (!read(path, data)) return false;
    out.assign(data.begin(), data.end());
    return true;
}

bool write(const std::string& path, const void* data, size_t size, bool append) {
    std::string p;
    if (!normalize(path, p) || p.empty()) return false;
    SlowOpLog slow("writing", p);
    plat::Lock lock(writeMutex());
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) plat::dirCreate(savePath(p.substr(0, slash)));
    return plat::fileWrite(savePath(p), data, size, append);
}

bool remove(const std::string& path) {
    std::string p;
    if (!normalize(path, p) || p.empty()) return false;
    plat::Lock lock(writeMutex());
    return plat::fileRemove(savePath(p));
}

bool createDirectory(const std::string& path) {
    std::string p;
    if (!normalize(path, p)) return false;
    plat::Lock lock(writeMutex());
    return plat::dirCreate(savePath(p));
}

bool info(const std::string& path, plat::PathInfo& out) {
    std::string p;
    if (!normalize(path, p)) return false;
    if (p.empty()) {
        out.type = plat::PATH_DIR;
        out.size = 0;
        out.modtime = 0;
        return true;
    }
    if (plat::pathInfo(savePath(p), out)) return true;
    return plat::pathInfo(sourcePath(p), out);
}

void list(const std::string& path, std::vector<std::string>& names) {
    std::string p;
    if (!normalize(path, p)) return;
    std::vector<std::string> a, b;
    plat::dirList(savePath(p), a);
    plat::dirList(sourcePath(p), b);
    names = a;
    for (size_t i = 0; i < b.size(); ++i)
        if (std::find(names.begin(), names.end(), b[i]) == names.end()) names.push_back(b[i]);
    // LÖVE (PhysFS) returns entries sorted; Balatro relies on a stable order.
    std::sort(names.begin(), names.end());
}

std::string saveDirectory() { return plat::saveRoot(); }
std::string sourceDirectory() { return plat::sourceRoot(); }

}  // namespace fs
}  // namespace rt
