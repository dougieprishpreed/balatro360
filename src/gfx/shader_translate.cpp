#include "shader_translate.h"

#include <ctype.h>
#include <map>
#include <stdlib.h>
#include <string.h>

namespace gfx {

namespace {

// --------------------------------------------------------------- tokenizer
enum TokKind { TK_WS, TK_COMMENT, TK_IDENT, TK_NUMBER, TK_PUNCT };

struct Token {
    TokKind kind;
    std::string text;
    bool preproc;   // part of a preprocessor directive line
};

bool isIdentStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

void tokenize(const std::string& s, std::vector<Token>& out) {
    size_t i = 0, n = s.size();
    bool lineStart = true;
    bool inPre = false;
    while (i < n) {
        char c = s[i];
        Token t;
        t.preproc = false;
        if (c == '\n') {
            // End of line ends a directive unless escaped.
            bool escaped = !out.empty() && out.back().text.size() && out.back().text == "\\";
            if (!escaped) inPre = false;
            t.kind = TK_WS;
            t.text = "\n";
            t.preproc = inPre;
            out.push_back(t);
            ++i;
            lineStart = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\f' || s[j] == '\v')) ++j;
            t.kind = TK_WS;
            t.text = s.substr(i, j - i);
            t.preproc = inPre;
            out.push_back(t);
            i = j;
            continue;
        }
        if (c == '#' && lineStart) inPre = true;
        lineStart = false;
        t.preproc = inPre;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            size_t j = i;
            while (j < n && s[j] != '\n') ++j;
            t.kind = TK_COMMENT;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = s.find("*/", i + 2);
            j = (j == std::string::npos) ? n : j + 2;
            t.kind = TK_COMMENT;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        if (isIdentStart(c)) {
            size_t j = i;
            while (j < n && isIdentChar(s[j])) ++j;
            t.kind = TK_IDENT;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' ||
                             ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                ++j;
            t.kind = TK_NUMBER;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        t.kind = TK_PUNCT;
        t.text = std::string(1, c);
        out.push_back(t);
        ++i;
    }
}

bool isTrivia(const Token& t) { return t.kind == TK_WS || t.kind == TK_COMMENT; }

size_t nextSig(const std::vector<Token>& toks, size_t i, size_t end) {
    while (i < end && isTrivia(toks[i])) ++i;
    return i;
}

// Index of the ')' matching the '(' at open, or end.
size_t matchParen(const std::vector<Token>& toks, size_t open, size_t end) {
    int depth = 0;
    for (size_t i = open; i < end; ++i) {
        if (toks[i].kind != TK_PUNCT) continue;
        char c = toks[i].text[0];
        if (c == '(') ++depth;
        else if (c == ')') {
            if (--depth == 0) return i;
        }
    }
    return end;
}

// ------------------------------------------------------------ type tables
struct TypeDesc {
    const char* glsl;
    const char* hlsl;
    UniformType utype;
    int components;
};

const TypeDesc kTypes[] = {
    {"float", "float", UNIFORM_FLOAT, 1},
    {"number", "float", UNIFORM_FLOAT, 1},
    {"vec2", "float2", UNIFORM_FLOAT, 2},
    {"vec3", "float3", UNIFORM_FLOAT, 3},
    {"vec4", "float4", UNIFORM_FLOAT, 4},
    {"int", "int", UNIFORM_INT, 1},
    {"ivec2", "int2", UNIFORM_INT, 2},
    {"ivec3", "int3", UNIFORM_INT, 3},
    {"ivec4", "int4", UNIFORM_INT, 4},
    {"bool", "bool", UNIFORM_BOOL, 1},
    {"bvec2", "bool2", UNIFORM_BOOL, 2},
    {"bvec3", "bool3", UNIFORM_BOOL, 3},
    {"bvec4", "bool4", UNIFORM_BOOL, 4},
    {"mat2", "float2x2", UNIFORM_MAT, 2},
    {"mat3", "float3x3", UNIFORM_MAT, 3},
    {"mat4", "float4x4", UNIFORM_MAT, 4},
    {"Image", "sampler2D", UNIFORM_SAMPLER, 1},
    {"sampler2D", "sampler2D", UNIFORM_SAMPLER, 1},
};

const TypeDesc* findType(const std::string& name) {
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i)
        if (name == kTypes[i].glsl) return &kTypes[i];
    return NULL;
}

// Identifiers that are legal in GLSL but reserved in HLSL.
const char* kHlslReserved[] = {
    "texture", "sampler", "pass", "technique", "compile", "string", "matrix", "vector",
    "line", "point", "triangle", "lineadj", "triangleadj", "linear", "centroid",
    "nointerpolation", "noperspective", "sample", "shared", "register", "packoffset",
    "column_major", "row_major", "half", "double", "dword", "pixelshader", "vertexshader",
    "snorm", "unorm", "texture1D", "texture2D_", "texture3D", "textureCUBE", "sampler1D",
    "sampler3D", "samplerCUBE", "sampler_state", "SamplerState", "Texture2D", "cbuffer",
    "tbuffer", "groupshared", "precise", "static", "extern_", "asm", "decl", "tex1D",
    "tex2D", "tex3D", "texCUBE", "mul", "lerp", "frac", "saturate", "rsqrt", "atan2",
    "fmod", "ddx", "ddy", "clip", "discard_", "position", "effect", "POSITION", "COLOR",
    NULL};

bool isHlslReserved(const std::string& s) {
    for (int i = 0; kHlslReserved[i]; ++i)
        if (s == kHlslReserved[i]) return true;
    return false;
}

// -------------------------------------------------------------- analysis
void parseUniforms(const std::vector<Token>& toks, std::vector<UniformInfo>& out) {
    int depth = 0;
    size_t n = toks.size();
    for (size_t i = 0; i < n; ++i) {
        const Token& t = toks[i];
        if (t.kind == TK_PUNCT) {
            if (t.text == "{") ++depth;
            else if (t.text == "}") --depth;
            continue;
        }
        if (depth != 0 || t.kind != TK_IDENT || t.preproc) continue;
        if (t.text != "extern" && t.text != "uniform") continue;
        // extern [qualifiers...] type name [ '[' N ']' ] (, name ...)* ;
        size_t j = nextSig(toks, i + 1, n);
        const TypeDesc* td = NULL;
        while (j < n && toks[j].kind == TK_IDENT) {
            td = findType(toks[j].text);
            if (td) break;
            j = nextSig(toks, j + 1, n);
        }
        if (!td) continue;
        j = nextSig(toks, j + 1, n);
        while (j < n && toks[j].kind == TK_IDENT) {
            UniformInfo u;
            u.name = toks[j].text;
            u.type = td->utype;
            u.components = td->components;
            u.arraySize = 1;
            j = nextSig(toks, j + 1, n);
            if (j < n && toks[j].text == "[") {
                size_t k = nextSig(toks, j + 1, n);
                if (k < n && toks[k].kind == TK_NUMBER) u.arraySize = atoi(toks[k].text.c_str());
                while (j < n && toks[j].text != "]") ++j;
                j = nextSig(toks, j + 1, n);
            }
            bool dup = false;
            for (size_t q = 0; q < out.size(); ++q)
                if (out[q].name == u.name) dup = true;
            if (!dup) out.push_back(u);
            if (j < n && toks[j].text == ",") {
                j = nextSig(toks, j + 1, n);
                continue;
            }
            break;
        }
        i = j;
    }
}

bool definesFunction(const std::vector<Token>& toks, const char* name) {
    // Looks for "vec4 <name> (" at brace depth 0.
    int depth = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.kind == TK_PUNCT) {
            if (t.text == "{") ++depth;
            else if (t.text == "}") --depth;
            continue;
        }
        if (depth != 0 || t.kind != TK_IDENT || t.text != name) continue;
        size_t j = nextSig(toks, i + 1, toks.size());
        if (j < toks.size() && toks[j].text == "(") {
            // Previous significant token must be a type (a definition).
            size_t k = i;
            while (k > 0) {
                --k;
                if (!isTrivia(toks[k])) break;
            }
            if (toks[k].kind == TK_IDENT && findType(toks[k].text)) return true;
        }
    }
    return false;
}

