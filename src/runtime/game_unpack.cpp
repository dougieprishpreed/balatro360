// First-start unpacking of the player's Balatro.exe (see game_unpack.h).
//
// The archive is found from its end of central directory record, as
// tools/prepare_game.py does: the record gives the central directory's size
// and its offset from the archive's start, so the archive starts at
// record - size - offset (0 for a Balatro.love). Entries are stored or
// deflated (miniz's tinfl) and checked against their CRC-32. The PC-only
// .dll and .exe files are left out, and main.lua is written last, so a game
// folder without it is never a finished one.
#include "game_unpack.h"
#include "../platform/platform.h"
#include "miniz.h"

#include <string.h>

namespace rt {
namespace unpack {

namespace {

const char* const kSources[] = {"Balatro.exe", "Balatro.love"};

struct Entry {
    std::string name;
    u32 method;
    u32 crc;
    u32 packedSize;
    u32 size;
    u32 localOffset;
};

struct State {
    bool started;
    bool finished;
    std::string source;
    std::vector<u8> data;   // the whole source file
    size_t base;            // where the archive starts in it
    std::vector<Entry> entries;
    size_t next;
    double total;           // unpacked bytes, all and so far
    double done;
    double t0;
    State() : started(false), finished(false), base(0), next(0), total(0), done(0), t0(0) {}
};

State g;

u32 read16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }
u32 read32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

// "game:/game" -> "game:", "/tmp/b360/game" -> "/tmp/b360".
std::string parentOf(const std::string& dir) {
    size_t end = dir.size();
    while (end > 1 && dir[end - 1] == '/') --end;
    size_t slash = dir.rfind('/', end - 1);
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return dir.substr(0, slash);
}

bool endsWith(const std::string& s, const char* tail) {
    size_t n = strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[s.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != tail[i]) return false;
    }
    return true;
}

// Whether a name in the archive is one to write: a file, inside the game
// folder, and not one of the PC version's libraries.
bool wanted(std::string& name) {
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '\\') name[i] = '/';
        if ((unsigned char)name[i] < 0x20 || name[i] == ':') return false;
    }
    if (name.empty() || name[0] == '/' || name[name.size() - 1] == '/') return false;
    if (name == ".." || name.compare(0, 3, "../") == 0 || name.find("/../") != std::string::npos ||
        endsWith(name, "/.."))
        return false;
    return !endsWith(name, ".dll") && !endsWith(name, ".exe");
}

Result fail(std::string& error, const std::string& why) {
    error = g.source + ": " + why;
    plat::log("[unpack] %s", error.c_str());
    std::vector<u8>().swap(g.data);
    g.entries.clear();
    g.finished = true;
    return FAILED;
}

