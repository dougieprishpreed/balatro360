#include "filesystem.h"

#include <algorithm>

namespace rt {
namespace fs {

namespace {
plat::Mutex& fsMutex() {
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

bool read(const std::string& path, std::vector<u8>& out) {
    std::string p;
    if (!normalize(path, p) || p.empty()) return false;
    plat::Lock lock(fsMutex());
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
    plat::Lock lock(fsMutex());
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) plat::dirCreate(savePath(p.substr(0, slash)));
    return plat::fileWrite(savePath(p), data, size, append);
}

bool remove(const std::string& path) {
    std::string p;
    if (!normalize(path, p) || p.empty()) return false;
    plat::Lock lock(fsMutex());
    return plat::fileRemove(savePath(p));
}

bool createDirectory(const std::string& path) {
    std::string p;
    if (!normalize(path, p)) return false;
    plat::Lock lock(fsMutex());
    return plat::dirCreate(savePath(p));
}

bool info(const std::string& path, plat::PathInfo& out) {
    std::string p;
    if (!normalize(path, p)) return false;
    plat::Lock lock(fsMutex());
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
    plat::Lock lock(fsMutex());
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