const char* kDefaultPosition =
    "\nvec4 position(mat4 transform_projection, vec4 vertex_position)\n"
    "{\n    return transform_projection * vertex_position;\n}\n";

const char* kDefaultEffect =
    "\nvec4 effect(vec4 vcolor, Image tex, vec2 texcoord, vec2 pixcoord)\n"
    "{\n    return Texel(tex, texcoord) * vcolor;\n}\n";

// --------------------------------------------------------------- HLSL emit
class HlslTranslator {
public:
    HlslTranslator(const std::vector<Token>& t, std::vector<std::string>* w) : toks(t), warnings(w) {
        collectMatrixNames();
    }

    std::string run() {
        std::string out;
        markGlobals();
        emitRange(0, toks.size(), out);
        return out;
    }

private:
    const std::vector<Token>& toks;
    std::vector<std::string>* warnings;
    std::vector<std::string> matNames;
    std::vector<bool> staticBefore;   // insert "static " before token i
    std::vector<bool> inUniformDecl;  // token i is part of a uniform declaration

    void warn(const std::string& s) {
        if (warnings) warnings->push_back(s);
    }

    bool isMatName(const std::string& s) const {
        for (size_t i = 0; i < matNames.size(); ++i)
            if (matNames[i] == s) return true;
        return false;
    }

