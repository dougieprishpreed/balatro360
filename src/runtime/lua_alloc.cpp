// Memory allocator for Lua states.
//
// Lua allocates and frees huge numbers of small blocks (strings, tables,
// closures). On the 360 those would all go through the CRT heap, which takes
// a lock on every call. Each Lua state here gets its own pool instead, without
// locks (a state is only used by one thread at a time) or per-block headers
// (Lua always passes the block's size when freeing). Blocks over 256 bytes use
// the CRT heap.
//
// Small blocks come from 64 KB slabs, each holding one size class (8 to 256
// bytes, in steps of 8), and a class always allocates from its lowest-address
// slab with room. That keeps what is alive packed into as few slabs as
// possible however long the game runs. A pool that reused freed blocks last in,
// first out from anywhere spread a session's objects over its whole peak
// footprint: Balatro walks ~600 objects of ~10 small tables each every frame,
// more than the console's 1 MB L2 holds, and frames got slower the longer the
// game ran (a second run started as slow as the first one ended).
#include "modules.h"

#include <stdlib.h>
#include <string.h>
#include <vector>

// 0: the C library's allocator; 1: the old last-in-first-out pool; 2: slabs.
// 64-bit LuaJIT (host builds only) insists on its own allocator.
#if defined(B360_LUAJIT) && (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__))
#undef B360_LUA_ALLOC
#define B360_LUA_ALLOC 0
#elif !defined(B360_LUA_ALLOC)
#define B360_LUA_ALLOC 2
#endif

namespace rt {

namespace {

enum { GRAIN = 8, MAX_SMALL = 256, CLASSES = MAX_SMALL / GRAIN };

size_t sizeClass(size_t size) { return (size + GRAIN - 1) / GRAIN; }   // 1..CLASSES

class LifoPool {
public:
    enum { CHUNK = 64 * 1024 };

    LifoPool() : bump(NULL), bumpLeft(0) { memset(freeList, 0, sizeof(freeList)); }
    ~LifoPool() {
        for (size_t i = 0; i < chunks.size(); ++i) free(chunks[i]);
    }

    void* allocSmall(size_t c) {
        void* p = freeList[c];
        if (p) {
            freeList[c] = *(void**)p;
            return p;
        }
        size_t bytes = c * GRAIN;
        if (bumpLeft < bytes) {
            // The tail of the old chunk goes to its free lists, not to waste.
            while (bumpLeft >= GRAIN) {
                size_t tc = bumpLeft / GRAIN;
                if (tc > CLASSES) tc = CLASSES;
                freeSmall(bump, tc);
                bump += tc * GRAIN;
                bumpLeft -= tc * GRAIN;
            }
            char* chunk = (char*)malloc(CHUNK);
            if (!chunk) return NULL;
            chunks.push_back(chunk);
            bump = chunk;
            bumpLeft = CHUNK;
        }
        p = bump;
        bump += bytes;
        bumpLeft -= bytes;
        return p;
    }

    void freeSmall(void* p, size_t c) {
        *(void**)p = freeList[c];
        freeList[c] = p;
    }

private:
    void* freeList[CLASSES + 1];
    char* bump;
    size_t bumpLeft;
    std::vector<void*> chunks;
};

class SlabPool {
public:
    enum { SLAB = 64 * 1024, REGION_SLABS = 16 };

    SlabPool() : empty(NULL), regionNext(NULL), regionEnd(NULL) { memset(partial, 0, sizeof(partial)); }
    ~SlabPool() {
        for (size_t i = 0; i < regions.size(); ++i) free(regions[i]);
    }

    void* allocSmall(size_t c) {
        Slab* s = partial[c];
        if (!s && !(s = takeSlab(c))) return NULL;
        void* p = s->freeList;
        if (p) {
            s->freeList = *(void**)p;
        } else {
            p = s->bump;
            s->bump += c * GRAIN;
        }
        ++s->used;
        if (!s->freeList && s->bump + c * GRAIN > (char*)s + SLAB) unlink(partial[c], s);   // full
        return p;
    }

