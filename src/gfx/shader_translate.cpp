#include "shader_translate.h"

#include <ctype.h>
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

}  // namespace

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
