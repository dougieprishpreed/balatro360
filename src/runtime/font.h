// TrueType fonts for love.graphics (stb_truetype based glyph atlas).
#ifndef B360_FONT_H
#define B360_FONT_H

#include "object.h"
#include "../gfx/device.h"

#include <map>
#include <string>
#include <vector>

namespace rt {

struct ColorF {
    float r, g, b, a;
};

struct ColoredString {
    ColorF color;
    std::string text;
};

// One positioned glyph quad, in font pixel units relative to the text origin.
struct GlyphQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    gfx::Texture* page;
    ColorF color;
};

enum AlignMode { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY };

class Font : public Object {
public:
    // ttfData: raw .ttf contents (taken over by the font). size: pixel size.
    Font(gfx::Device* dev, const std::string& name, std::vector<u8>& ttfData, float size,
         gfx::Filter minF, gfx::Filter magF);
    ~Font();

    bool valid() const { return ok; }

    float getHeight() const { return (float)height; }
    float getAscent() const { return (float)ascent; }
    float getDescent() const { return (float)descent; }
    float getBaseline() const { return (float)ascent; }
    float getLineHeight() const { return lineHeight; }
    void setLineHeight(float h) { lineHeight = h; }
    float getWidth(const std::string& utf8);
    float getKerning(u32 a, u32 b);
    bool hasGlyph(u32 cp);

    void setFilter(gfx::Filter minF, gfx::Filter magF);
    gfx::Filter minFilter() const { return minF; }
    gfx::Filter magFilter() const { return magF; }

    // Splits text into lines no wider than limit (LÖVE getWrap semantics).
    void wrap(const std::vector<ColoredString>& text, float limit,
              std::vector<std::vector<ColoredString> >& lines, std::vector<float>& widths);

    // Lays out (possibly wrapped) coloured text. limit <= 0: no wrapping.
    // Returns the number of lines; width receives the widest line.
    int layout(const std::vector<ColoredString>& text, float limit, AlignMode align,
               std::vector<GlyphQuad>& out, float* width);

    static u32 decodeUTF8(const std::string& s, size_t& i);

private:
    struct Glyph {
        float advance;
        float x0, y0, x1, y1;   // box in font pixels relative to pen/baseline
        float u0, v0, u1, v1;
        gfx::Texture* page;
        bool empty;
    };

    const Glyph& glyph(u32 cp);
    void addPage();
    float lineWidth(const std::vector<ColoredString>& line);

    gfx::Device* dev;
    std::string name;
    std::vector<u8> ttf;
    void* info;   // stbtt_fontinfo
    bool ok;
    float size;
    float scale;        // stbtt scale for the nominal size
    float rasterScale;  // raster pixels per font pixel (<= 1 for huge fonts)
    int ascent, descent, height;
    float lineHeight;
    gfx::Filter minF, magF;

    std::map<u32, Glyph> glyphs;
    std::vector<gfx::Texture*> pages;
    int penX, penY, rowH;
};

}  // namespace rt

#endif