    void collectMatrixNames() {
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (toks[i].kind != TK_IDENT) continue;
            if (toks[i].text != "mat2" && toks[i].text != "mat3" && toks[i].text != "mat4") continue;
            size_t j = nextSig(toks, i + 1, toks.size());
            if (j < toks.size() && toks[j].kind == TK_IDENT) matNames.push_back(toks[j].text);
        }
    }

    // Global variable declarations need "static" in HLSL (otherwise they
    // become uniforms); global "const" too.
    void markGlobals() {
        staticBefore.assign(toks.size(), false);
        inUniformDecl.assign(toks.size(), false);
        {
            int d = 0;
            for (size_t i = 0; i < toks.size(); ++i) {
                const Token& t = toks[i];
                if (t.kind == TK_PUNCT && t.text == "{") ++d;
                else if (t.kind == TK_PUNCT && t.text == "}") --d;
                if (d != 0 || t.preproc || t.kind != TK_IDENT) continue;
                if (t.text != "extern" && t.text != "uniform") continue;
                for (size_t j = i; j < toks.size() && toks[j].text != ";"; ++j) inUniformDecl[j] = true;
            }
        }
        int depth = 0;
        bool stmtStart = true;
        for (size_t i = 0; i < toks.size(); ++i) {
            const Token& t = toks[i];
            if (isTrivia(t) || t.preproc) continue;
            if (t.kind == TK_PUNCT) {
                if (t.text == "{") ++depth;
                else if (t.text == "}") {
                    --depth;
                    if (depth == 0) stmtStart = true;
                    continue;
                } else if (t.text == ";" && depth == 0) {
                    stmtStart = true;
                    continue;
                }
                stmtStart = false;
                continue;
            }
            if (depth == 0 && stmtStart && t.kind == TK_IDENT) {
                stmtStart = false;
                if (t.text == "const") {
                    staticBefore[i] = true;
                } else if (findType(t.text)) {
                    // Function definition or prototype? Look for '(' before '=' / ';'.
                    bool isFunc = false;
                    for (size_t j = i + 1; j < toks.size(); ++j) {
                        if (toks[j].kind != TK_PUNCT) continue;
                        if (toks[j].text == "(") { isFunc = true; break; }
                        if (toks[j].text == "=" || toks[j].text == ";" || toks[j].text == "{") break;
                    }
                    if (!isFunc) staticBefore[i] = true;
                }
                continue;
            }
            stmtStart = false;
        }
    }

    std::string renameIdent(const std::string& s, size_t tokIndex = (size_t)-1) const {
        const TypeDesc* td = findType(s);
        if (td && tokIndex < inUniformDecl.size() && inUniformDecl[tokIndex] &&
            (td->utype == UNIFORM_BOOL || td->utype == UNIFORM_INT)) {
            // bool/int uniforms become floats: SM3 bool/int constants are
            // awkward (and shared between stages on the 360); a float works
            // in every expression these uniforms appear in (conditions,
            // ternaries, arithmetic).
            static const char* floats[] = {"", "float", "float2", "float3", "float4"};
            return floats[td->components];
        }
        if (td) return td->hlsl;
        if (s == "extern") return "uniform";
        if (s == "position") return "love_position";
        if (s == "effect") return "love_effect";
        if (s == "texture2D" || s == "Texel") return "love_Texel";
        if (s == "mix") return "lerp";
        if (s == "fract") return "frac";
        if (s == "inversesqrt") return "rsqrt";
        if (s == "dFdx") return "ddx";
        if (s == "dFdy") return "ddy";
        if (s == "mod") return "glsl_mod";
        if (s == "gl_FragCoord") return "love_FragCoord";
        if (isHlslReserved(s)) return s + "_";
        return s;
    }

    void splitArgs(size_t open, size_t close, std::vector<std::pair<size_t, size_t> >& args) {
        int depth = 0;
        size_t start = open + 1;
        for (size_t i = open + 1; i < close; ++i) {
            if (toks[i].kind != TK_PUNCT) continue;
            char c = toks[i].text[0];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) {
                args.push_back(std::make_pair(start, i));
                start = i + 1;
            }
        }
        // An empty argument list stays empty.
        bool any = false;
        for (size_t i = start; i < close; ++i)
            if (!isTrivia(toks[i])) any = true;
        if (any || !args.empty()) args.push_back(std::make_pair(start, close));
    }

    // Parse an operand to the right of '*' for matrix multiplication:
    // identifier with member accesses, or a parenthesised expression.
    size_t operandEnd(size_t i, size_t end) {
        i = nextSig(toks, i, end);
        if (i >= end) return end;
        if (toks[i].text == "(") return matchParen(toks, i, end) + 1;
        if (toks[i].kind != TK_IDENT) return i + 1;
        size_t j = nextSig(toks, i + 1, end);
        if (j < end && toks[j].text == "(") {
            j = matchParen(toks, j, end) + 1;
        } else {
            j = i + 1;
        }
        // member access chain
        for (;;) {
            size_t k = nextSig(toks, j, end);
            if (k < end && toks[k].text == "." ) {
                size_t m = nextSig(toks, k + 1, end);
                if (m < end && toks[m].kind == TK_IDENT) { j = m + 1; continue; }
            }
            break;
        }
        return j;
    }

    void emitRange(size_t begin, size_t end, std::string& out) {
        for (size_t i = begin; i < end; ++i) {
            const Token& t = toks[i];
            if (staticBefore.size() == toks.size() && staticBefore[i]) out += "static ";
            if (t.kind != TK_IDENT) {
                out += t.text;
                continue;
            }
            size_t j = nextSig(toks, i + 1, end);
            bool call = j < end && toks[j].text == "(";
            if (call) {
                size_t close = matchParen(toks, j, end);
                if (close >= end) {
                    out += renameIdent(t.text);
                    continue;
                }
                std::vector<std::pair<size_t, size_t> > args;
                splitArgs(j, close, args);
                std::vector<std::string> targs;
                for (size_t a = 0; a < args.size(); ++a) {
                    std::string s;
                    emitRange(args[a].first, args[a].second, s);
                    targs.push_back(s);
                }
                const TypeDesc* td = findType(t.text);
                if (td && td->utype != UNIFORM_SAMPLER && args.size() == 1) {
                    // GLSL single-argument constructors broadcast scalars and
                    // truncate vectors; HLSL needs an explicit cast.
                    out += "((";
                    out += td->hlsl;
                    out += ")(";
                    out += targs[0];
                    out += "))";
                } else if (t.text == "atan" && args.size() == 2) {
                    out += "atan2(" + targs[0] + "," + targs[1] + ")";
                } else {
                    out += renameIdent(t.text);
                    out += "(";
                    for (size_t a = 0; a < targs.size(); ++a) {
                        if (a) out += ",";
                        out += targs[a];
                    }
                    out += ")";
                }
                i = close;
                continue;
            }
            if (isMatName(t.text) && j < end && toks[j].text == "*") {
                size_t opEnd = operandEnd(j + 1, end);
                std::string rhs;
                emitRange(j + 1, opEnd, rhs);
                out += "mul(" + renameIdent(t.text) + "," + rhs + ")";
                i = opEnd - 1;
                continue;
            }
            if (t.text == "precision") {
                // GLSL ES "precision highp float;" statements: drop.
                while (i < end && toks[i].text != ";") ++i;
                continue;
            }
            if (t.text == "gl_FragColor" || t.text == "love_PixelColor") {
                warn("shader writes " + t.text + " directly; unsupported");
            }
            out += renameIdent(t.text, i);
        }
    }
};

const char* kHlslPrelude =
    "#define lowp\n"
    "#define mediump\n"
    "#define highp\n"
    // GLSL mod() semantics (fmod differs for negative values). A macro
    // works for every scalar/vector combination without overloads.
    "#define glsl_mod(x, y) ((x) - (y) * floor((x) / (y)))\n"
    "uniform float4 love_ScreenSize;\n";

// ------------------------------------------------------- Vulkan GLSL emit
// Identifiers that are legal in LÖVE's GLSL but keywords, types or reserved
// words in Vulkan GLSL 4.50. They get a trailing '_' unless called.
const char* kVkReserved[] = {
    "texture", "sampler", "sample", "buffer", "shared", "coherent", "volatile", "restrict", "readonly",
    "writeonly", "layout", "centroid", "flat", "smooth", "noperspective", "patch", "subroutine", "common",
    "partition", "active", "filter", "resource", "input", "output", "image", "texture1D", "texture2D",
    "texture3D", "textureCube", "subpassInput", "precise", "superp", "demote", NULL};

bool isVkReserved(const std::string& s) {
    for (int i = 0; kVkReserved[i]; ++i)
        if (s == kVkReserved[i]) return true;
    return false;
}

// Decimal text of a non-negative int (no sprintf: the XDK build compiles this file too).
std::string intText(int v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && n < 15);
    std::string r;
    while (n > 0) r += buf[--n];
    return r;
}