// Finds the source and reads its archive's central directory.
Result start(std::string& error) {
    g.started = true;
    std::string root = parentOf(plat::sourceRoot());
    for (size_t i = 0; i < sizeof(kSources) / sizeof(kSources[0]) && g.source.empty(); ++i) {
        plat::PathInfo info;
        std::string path = plat::joinPath(root, kSources[i]);
        if (plat::pathInfo(path, info) && info.type == plat::PATH_FILE) g.source = path;
    }
    if (g.source.empty()) {
        g.finished = true;
        return NOTHING;
    }
    g.t0 = plat::getTime();
    if (!plat::fileRead(g.source, g.data)) return fail(error, "can't be read");

    const u8* d = g.data.empty() ? NULL : &g.data[0];
    size_t size = g.data.size();
    size_t eocd = 0;
    bool found = false;
    if (size >= 22) {
        size_t lowest = size - 22 > 65535 ? size - 22 - 65535 : 0;
        for (size_t i = size - 22 + 1; i-- > lowest;) {
            if (read32(d + i) == 0x06054b50) {
                eocd = i;
                found = true;
                break;
            }
        }
    }
    if (!found) return fail(error, "holds no game archive");
    u32 count = read16(d + eocd + 10);
    u32 dirSize = read32(d + eocd + 12);
    u32 dirOffset = read32(d + eocd + 16);
    if ((size_t)dirSize + dirOffset > eocd) return fail(error, "has a damaged game archive");
    g.base = eocd - dirSize - dirOffset;

    size_t p = g.base + dirOffset;
    size_t end = p + dirSize;
    bool hasMain = false;
    for (u32 i = 0; i < count; ++i) {
        if (p + 46 > end || read32(d + p) != 0x02014b50) return fail(error, "has a damaged game archive");
        u32 flags = read16(d + p + 8);
        Entry e;
        e.method = read16(d + p + 10);
        e.crc = read32(d + p + 16);
        e.packedSize = read32(d + p + 20);
        e.size = read32(d + p + 24);
        u32 nameLen = read16(d + p + 28);
        u32 extraLen = read16(d + p + 30);
        u32 commentLen = read16(d + p + 32);
        e.localOffset = read32(d + p + 42);
        if (p + 46 + nameLen > end) return fail(error, "has a damaged game archive");
        e.name.assign((const char*)d + p + 46, nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (!wanted(e.name)) continue;
        if (flags & 1) return fail(error, "has an encrypted file, " + e.name);
        if (e.method != 0 && e.method != 8) return fail(error, "has a file neither stored nor deflated, " + e.name);
        if (e.name == "main.lua") hasMain = true;
        g.entries.push_back(e);
        g.total += e.size;
    }
    if (!hasMain) return fail(error, "holds no main.lua; is it Balatro.exe?");
    for (size_t i = 0; i + 1 < g.entries.size(); ++i) {
        if (g.entries[i].name == "main.lua") {
            Entry last = g.entries[i];
            g.entries.erase(g.entries.begin() + i);
            g.entries.push_back(last);
            break;
        }
    }
    plat::log("[unpack] %s: %u files, %.1f MB, into %s", g.source.c_str(), (unsigned)g.entries.size(),
              g.total / (1024.0 * 1024.0), plat::sourceRoot().c_str());
    return WORKING;
}

bool unpackEntry(const Entry& e, std::string& why) {
    const u8* d = &g.data[0];
    size_t local = g.base + e.localOffset;
    if (local + 30 > g.data.size() || read32(d + local) != 0x04034b50) {
        why = "has a damaged game archive";
        return false;
    }
    size_t at = local + 30 + read16(d + local + 26) + read16(d + local + 28);
    if (at + e.packedSize > g.data.size() || (e.method == 0 && e.packedSize != e.size)) {
        why = "has a damaged game archive";
        return false;
    }
    std::vector<u8> out(e.size);
    u8* o = e.size ? &out[0] : NULL;
    if (e.method == 0) {
        if (e.size) memcpy(o, d + at, e.size);
    } else if (e.size && tinfl_decompress_mem_to_mem(o, e.size, d + at, e.packedSize, 0) != e.size) {
        why = "has a damaged file, " + e.name;
        return false;
    }
    if ((u32)mz_crc32(MZ_CRC32_INIT, o, e.size) != e.crc) {
        why = "has a damaged file, " + e.name;
        return false;
    }
    std::string path = plat::joinPath(plat::sourceRoot(), e.name);
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) plat::dirCreate(path.substr(0, slash));
    if (!plat::fileWrite(path, o, e.size, false)) {
        why = "couldn't write " + path;
        return false;
    }
    return true;
}

}  // namespace

Result step(double seconds, double& progress, std::string& error) {
    progress = 0.0;
    if (g.finished) return NOTHING;
    if (!g.started) {
        Result r = start(error);
        if (r != WORKING) return r;
    }
    double until = plat::getTime() + seconds;
    do {
        const Entry& e = g.entries[g.next];
        std::string why;
        if (!unpackEntry(e, why)) return fail(error, why);
        g.done += e.size;
        ++g.next;
    } while (g.next < g.entries.size() && plat::getTime() < until);
    progress = g.total > 0 ? g.done / g.total : 1.0;
    if (g.next < g.entries.size()) return WORKING;

    plat::log("[unpack] done in %.1f s", plat::getTime() - g.t0);
    std::vector<u8>().swap(g.data);
    g.entries.clear();
    g.finished = true;
    if (!plat::fileRemove(g.source)) plat::log("[unpack] couldn't delete %s", g.source.c_str());
    progress = 1.0;
    return DONE;
}

}  // namespace unpack
}  // namespace rt
