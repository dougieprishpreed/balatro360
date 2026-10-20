#include "font.h"

#include "stb_truetype.h"

#include <math.h>
#include <string.h>

// Glyphs are rasterised at most at this pixel size and scaled up when drawn.
// Balatro creates its fonts at 200px and draws them heavily scaled down, so
// rasterising at full size would only waste texture memory.
#ifndef B360_FONT_MAX_RASTER
#define B360_FONT_MAX_RASTER 64.0f
#endif

namespace rt {

namespace {
const int kPageSize = 1024;
struct WrapCP {
    u32 cp;
    size_t seg;
};
const int kPad = 2;
}  // namespace

u32 Font::decodeUTF8(const std::string& s, size_t& i) {
    u8 c = (u8)s[i++];
    if (c < 0x80) return c;
    int extra = 0;
    u32 cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return 0xFFFD;
    }
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((u8)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((u8)s[i++] & 0x3F);
    }
    return cp;
}

Font::Font(gfx::Device* d, const std::string& n, std::vector<u8>& data, float sz, gfx::Filter mi,
           gfx::Filter ma)
    : dev(d), name(n), info(NULL), ok(false), size(sz), scale(1), rasterScale(1), ascent(0),
      descent(0), height(0), lineHeight(1), minF(mi), magF(ma), penX(0), penY(0), rowH(0) {
    ttf.swap(data);
    if (ttf.empty()) return;
    stbtt_fontinfo* fi = new stbtt_fontinfo;
    info = fi;
    int offset = stbtt_GetFontOffsetForIndex(&ttf[0], 0);
    if (offset < 0 || !stbtt_InitFont(fi, &ttf[0], offset)) return;
    if (size < 1) size = 1;
    scale = stbtt_ScaleForMappingEmToPixels(fi, size);
    rasterScale = size > B360_FONT_MAX_RASTER ? B360_FONT_MAX_RASTER / size : 1.0f;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    // FreeType's scaled metrics: ascender rounded up, descender down,
    // height rounded.
    ascent = (int)ceil(asc * scale);
    descent = (int)floor(desc * scale);
    height = (int)floor((asc - desc + gap) * scale + 0.5f);
    ok = true;
}

Font::~Font() {
    for (size_t i = 0; i < pages.size(); ++i) dev->destroyTexture(pages[i]);
    delete (stbtt_fontinfo*)info;
}

void Font::setFilter(gfx::Filter mi, gfx::Filter ma) {
    minF = mi;
    magF = ma;
    for (size_t i = 0; i < pages.size(); ++i) dev->setTextureParams(pages[i], minF, magF, gfx::WRAP_CLAMP, gfx::WRAP_CLAMP);
}

void Font::addPage() {
    std::vector<u8> blank((size_t)kPageSize * kPageSize * 4, 0);
    // White texels with zero alpha so linear filtering at glyph edges
    // doesn't pull in dark colours.
    for (size_t i = 0; i < blank.size(); i += 4) blank[i] = blank[i + 1] = blank[i + 2] = 255;
    gfx::Texture* t = dev->createTexture(kPageSize, kPageSize, &blank[0], false);
    dev->setTextureParams(t, minF, magF, gfx::WRAP_CLAMP, gfx::WRAP_CLAMP);
    pages.push_back(t);
    penX = kPad;
    penY = kPad;
    rowH = 0;
}