std::string vkMemberName(const std::string& name) { return isVkReserved(name) ? name + "_" : name; }

// GLSL type name of a uniform as declared.
std::string glslTypeName(const UniformInfo& u) {
    static const char* f[] = {"", "float", "vec2", "vec3", "vec4"};
    static const char* i[] = {"", "int", "ivec2", "ivec3", "ivec4"};
    static const char* b[] = {"", "bool", "bvec2", "bvec3", "bvec4"};
    static const char* m[] = {"", "", "mat2", "mat3", "mat4"};
    int c = u.components < 1 ? 1 : (u.components > 4 ? 4 : u.components);
    switch (u.type) {
    case UNIFORM_INT: return i[c];
    case UNIFORM_BOOL: return b[c];
    case UNIFORM_MAT: return c < 2 ? "mat2" : m[c];
    case UNIFORM_SAMPLER: return "sampler2D";
    default: return f[c];
    }
}

// A statement of a function body, for rewriting early returns.
struct VkStmt {
    enum Kind { SIMPLE, EMPTY, PREPROC, BLOCK, IF, LOOP, DO, RETURN };
    Kind kind;
    size_t b, e;     // the whole statement, [b, e)
    size_t xb, xe;   // IF/LOOP/DO: inside the parentheses; RETURN: the expression
    bool hasElse;
    std::vector<VkStmt> kids;   // BLOCK: statements; IF: then [, else]; LOOP/DO: body
    VkStmt() : kind(SIMPLE), b(0), e(0), xb(0), xe(0), hasElse(false) {}
};

// A for loop: where it starts, where its body ends, and the ';'s around its condition.
struct VkLoop {
    size_t start, bodyEnd, condSemi1, condSemi2;
};

class VkTranslator {
public:
    VkTranslator(const std::vector<Token>& t, const LoveShaderInfo& i, bool vertex, std::vector<std::string>* w)
        : toks(t), info(i), warnings(w), vertexStage(vertex), voidFunc(false), usesAtan2(false) {
        markUniformDecls();
        markConstLocals();
        if (!vertexStage) markInnerLoops();
    }

    bool needsAtan2() const { return usesAtan2; }

    std::string run() {
        std::string out;
        size_t n = toks.size(), cursor = 0;
        int depth = 0;
        for (size_t i = 0; i < n; ++i) {
            const Token& t = toks[i];
            if (t.preproc || isTrivia(t)) continue;
            if (t.kind == TK_PUNCT) {
                if (t.text == "{") ++depth;
                else if (t.text == "}") --depth;
                continue;
            }
            if (depth != 0 || t.kind != TK_IDENT || removed[i]) continue;
            if (!findType(t.text) && t.text != "void") continue;
            // TYPE NAME ( ... ) { ... }
            size_t name = nextSig(toks, i + 1, n);
            if (name >= n || toks[name].kind != TK_IDENT) continue;
            size_t open = nextSig(toks, name + 1, n);
            if (open >= n || toks[open].text != "(") continue;
            size_t close = matchParen(toks, open, n);
            size_t brace = nextSig(toks, close + 1, n);
            if (brace >= n || toks[brace].text != "{") continue;
            size_t end = matchBrace(brace, n);
            if (end >= n) break;
            if (needsRewrite(brace, end)) {
                emitRange(cursor, brace, out);
                retType = t.text;
                voidFunc = t.text == "void";
                emitBody(brace, end, out);
                cursor = end + 1;
            }
            i = end;
        }
        emitRange(cursor, n, out);
        return out;
    }

private:
    const std::vector<Token>& toks;
    const LoveShaderInfo& info;
    std::vector<std::string>* warnings;
    std::vector<bool> removed;   // tokens of uniform declarations (they move into the block)
    std::map<size_t, std::string> insertBefore;   // text emitted before token i
    bool vertexStage;
    std::string retType;
    bool voidFunc;
    bool usesAtan2;

    void warn(const std::string& s) {
        if (warnings) warnings->push_back(s);
    }

    // xost unrolls loops with constant counts, and a nest of them can need
    // more than the 64 registers a pixel shader has (CRT's 7x7 bloom took 67,
    // the hologram's 9x9 glow 175). A loop whose count is data, a for up to
    // int() of an interpolant, is a hardware loop instead, so the innermost
    // loop of each nest gets "+ int(VaryingTexCoord.z)", which is always 0.
    void markInnerLoops() {
        std::vector<VkLoop> loops;
        size_t n = toks.size();
        for (size_t i = 0; i < n; ++i) {
            if (toks[i].kind != TK_IDENT || toks[i].preproc || toks[i].text != "for") continue;
            size_t open = nextSig(toks, i + 1, n);
            if (open >= n || toks[open].text != "(") continue;
            size_t close = matchParen(toks, open, n);
            if (close >= n) continue;
            VkLoop l;
            l.start = i;
            l.condSemi1 = l.condSemi2 = n;
            int depth = 0;
            for (size_t k = open + 1; k < close; ++k) {
                if (toks[k].kind != TK_PUNCT) continue;
                char c = toks[k].text[0];
                if (c == '(' || c == '[') ++depth;
                else if (c == ')' || c == ']') --depth;
                else if (c == ';' && depth == 0) {
                    if (l.condSemi1 == n) l.condSemi1 = k;
                    else l.condSemi2 = k;
                }
            }
            if (l.condSemi2 == n) continue;
            size_t body = nextSig(toks, close + 1, n);
            if (body >= n) continue;
            if (toks[body].text == "{") {
                int d = 0;
                size_t k = body;
                for (; k < n; ++k) {
                    if (toks[k].kind != TK_PUNCT || toks[k].preproc) continue;
                    if (toks[k].text == "{") ++d;
                    else if (toks[k].text == "}" && --d == 0) break;
                }
                l.bodyEnd = k;
            } else {
                // A single statement: possibly another loop, whose body ends it.
                l.bodyEnd = n;   // fixed below
            }
            loops.push_back(l);
        }
        // Bodies without braces end where the statement they hold ends.
        for (size_t a = loops.size(); a-- > 0;) {
            if (loops[a].bodyEnd != n) continue;
            size_t open = nextSig(toks, loops[a].start + 1, n);
            size_t body = nextSig(toks, matchParen(toks, open, n) + 1, n);
            size_t end = n;
            for (size_t b = a + 1; b < loops.size(); ++b)
                if (loops[b].start == body) end = loops[b].bodyEnd;
            if (end == n) {
                int d = 0;
                for (size_t k = body; k < n; ++k) {
                    if (toks[k].kind != TK_PUNCT) continue;
                    char c = toks[k].text[0];
                    if (c == '(' || c == '[' || c == '{') ++d;
                    else if (c == ')' || c == ']' || c == '}') --d;
                    else if (c == ';' && d == 0) {
                        end = k;
                        break;
                    }
                }
            }
            loops[a].bodyEnd = end;
        }
        for (size_t a = 0; a < loops.size(); ++a) {
            bool nested = false, hasInner = false;
            for (size_t b = 0; b < loops.size(); ++b) {
                if (a == b) continue;
                if (loops[a].start > loops[b].start && loops[a].start < loops[b].bodyEnd) nested = true;
                if (loops[b].start > loops[a].start && loops[b].start < loops[a].bodyEnd) hasInner = true;
            }
            if (!nested || hasInner) continue;
            // "i REL bound" alone; anything else stays as it is.
            size_t rel = n;
            bool simple = true;
            for (size_t k = loops[a].condSemi1 + 1; k < loops[a].condSemi2; ++k) {
                const std::string& s = toks[k].text;
                if (s == "&" || s == "|" || s == "?" || s == "!") simple = false;
                if (rel == n && (s == "<" || s == ">")) rel = k;
            }
            if (!simple || rel == n) continue;
            size_t after = rel + 1;
            if (after < n && toks[after].text == "=") ++after;
            insertBefore[after] += "(";
            insertBefore[loops[a].condSemi2] += ") + int(VaryingTexCoord.z)";
        }
    }

