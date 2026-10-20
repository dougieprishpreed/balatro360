// Memory allocator for Lua states.
//
// Lua allocates and frees huge numbers of small blocks (strings, tables,
// closures). On the 360 those would all go through the CRT heap, which takes
// a lock on every call. Each Lua state here gets its own pool instead: blocks
// of up to 256 bytes come from per-size free lists carved out of 64 KB
// chunks, without locks (a state is only used by one thread at a time) or
// per-block headers (Lua always passes the block's size when freeing).
// Larger blocks use the CRT heap.
#include "modules.h"

#include <stdlib.h>
#include <string.h>
#include <vector>

// 64-bit LuaJIT (host builds only) insists on its own allocator.
#if defined(B360_LUAJIT) && (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__))
#define B360_POOL_ALLOC 0
#else
#define B360_POOL_ALLOC 1
#endif

namespace rt {

namespace {

class LuaPool {
public:
    enum { GRAIN = 8, MAX_SMALL = 256, CLASSES = MAX_SMALL / GRAIN, CHUNK = 64 * 1024 };

    LuaPool() : bump(NULL), bumpLeft(0) { memset(freeList, 0, sizeof(freeList)); }
    ~LuaPool() {
        for (size_t i = 0; i < chunks.size(); ++i) free(chunks[i]);
    }

    static size_t cls(size_t size) { return (size + GRAIN - 1) / GRAIN; }   // 1..CLASSES

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

    void* realloc_(void* ptr, size_t osize, size_t nsize) {
        if (nsize == 0) {
            if (ptr) {
                if (osize <= MAX_SMALL) freeSmall(ptr, cls(osize));
                else free(ptr);
            }
            return NULL;
        }
        if (!ptr) return nsize <= MAX_SMALL ? allocSmall(cls(nsize)) : malloc(nsize);
        if (osize > MAX_SMALL && nsize > MAX_SMALL) return realloc(ptr, nsize);
        if (osize <= MAX_SMALL && nsize <= MAX_SMALL && cls(osize) == cls(nsize)) return ptr;
        void* n = nsize <= MAX_SMALL ? allocSmall(cls(nsize)) : malloc(nsize);
        if (!n) return NULL;   // Lua keeps the old block
        memcpy(n, ptr, osize < nsize ? osize : nsize);
        if (osize <= MAX_SMALL) freeSmall(ptr, cls(osize));
        else free(ptr);
        return n;
    }

private:
    void* freeList[CLASSES + 1];
    char* bump;
    size_t bumpLeft;
    std::vector<void*> chunks;
};

void* poolAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    return ((LuaPool*)ud)->realloc_(ptr, osize, nsize);
}

#if B360_POOL_ALLOC
int panic(lua_State* L) {
    plat::log("PANIC: unprotected error in call to Lua API (%s)", lua_tostring(L, -1));
    return 0;
}
#endif

}  // namespace

lua_State* newRawLuaState() {
#if !B360_POOL_ALLOC
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