const Font::Glyph& Font::glyph(u32 cp) {
    std::map<u32, Glyph>::iterator it = glyphs.find(cp);
    if (it != glyphs.end()) return it->second;

    Glyph g;
    memset(&g, 0, sizeof(g));
    g.empty = true;
    stbtt_fontinfo* fi = (stbtt_fontinfo*)info;
    if (!ok) return glyphs[cp] = g;

    u32 lookup = cp;
    if (cp == '\t') lookup = ' ';
    int index = stbtt_FindGlyphIndex(fi, (int)lookup);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(fi, index, &adv, &lsb);
    g.advance = (float)floor(adv * scale + 0.5f);
    if (cp == '\t') g.advance *= 4;

    float rs = scale * rasterScale;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(fi, index, rs, rs, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && cp != ' ' && cp != '\t') {
        if (w + 2 * kPad > kPageSize || h + 2 * kPad > kPageSize) {
            return glyphs[cp] = g;
        }
        if (pages.empty()) addPage();
        if (penX + w + kPad > kPageSize) {
            penX = kPad;
            penY += rowH + kPad;
            rowH = 0;
        }
        if (penY + h + kPad > kPageSize) addPage();
        std::vector<u8> mono((size_t)w * h);
        stbtt_MakeGlyphBitmap(fi, &mono[0], w, h, w, rs, rs, index);
        std::vector<u8> rgba((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = mono[i];
        }
        gfx::Texture* page = pages.back();
        dev->updateTexture(page, penX, penY, w, h, &rgba[0]);
        g.page = page;
        g.u0 = (float)penX / kPageSize;
        g.v0 = (float)penY / kPageSize;
        g.u1 = (float)(penX + w) / kPageSize;
        g.v1 = (float)(penY + h) / kPageSize;
        g.x0 = x0 / rasterScale;
        g.y0 = y0 / rasterScale;
        g.x1 = x1 / rasterScale;
        g.y1 = y1 / rasterScale;
        g.empty = false;
        penX += w + kPad;
        if (h > rowH) rowH = h;
    }
    return glyphs[cp] = g;
}

float Font::getKerning(u32 a, u32 b) {
    if (!ok) return 0;
    int k = stbtt_GetCodepointKernAdvance((stbtt_fontinfo*)info, (int)a, (int)b);
    return k ? (float)floor(k * scale + 0.5f) : 0.0f;
}

bool Font::hasGlyph(u32 cp) {
    if (!ok) return false;
    return stbtt_FindGlyphIndex((stbtt_fontinfo*)info, (int)cp) != 0;
}

float Font::lineWidth(const std::vector<ColoredString>& line) {
    float w = 0;
    u32 prev = 0;
    for (size_t s = 0; s < line.size(); ++s) {
        const std::string& str = line[s].text;
        size_t i = 0;
        while (i < str.size()) {
            u32 cp = decodeUTF8(str, i);
            if (cp == '\r' || cp == '\n') continue;
            if (prev) w += getKerning(prev, cp);
            w += glyph(cp).advance;
            prev = cp;
        }
    }
    return w;
}

float Font::getWidth(const std::string& text) {
    float best = 0, w = 0;
    u32 prev = 0;
    size_t i = 0;
    while (i < text.size()) {
        u32 cp = decodeUTF8(text, i);
        if (cp == '\n') {
            if (w > best) best = w;
            w = 0;
            prev = 0;
            continue;
        }
        if (cp == '\r') continue;
        if (prev) w += getKerning(prev, cp);
        w += glyph(cp).advance;
        prev = cp;
    }
    return w > best ? w : best;
}

void Font::wrap(const std::vector<ColoredString>& text, float limit,
                std::vector<std::vector<ColoredString> >& lines, std::vector<float>& widths) {
    // Flatten into (codepoint, colour index) pairs.
    std::vector<WrapCP> cps;
    for (size_t s = 0; s < text.size(); ++s) {
        size_t i = 0;
        while (i < text[s].text.size()) {
            WrapCP c;
            c.cp = decodeUTF8(text[s].text, i);
            c.seg = s;
            cps.push_back(c);
        }
    }

    std::vector<std::pair<size_t, size_t> > ranges;   // [start, end) per line
    size_t lineStart = 0;
    size_t i = 0;
    float w = 0;
    size_t lastSpace = (size_t)-1;
    u32 prev = 0;
    while (i < cps.size()) {
        u32 cp = cps[i].cp;
        if (cp == '\n') {
            ranges.push_back(std::make_pair(lineStart, i));
            lineStart = ++i;
            w = 0;
            prev = 0;
            lastSpace = (size_t)-1;
            continue;
        }
        if (cp == '\r') {
            ++i;
            continue;
        }
        float adv = glyph(cp).advance + (prev ? getKerning(prev, cp) : 0);
        if (limit > 0 && w + adv > limit && cp != ' ' && i > lineStart) {
            size_t breakAt = (lastSpace != (size_t)-1 && lastSpace > lineStart) ? lastSpace : i;
            ranges.push_back(std::make_pair(lineStart, breakAt));
            // Skip the spaces at the break.
            size_t next = breakAt;
            while (next < cps.size() && cps[next].cp == ' ') ++next;
            lineStart = next;
            i = next;
            w = 0;
            prev = 0;
            lastSpace = (size_t)-1;
            continue;
        }
        if (cp == ' ') lastSpace = i;
        w += adv;
        prev = cp;
        ++i;
    }
    ranges.push_back(std::make_pair(lineStart, cps.size()));

    for (size_t r = 0; r < ranges.size(); ++r) {
        std::vector<ColoredString> line;
        size_t end = ranges[r].second;
        // Trailing spaces don't count towards the width of wrapped lines.
        while (end > ranges[r].first && cps[end - 1].cp == ' ' && r + 1 < ranges.size()) --end;
        for (size_t k = ranges[r].first; k < end; ++k) {
            const WrapCP& c = cps[k];
            if (line.empty() || line.back().color.r != text[c.seg].color.r ||
                line.back().color.g != text[c.seg].color.g || line.back().color.b != text[c.seg].color.b ||
                line.back().color.a != text[c.seg].color.a) {
                ColoredString cs;
                cs.color = text[c.seg].color;
                line.push_back(cs);
            }
            // Re-encode as UTF-8.
            u32 v = c.cp;
            std::string& s = line.back().text;
            if (v < 0x80) {
                s += (char)v;
            } else if (v < 0x800) {
                s += (char)(0xC0 | (v >> 6));
                s += (char)(0x80 | (v & 0x3F));
            } else if (v < 0x10000) {
                s += (char)(0xE0 | (v >> 12));
                s += (char)(0x80 | ((v >> 6) & 0x3F));
                s += (char)(0x80 | (v & 0x3F));
            } else {
                s += (char)(0xF0 | (v >> 18));
                s += (char)(0x80 | ((v >> 12) & 0x3F));
                s += (char)(0x80 | ((v >> 6) & 0x3F));
                s += (char)(0x80 | (v & 0x3F));
            }
        }
        widths.push_back(lineWidth(line));
        lines.push_back(line);
    }
}

int Font::layout(const std::vector<ColoredString>& text, float limit, AlignMode align,
                 std::vector<GlyphQuad>& out, float* widthOut) {
    std::vector<std::vector<ColoredString> > lines;
    std::vector<float> widths;
    wrap(text, limit, lines, widths);
    float lineStep = (float)floor(height * lineHeight + 0.5f);
    float maxW = 0;
    for (size_t l = 0; l < lines.size(); ++l) {
        float lw = widths[l];
        if (lw > maxW) maxW = lw;
        float x = 0;
        float extraSpace = 0;
        if (limit > 0) {
            if (align == ALIGN_CENTER) x = (float)floor((limit - lw) / 2);
            else if (align == ALIGN_RIGHT) x = (float)floor(limit - lw);
            else if (align == ALIGN_JUSTIFY && l + 1 < lines.size()) {
                int spaces = 0;
                for (size_t s = 0; s < lines[l].size(); ++s)
                    for (size_t c = 0; c < lines[l][s].text.size(); ++c)
                        if (lines[l][s].text[c] == ' ') ++spaces;
                if (spaces > 0) extraSpace = (limit - lw) / spaces;
            }
        }
        float y = l * lineStep + (float)ascent;
        u32 prev = 0;
        for (size_t s = 0; s < lines[l].size(); ++s) {
            const std::string& str = lines[l][s].text;
            size_t i = 0;
            while (i < str.size()) {
                u32 cp = decodeUTF8(str, i);
                if (prev) x += getKerning(prev, cp);
                const Glyph& g = glyph(cp);
                if (!g.empty) {
                    GlyphQuad q;
                    q.x0 = x + g.x0;
                    q.y0 = y + g.y0;
                    q.x1 = x + g.x1;
                    q.y1 = y + g.y1;
                    q.u0 = g.u0;
                    q.v0 = g.v0;
                    q.u1 = g.u1;
                    q.v1 = g.v1;
                    q.page = g.page;
                    q.color = lines[l][s].color;
                    out.push_back(q);
                }
                x += g.advance;
                if (cp == ' ') x += extraSpace;
                prev = cp;
            }
        }
    }
    if (widthOut) *widthOut = maxW;
    return (int)lines.size();
}

}  // namespace rt