    // "int n = 4;" in a function, never assigned again, becomes const, so
    // glslang folds it: xost has no integer negate for a loop from -n, and
    // unrolls a loop only with constant bounds.
    void markConstLocals() {
        size_t n = toks.size();
        int depth = 0;
        for (size_t i = 0; i < n; ++i) {
            const Token& t = toks[i];
            if (t.preproc || isTrivia(t)) continue;
            if (t.kind == TK_PUNCT) {
                if (t.text == "{") ++depth;
                else if (t.text == "}") --depth;
                continue;
            }
            if (depth == 0 || t.text != "int") continue;
            size_t prev = i;
            while (prev > 0 && isTrivia(toks[prev - 1])) --prev;
            if (prev == 0) continue;
            const std::string& p = toks[prev - 1].text;
            if (p != "{" && p != ";" && p != "}") continue;
            size_t name = nextSig(toks, i + 1, n);
            size_t eq = nextSig(toks, name + 1, n);
            size_t num = nextSig(toks, eq + 1, n);
            size_t semi = nextSig(toks, num + 1, n);
            if (semi >= n || toks[name].kind != TK_IDENT || toks[eq].text != "=" || toks[num].kind != TK_NUMBER ||
                toks[semi].text != ";")
                continue;
            if (!assignedAfter(toks[name].text, semi)) insertBefore[i] += "const ";
        }
    }

    bool assignedAfter(const std::string& name, size_t from) const {
        size_t n = toks.size();
        for (size_t k = from; k < n; ++k) {
            if (toks[k].kind != TK_IDENT || toks[k].text != name || prevSigIsDot(k)) continue;
            size_t a = nextSig(toks, k + 1, n);
            size_t b = a < n ? a + 1 : n;
            if (a < n && toks[a].text == "=" && !(b < n && toks[b].text == "=")) return true;
            if (a < n && b < n && toks[b].text == "=" &&
                (toks[a].text == "+" || toks[a].text == "-" || toks[a].text == "*" || toks[a].text == "/"))
                return true;
            if (a < n && b < n && toks[a].text == toks[b].text && (toks[a].text == "+" || toks[a].text == "-"))
                return true;
            if (k >= 2 && toks[k - 1].text == toks[k - 2].text && (toks[k - 1].text == "+" || toks[k - 1].text == "-"))
                return true;
        }
        return false;
    }

