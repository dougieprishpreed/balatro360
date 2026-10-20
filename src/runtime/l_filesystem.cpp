// love.filesystem
#include "modules.h"
#include "filesystem.h"

#include <string.h>

namespace rt {

namespace {

const char* typeName(plat::PathType t) {
    return t == plat::PATH_DIR ? "directory" : (t == plat::PATH_FILE ? "file" : "other");
}

int w_getInfo(lua_State* L) {
    std::string path = luax_checkstring(L, 1);
    plat::PathInfo info;
    if (!fs::info(path, info)) {
        lua_pushnil(L);
        return 1;
    }
    int tableIdx = 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* filter = lua_tostring(L, 2);
        if (strcmp(filter, typeName(info.type)) != 0) {
            lua_pushnil(L);
            return 1;
        }
        if (lua_istable(L, 3)) tableIdx = 3;
    } else if (lua_istable(L, 2)) {
        tableIdx = 2;
    }
    if (tableIdx) lua_pushvalue(L, tableIdx);
    else lua_createtable(L, 0, 3);
    lua_pushstring(L, typeName(info.type));
    lua_setfield(L, -2, "type");
    if (info.type == plat::PATH_FILE) {
        lua_pushnumber(L, (lua_Number)info.size);
        lua_setfield(L, -2, "size");
    }
    lua_pushnumber(L, (lua_Number)info.modtime);
    lua_setfield(L, -2, "modtime");
    return 1;
}

int w_read(lua_State* L) {
    int arg = 1;
    // LÖVE 11.3+: read([container,] name [, size])
    if (lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TSTRING) {
        const char* c = lua_tostring(L, 1);
        if (strcmp(c, "string") == 0 || strcmp(c, "data") == 0) arg = 2;
    }
    std::string path = luax_checkstring(L, arg);
    std::vector<u8> data;
    if (!fs::read(path, data)) {
        lua_pushnil(L);
        lua_pushfstring(L, "Could not open file %s. Does not exist.", path.c_str());
        return 2;
    }
    size_t size = data.size();
    if (lua_isnumber(L, arg + 1)) {
        lua_Number n = lua_tonumber(L, arg + 1);
        if (n >= 0 && (size_t)n < size) size = (size_t)n;
    }
    lua_pushlstring(L, data.empty() ? "" : (const char*)&data[0], size);
    lua_pushnumber(L, (lua_Number)size);
    return 2;
}

int writeImpl(lua_State* L, bool append) {
    std::string path = luax_checkstring(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (lua_isnumber(L, 3)) {
        lua_Number n = lua_tonumber(L, 3);
        if (n >= 0 && (size_t)n < len) len = (size_t)n;
    }
    if (!fs::write(path, data, len, append)) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "Could not open file %s.", path.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int w_write(lua_State* L) { return writeImpl(L, false); }
int w_append(lua_State* L) { return writeImpl(L, true); }

int w_remove(lua_State* L) {
    lua_pushboolean(L, fs::remove(luax_checkstring(L, 1)));
    return 1;
}

int w_createDirectory(lua_State* L) {
    lua_pushboolean(L, fs::createDirectory(luax_checkstring(L, 1)));
    return 1;
}

int w_getDirectoryItems(lua_State* L) {
    std::vector<std::string> names;
    fs::list(luax_checkstring(L, 1), names);
    lua_createtable(L, (int)names.size(), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

int w_getSaveDirectory(lua_State* L) {
    lua_pushstring(L, fs::saveDirectory().c_str());
    return 1;
}

int w_getSourceDirectory(lua_State* L) {
    lua_pushstring(L, fs::sourceDirectory().c_str());
    return 1;
}

int w_getIdentity(lua_State* L) {
    lua_pushstring(L, "Balatro");
    return 1;
}

int w_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

int w_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

int w_load(lua_State* L) {
    std::string path = luax_checkstring(L, 1);
    int status = loadFileChunk(L, path);
    if (status != 0) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

int w_exists(lua_State* L) {
    plat::PathInfo info;
    lua_pushboolean(L, fs::info(luax_checkstring(L, 1), info));
    return 1;
}

int w_isFile(lua_State* L) {
    plat::PathInfo info;
    lua_pushboolean(L, fs::info(luax_checkstring(L, 1), info) && info.type == plat::PATH_FILE);
    return 1;
}

int w_isDirectory(lua_State* L) {
    plat::PathInfo info;
    lua_pushboolean(L, fs::info(luax_checkstring(L, 1), info) && info.type == plat::PATH_DIR);
    return 1;
}

int linesIter(lua_State* L) {
    size_t len = 0;
    const char* s = lua_tolstring(L, lua_upvalueindex(1), &len);
    size_t pos = (size_t)lua_tonumber(L, lua_upvalueindex(2));
    if (pos >= len) return 0;
    size_t end = pos;
    while (end < len && s[end] != '\n') ++end;
    size_t lineEnd = end;
    if (lineEnd > pos && s[lineEnd - 1] == '\r') --lineEnd;
    lua_pushlstring(L, s + pos, lineEnd - pos);
    lua_pushnumber(L, (lua_Number)(end + 1));
    lua_replace(L, lua_upvalueindex(2));
    return 1;
}

int w_lines(lua_State* L) {
    std::string data;
    std::string path = luax_checkstring(L, 1);
    if (!fs::read(path, data)) return luaL_error(L, "Could not open file %s.", path.c_str());
    lua_pushlstring(L, data.data(), data.size());
    lua_pushnumber(L, 0);
    lua_pushcclosure(L, linesIter, 2);
    return 1;
}

int w_getRealDirectory(lua_State* L) {
    std::string path = luax_checkstring(L, 1);
    std::string p;
    fs::normalize(path, p);
    plat::PathInfo info;
    if (plat::pathInfo(plat::joinPath(plat::saveRoot(), p), info)) lua_pushstring(L, plat::saveRoot().c_str());
    else if (plat::pathInfo(plat::joinPath(plat::sourceRoot(), p), info)) lua_pushstring(L, plat::sourceRoot().c_str());
    else lua_pushnil(L);
    return 1;
}

const luaL_Reg kFuncs[] = {
    {"getInfo", w_getInfo},
    {"read", w_read},
    {"write", w_write},
    {"append", w_append},
    {"remove", w_remove},
    {"createDirectory", w_createDirectory},
    {"getDirectoryItems", w_getDirectoryItems},
    {"getSaveDirectory", w_getSaveDirectory},
    {"getSourceBaseDirectory", w_getSourceDirectory},
    {"getSource", w_getSourceDirectory},
    {"getWorkingDirectory", w_getSourceDirectory},
    {"getUserDirectory", w_getSaveDirectory},
    {"getAppdataDirectory", w_getSaveDirectory},
    {"getIdentity", w_getIdentity},
    {"setIdentity", w_true},
    {"isFused", w_true},
    {"mount", w_false},
    {"unmount", w_false},
    {"load", w_load},
    {"lines", w_lines},
    {"exists", w_exists},
    {"isFile", w_isFile},
    {"isDirectory", w_isDirectory},
    {"getRealDirectory", w_getRealDirectory},
    {"setSymlinksEnabled", w_true},
    {"areSymlinksEnabled", w_false},
    {NULL, NULL}};

}  // namespace

int loadFileChunk(lua_State* L, const std::string& path) {
    std::string data;
    if (!fs::read(path, data)) {
        lua_pushfstring(L, "Could not open file %s. Does not exist.", path.c_str());
        return LUA_ERRFILE;
    }
    std::string chunkname = "@" + path;
    // Skip a UTF-8 BOM and a leading #! line like the stock loader does.
    size_t start = 0;
    if (data.size() >= 3 && (u8)data[0] == 0xEF && (u8)data[1] == 0xBB && (u8)data[2] == 0xBF) start = 3;
    if (data.size() > start && data[start] == '#') {
        // Keep the newline so line numbers stay correct.
        while (start < data.size() && data[start] != '\n') ++start;
    }
    return luaL_loadbuffer(L, data.data() + start, data.size() - start, chunkname.c_str());
}

void open_filesystem(lua_State* L) {
    fs::init();
    luax_registermodule(L, "filesystem", kFuncs);
}

}  // namespace rt