    void freeSmall(void* p, size_t c) {
        Slab* s = (Slab*)((size_t)p & ~(size_t)(SLAB - 1));
        *(void**)p = s->freeList;
        s->freeList = p;
        --s->used;
        if (!s->listed) {
            insert(partial[c], s);   // was full
        } else if (s->used == 0 && s != partial[c]) {
            // Empty: any class may take it, lowest address first. The class
            // keeps its lowest slab, so one object coming and going doesn't
            // move a slab back and forth.
            unlink(partial[c], s);
            insert(empty, s);
        }
    }

private:
    struct Slab {
        Slab* prev;
        Slab* next;
        void* freeList;   // freed blocks
        char* bump;       // never handed out from here to the slab's end
        size_t used;      // blocks handed out
        bool listed;      // in a class's partial list or the empty list
    };
    enum { HEADER = (sizeof(Slab) + GRAIN - 1) / GRAIN * GRAIN };

    Slab* partial[CLASSES + 1];   // per class, slabs with room, by address
    Slab* empty;                  // unused slabs, by address
    char* regionNext;
    char* regionEnd;
    std::vector<void*> regions;

    // Lists stay sorted by address, so their head is the lowest slab.
    static void insert(Slab*& head, Slab* s) {
        Slab* prev = NULL;
        Slab* next = head;
        while (next && next < s) {
            prev = next;
            next = next->next;
        }
        s->prev = prev;
        s->next = next;
        if (next) next->prev = s;
        if (prev) prev->next = s;
        else head = s;
        s->listed = true;
    }

    static void unlink(Slab*& head, Slab* s) {
        if (s->prev) s->prev->next = s->next;
        else head = s->next;
        if (s->next) s->next->prev = s->prev;
        s->prev = s->next = NULL;
        s->listed = false;
    }

    Slab* takeSlab(size_t c) {
        Slab* s = empty;
        if (s) {
            unlink(empty, s);
        } else {
            if (regionNext == regionEnd) {
                // Slabs are aligned to their size, so a block's slab is its
                // address rounded down.
                char* raw = (char*)malloc((size_t)REGION_SLABS * SLAB + SLAB);
                if (!raw) return NULL;
                regions.push_back(raw);
                regionNext = (char*)(((size_t)raw + SLAB - 1) & ~(size_t)(SLAB - 1));
                regionEnd = regionNext + (size_t)REGION_SLABS * SLAB;
            }
            s = (Slab*)regionNext;
            regionNext += SLAB;
        }
        s->freeList = NULL;
        s->bump = (char*)s + HEADER;
        s->used = 0;
        insert(partial[c], s);
        return s;
    }
};

#if B360_LUA_ALLOC == 1
typedef LifoPool LuaPool;
#else
typedef SlabPool LuaPool;
#endif

void* poolAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    LuaPool* pool = (LuaPool*)ud;
    if (nsize == 0) {
        if (ptr) {
            if (osize <= MAX_SMALL) pool->freeSmall(ptr, sizeClass(osize));
            else free(ptr);
        }
        return NULL;
    }
    if (!ptr) return nsize <= MAX_SMALL ? pool->allocSmall(sizeClass(nsize)) : malloc(nsize);
    if (osize > MAX_SMALL && nsize > MAX_SMALL) return realloc(ptr, nsize);
    if (osize <= MAX_SMALL && nsize <= MAX_SMALL && sizeClass(osize) == sizeClass(nsize)) return ptr;
    void* n = nsize <= MAX_SMALL ? pool->allocSmall(sizeClass(nsize)) : malloc(nsize);
    if (!n) return NULL;   // Lua keeps the old block
    memcpy(n, ptr, osize < nsize ? osize : nsize);
    if (osize <= MAX_SMALL) pool->freeSmall(ptr, sizeClass(osize));
    else free(ptr);
    return n;
}

#if B360_LUA_ALLOC
int panic(lua_State* L) {
    plat::log("PANIC: unprotected error in call to Lua API (%s)", lua_tostring(L, -1));
    return 0;
}
#endif

}  // namespace

lua_State* newRawLuaState() {
#if !B360_LUA_ALLOC
    return luaL_newstate();
#else
    LuaPool* pool = new LuaPool;
    lua_State* L = lua_newstate(poolAlloc, pool);
    if (!L) {
        delete pool;
        return luaL_newstate();
    }
    lua_atpanic(L, panic);
    return L;
#endif
}

void closeLuaState(lua_State* L) {
    void* ud = NULL;
    lua_Alloc f = lua_getallocf(L, &ud);
    lua_close(L);
    if (f == poolAlloc) delete (LuaPool*)ud;
}

}  // namespace rt