    // Whether the call whose '(' is at open has two arguments.
    bool twoArgs(size_t open) const {
        size_t close = matchParen(toks, open, toks.size());
        int depth = 0, commas = 0;
        for (size_t k = open + 1; k < close; ++k) {
            if (toks[k].kind != TK_PUNCT) continue;
            char c = toks[k].text[0];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) ++commas;
        }
        return commas == 1;
    }

    void markUniformDecls() {
        removed.assign(toks.size(), false);
        int d = 0;
        for (size_t i = 0; i < toks.size(); ++i) {
            const Token& t = toks[i];
            if (t.kind == TK_PUNCT && t.text == "{") ++d;
            else if (t.kind == TK_PUNCT && t.text == "}") --d;
            if (d != 0 || t.preproc || t.kind != TK_IDENT) continue;
            if (t.text != "extern" && t.text != "uniform") continue;
            size_t j = i;
            for (; j < toks.size() && toks[j].text != ";"; ++j) removed[j] = true;
            if (j < toks.size()) removed[j] = true;
            i = j;
        }
    }

    size_t matchBrace(size_t open, size_t end) const {
        int depth = 0;
        for (size_t i = open; i < end; ++i) {
            if (toks[i].kind != TK_PUNCT || toks[i].preproc) continue;
            if (toks[i].text == "{") ++depth;
            else if (toks[i].text == "}" && --depth == 0) return i;
        }
        return end;
    }

    // The ';' ending a simple statement at i (nesting aside), or end.
    size_t findSemicolon(size_t i, size_t end) const {
        int depth = 0;
        for (; i < end; ++i) {
            const Token& t = toks[i];
            if (t.kind != TK_PUNCT || t.preproc) continue;
            char c = t.text[0];
            if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
            else if (c == ';' && depth <= 0) return i;
        }
        return end;
    }

    const UniformInfo* findUniform(const std::string& name) const {
        for (size_t i = 0; i < info.uniforms.size(); ++i)
            if (info.uniforms[i].name == name) return &info.uniforms[i];
        return NULL;
    }

    bool prevSigIsDot(size_t i) const {
        while (i > 0) {
            --i;
            if (!isTrivia(toks[i])) return toks[i].text == ".";
        }
        return false;
    }

    void emitRange(size_t b, size_t e, std::string& out) {
        for (size_t i = b; i < e && i < toks.size(); ++i) {
            const Token& t = toks[i];
            if (!insertBefore.empty()) {
                std::map<size_t, std::string>::const_iterator ins = insertBefore.find(i);
                if (ins != insertBefore.end()) out += ins->second;
            }
            if (removed[i]) {
                if (t.text == "\n") out += "\n";   // keeps line numbers
                continue;
            }
            if (t.kind != TK_IDENT || t.preproc || prevSigIsDot(i)) {
                out += t.text;
                continue;
            }
            size_t j = nextSig(toks, i + 1, toks.size());
            bool call = j < toks.size() && toks[j].text == "(";
            if (t.text == "precision") {
                // GLSL ES "precision highp float;" statements: drop.
                while (i < e && toks[i].text != ";") ++i;
                continue;
            }
            if (call && (t.text == "Texel" || t.text == "texture2D")) {
                out += "love_Texel";
                continue;
            }
            const UniformInfo* u = findUniform(t.text);
            if (u && u->type == UNIFORM_SAMPLER) {
                warn("extra texture '" + t.text + "' samples the main texture");
                out += "love_MainTex";
                continue;
            }
            if (u && u->type == UNIFORM_BOOL && u->arraySize == 1) {
                // Not bool(x): that is x != 0.0, an unordered compare xost lacks.
                if (u->components == 1) {
                    out += "(b360u_" + u->name + " > 0.5)";
                } else {
                    static const char* v[] = {"", "", "vec2", "vec3", "vec4"};
                    out += "greaterThan(b360u_" + u->name + ", " + v[u->components > 4 ? 4 : u->components] +
                           "(0.5))";
                }
                continue;
            }
            if (u && u->type == UNIFORM_INT && u->arraySize == 1) {
                out += glslTypeName(*u) + "(b360u_" + u->name + ")";
                continue;
            }
            if (call && t.text == "atan" && twoArgs(j)) {
                // xost has no atan2 (GLSL.std.450 Atan2).
                usesAtan2 = true;
                out += "b360_atan2";
                continue;
            }
            if (!call && isVkReserved(t.text)) {
                out += t.text + "_";
                continue;
            }
            if (t.text == "gl_FragColor" || t.text == "love_PixelColor")
                warn("shader writes " + t.text + " directly; unsupported");
            out += t.text;
        }
    }

    // -- early returns
    VkStmt parseStmt(size_t i, size_t end) {
        VkStmt s;
        s.b = i;
        s.xb = s.xe = i;
        const Token& t = toks[i];
        if (t.preproc) {
            size_t j = i;
            while (j < end && toks[j].preproc) ++j;
            s.kind = VkStmt::PREPROC;
            s.e = j;
            return s;
        }
        if (t.text == ";") {
            s.kind = VkStmt::EMPTY;
            s.e = i + 1;
            return s;
        }
        if (t.text == "{") {
            size_t close = matchBrace(i, end);
            s.kind = VkStmt::BLOCK;
            parseList(i + 1, close, s.kids);
            s.e = close < end ? close + 1 : end;
            return s;
        }
        if (t.text == "if" || t.text == "for" || t.text == "while") {
            size_t open = nextSig(toks, i + 1, end);
            size_t close = matchParen(toks, open, end);
            s.xb = open + 1;
            s.xe = close;
            size_t body = nextSig(toks, close + 1, end);
            if (close >= end || body >= end) {
                s.kind = VkStmt::SIMPLE;
                s.e = end;
                return s;
            }
            s.kids.push_back(parseStmt(body, end));
            s.e = s.kids[0].e;
            s.kind = t.text == "if" ? VkStmt::IF : VkStmt::LOOP;
            if (s.kind == VkStmt::IF) {
                size_t j = nextSig(toks, s.e, end);
                if (j < end && toks[j].text == "else") {
                    size_t k = nextSig(toks, j + 1, end);
                    if (k < end) {
                        s.kids.push_back(parseStmt(k, end));
                        s.hasElse = true;
                        s.e = s.kids[1].e;
                    }
                }
            }
            return s;
        }
        if (t.text == "do") {
            size_t body = nextSig(toks, i + 1, end);
            s.kids.push_back(parseStmt(body, end));
            size_t w = nextSig(toks, s.kids[0].e, end);
            size_t open = nextSig(toks, w + 1, end);
            size_t close = matchParen(toks, open, end);
            size_t semi = nextSig(toks, close + 1, end);
            s.kind = VkStmt::DO;
            s.xb = open + 1;
            s.xe = close;
            s.e = semi < end ? semi + 1 : end;
            return s;
        }
        size_t semi = findSemicolon(i, end);
        s.kind = t.text == "return" ? VkStmt::RETURN : VkStmt::SIMPLE;
        if (s.kind == VkStmt::RETURN) {
            s.xb = i + 1;
            s.xe = semi;
        }
        s.e = semi < end ? semi + 1 : end;
        return s;
    }

    void parseList(size_t b, size_t e, std::vector<VkStmt>& out) {
        size_t i = nextSig(toks, b, e);
        while (i < e) {
            out.push_back(parseStmt(i, e));
            size_t next = out.back().e;
            if (next <= i) break;
            i = nextSig(toks, next, e);
        }
    }

    static int countReturns(const VkStmt& s) {
        int n = s.kind == VkStmt::RETURN ? 1 : 0;
        for (size_t i = 0; i < s.kids.size(); ++i) n += countReturns(s.kids[i]);
        return n;
    }

    // A function needs rewriting unless its only return ends its body.
    bool needsRewrite(size_t brace, size_t end) {
        std::vector<VkStmt> body;
        parseList(brace + 1, end, body);
        int total = 0;
        for (size_t i = 0; i < body.size(); ++i) total += countReturns(body[i]);
        if (total == 0) return false;
        size_t last = body.size();
        while (last > 0 && (body[last - 1].kind == VkStmt::PREPROC || body[last - 1].kind == VkStmt::EMPTY)) --last;
        return !(total == 1 && last > 0 && body[last - 1].kind == VkStmt::RETURN);
    }

    void emitBody(size_t brace, size_t end, std::string& out) {
        std::vector<VkStmt> body;
        parseList(brace + 1, end, body);
        out += "{\n";
        if (!voidFunc) out += retType + " b360_ret;\n";
        out += "bool b360_done = false;\n";
        emitList(body, 0, out, false);
        if (!voidFunc) out += "return b360_ret;\n";
        out += "}";
    }

    // After a statement that may return, the rest of the list runs only if
    // it didn't.
    void emitList(const std::vector<VkStmt>& list, size_t from, std::string& out, bool inLoop) {
        for (size_t k = from; k < list.size(); ++k) {
            emitStmt(list[k], out, inLoop);
            if (k + 1 < list.size() && countReturns(list[k]) > 0) {
                if (inLoop) out += "if (b360_done) break;\n";
                out += "if (!b360_done) {\n";
                emitList(list, k + 1, out, inLoop);
                out += "}\n";
                return;
            }
        }
    }

    void emitBlock(const VkStmt& s, std::string& out, bool inLoop) {
        if (s.kind == VkStmt::BLOCK) {
            emitStmt(s, out, inLoop);
            return;
        }
        out += "{\n";
        emitStmt(s, out, inLoop);
        out += "}\n";
    }

    void emitLoopBody(const VkStmt& body, std::string& out) {
        out += "{\n";
        if (body.kind == VkStmt::BLOCK) emitList(body.kids, 0, out, true);
        else emitStmt(body, out, true);
        if (countReturns(body) > 0) out += "if (b360_done) break;\n";
        out += "}\n";
    }

    void emitStmt(const VkStmt& s, std::string& out, bool inLoop) {
        switch (s.kind) {
        case VkStmt::RETURN:
            if (voidFunc) {
                out += "{ b360_done = true; }\n";
            } else {
                out += "{ b360_ret = ";
                emitRange(s.xb, s.xe, out);
                out += "; b360_done = true; }\n";
            }
            break;
        case VkStmt::IF:
            out += "if (";
            emitRange(s.xb, s.xe, out);
            out += ") ";
            emitBlock(s.kids[0], out, inLoop);
            if (s.hasElse) {
                out += "else ";
                emitBlock(s.kids[1], out, inLoop);
            }
            break;
        case VkStmt::BLOCK:
            out += "{\n";
            emitList(s.kids, 0, out, inLoop);
            out += "}\n";
            break;
        case VkStmt::LOOP:
            emitRange(s.b, s.xe + 1, out);
            out += " ";
            emitLoopBody(s.kids[0], out);
            break;
        case VkStmt::DO:
            out += "do ";
            emitLoopBody(s.kids[0], out);
            out += "while (";
            emitRange(s.xb, s.xe, out);
            out += ");\n";
            break;
        default:
            emitRange(s.b, s.e, out);
            out += "\n";
            break;
        }
    }
};

}  // namespace

void vulkanUniformLayout(const LoveShaderInfo& info, std::vector<int>& offsets, int& blockBytes) {
    offsets.assign(info.uniforms.size(), -1);
    int at = VK_BUILTIN_UNIFORM_BYTES;
    for (size_t i = 0; i < info.uniforms.size(); ++i) {
        const UniformInfo& u = info.uniforms[i];
        if (u.type == UNIFORM_SAMPLER) continue;
        int n = u.arraySize < 1 ? 1 : u.arraySize;
        int size, align;
        if (u.type == UNIFORM_MAT) {
            // std140: a column per vec4.
            size = 16 * u.components * n;
            align = 16;
        } else if (n > 1) {
            size = 16 * n;   // std140 array stride
            align = 16;
        } else {
            size = 4 * u.components;
            align = u.components == 1 ? 4 : (u.components == 2 ? 8 : 16);
        }
        at = (at + align - 1) & ~(align - 1);
        offsets[i] = at;
        at += size;
    }
    blockBytes = (at + 15) & ~15;
}

std::string buildVulkanGLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage,
                                 std::vector<std::string>* warnings) {
    std::string full = src;
    full += "\n";
    if (vertexStage && !info.hasPosition) full += kDefaultPosition;
    if (!vertexStage && !info.hasEffect) full += kDefaultEffect;

    std::vector<Token> toks;
    tokenize(full, toks);
    VkTranslator tr(toks, info, vertexStage, warnings);
    std::string body = tr.run();

    std::vector<int> offsets;
    int blockBytes = 0;
    vulkanUniformLayout(info, offsets, blockBytes);

    std::string s;
    s += "#version 450\n";
    s += vertexStage ? "#define VERTEX 1\n" : "#define PIXEL 1\n";
    s += "#define lowp\n#define mediump\n#define highp\n";
    s += "#define number float\n#define Image sampler2D\n#define Texel love_Texel\n";
    s += vertexStage ? "#define love_Texel(s, uv) textureLod(s, uv, 0.0)\n" : "#define love_Texel(s, uv) texture(s, uv)\n";
    s += "layout(std140, set = 0, binding = 0) uniform LoveUniforms {\n"
         "    layout(offset = 0) mat4 TransformProjectionMatrix;\n"
         "    layout(offset = 64) vec4 love_ScreenSize;\n";
    for (size_t i = 0; i < info.uniforms.size(); ++i) {
        const UniformInfo& u = info.uniforms[i];
        if (offsets[i] < 0) continue;
        std::string type = glslTypeName(u), name = vkMemberName(u.name);
        if (u.type == UNIFORM_BOOL || u.type == UNIFORM_INT) {
            static const char* f[] = {"", "float", "vec2", "vec3", "vec4"};
            type = f[u.components < 1 ? 1 : (u.components > 4 ? 4 : u.components)];
            if (u.arraySize == 1) name = "b360u_" + u.name;
        }
        s += "    layout(offset = " + intText(offsets[i]) + ") " + type + " " + name;
        if (u.arraySize > 1) s += "[" + intText(u.arraySize) + "]";
        s += ";\n";
    }
    s += "};\n";
    if (vertexStage) {
        s += "layout(location = 0) in vec2 love_VertexPos;\n"
             "layout(location = 1) in vec4 love_VertexColor;\n"
             "layout(location = 2) in vec2 love_VertexTexCoord;\n"
             "layout(location = 0) out vec4 VaryingTexCoord;\n"
             "layout(location = 1) out vec4 VaryingColor;\n";
    } else {
        s += "layout(location = 0) in vec4 VaryingTexCoord;\n"
             "layout(location = 1) in vec4 VaryingColor;\n"
             "layout(set = 0, binding = 1) uniform sampler2D love_MainTex;\n"
             "layout(location = 0) out vec4 love_FragOut;\n";
    }
    if (tr.needsAtan2()) {
        // atan(y, x) from a polynomial for atan on [0, 1] (error below 1e-5),
        // as xost has no atan2 and its atan is only good from -1 to 1.
        s += "float b360_atan2(float y, float x) {\n"
             "    float ax = abs(x), ay = abs(y);\n"
             "    float a = min(ax, ay) / max(max(ax, ay), 1e-20);\n"
             "    float q = a * a;\n"
             "    float r = ((((0.0208351 * q - 0.0851330) * q + 0.1801410) * q - 0.3302995) * q + 0.9998660) * a;\n"
             "    r = ay > ax ? 1.5707963268 - r : r;\n"
             "    r = x < 0.0 ? 3.1415926536 - r : r;\n"
             "    return y < 0.0 ? -r : r;\n"
             "}\n";
    }
    s += "#line 1\n";
    s += body;
    s += "\n";
    if (vertexStage) {
        s += "void main() {\n"
             "    VaryingTexCoord = vec4(love_VertexTexCoord, 0.0, 1.0);\n"
             "    VaryingColor = love_VertexColor;\n"
             "    gl_Position = position(TransformProjectionMatrix, vec4(love_VertexPos, 0.0, 1.0));\n"
             "}\n";
    } else {
        // Vulkan's gl_FragCoord is in pixels from the top left, at pixel
        // centres: LÖVE's screen_coords.
        s += "void main() {\n"
             "    love_FragOut = effect(VaryingColor, love_MainTex, VaryingTexCoord.st, gl_FragCoord.xy);\n"
             "}\n";
    }
    return s;
}

void analyzeLoveShader(const std::string& src, LoveShaderInfo& info) {
    std::vector<Token> toks;
    tokenize(src, toks);
    info.hasPosition = definesFunction(toks, "position");
    info.hasEffect = definesFunction(toks, "effect");
    info.uniforms.clear();
    parseUniforms(toks, info.uniforms);
}

std::string buildGLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage) {
    std::string s;
    s += "#version 120\n";
    s += vertexStage ? "#define VERTEX 1\n" : "#define PIXEL 1\n";
    s += "#define lowp\n#define mediump\n#define highp\n";
    s += "#define number float\n#define Image sampler2D\n#define extern uniform\n";
    s += "#define Texel texture2D\n";
    s += "uniform vec4 love_ScreenSize;\n";
    s += "varying vec4 VaryingTexCoord;\nvarying vec4 VaryingColor;\n";
    if (vertexStage) {
        s += "attribute vec4 VertexPosition;\nattribute vec4 VertexTexCoord;\nattribute vec4 VertexColor;\n";
        s += "uniform mat4 TransformProjectionMatrix;\n";
    } else {
        s += "uniform sampler2D MainTex;\n";
    }
    s += "#line 1\n";
    s += src;
    s += "\n";
    if (vertexStage) {
        if (!info.hasPosition) s += kDefaultPosition;
        s += "void main() {\n"
             "    VaryingTexCoord = VertexTexCoord;\n"
             "    VaryingColor = VertexColor;\n"
             "    gl_Position = position(TransformProjectionMatrix, VertexPosition);\n"
             "}\n";
    } else {
        if (!info.hasEffect) s += kDefaultEffect;
        s += "void main() {\n"
             "    vec2 love_PixelCoord = gl_FragCoord.xy * vec2(1.0, love_ScreenSize.z) + vec2(0.0, love_ScreenSize.w);\n"
             "    gl_FragColor = effect(VaryingColor, MainTex, VaryingTexCoord.st, love_PixelCoord);\n"
             "}\n";
    }
    return s;
}

std::string buildHLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage,
                           std::vector<std::string>* warnings) {
    std::string full = src;
    full += "\n";
    if (vertexStage && !info.hasPosition) full += kDefaultPosition;
    if (!vertexStage && !info.hasEffect) full += kDefaultEffect;

    std::vector<Token> toks;
    tokenize(full, toks);
    HlslTranslator tr(toks, warnings);
    std::string body = tr.run();

    std::string s;
    s += vertexStage ? "#define VERTEX 1\n" : "#define PIXEL 1\n";
    s += kHlslPrelude;
    if (vertexStage) {
        s += "#define love_Texel(s, uv) tex2Dlod(s, float4((uv), 0.0, 0.0))\n";
        s += "uniform float4 love_ProjRows[4];\n";
    } else {
        s += "#define love_Texel(s, uv) tex2D(s, uv)\n";
        s += "sampler2D love_MainTex : register(s0);\n";
    }
    s += "#line 1\n";
    s += body;
    s += "\n";
    if (vertexStage) {
        s += "struct love_VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 tc : TEXCOORD0; };\n"
             "love_VSOut love_main(float2 inPos : POSITION, float2 inTc : TEXCOORD0, float4 inColor : COLOR0)\n"
             "{\n"
             "    love_VSOut o;\n"
             "    float4x4 tp = float4x4(love_ProjRows[0], love_ProjRows[1], love_ProjRows[2], love_ProjRows[3]);\n"
             "    o.pos = love_position(tp, float4(inPos, 0.0, 1.0));\n"
             "    o.color = inColor;\n"
             "    o.tc = inTc;\n"
             "    return o;\n"
             "}\n";
    } else {
        s += "float4 love_main(float4 color : COLOR0, float2 tc : TEXCOORD0, float2 vpos : VPOS) : COLOR0\n"
             "{\n"
             "    return love_effect(color, love_MainTex, tc, vpos + float2(0.5, 0.5));\n"
             "}\n";
    }
    return s;
}

}  // namespace gfx
