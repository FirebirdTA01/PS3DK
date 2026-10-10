// The by-value std::string_view shape that froze a game on hardware: the
// view arrives packed in one GPR (length in the high word, pointer in the
// low), and the loop over its characters must address memory through the
// pointer alone.  Before the address-model change GCC seeded the loop with a
// raw copy of the packed register (mr rX,r4; lbz 0(rX)).  This is that
// game's code shape; its own unit, called from main.c.
#include <cstdio>
#include <map>
#include <string>
#include <string_view>

namespace {
struct GlyphMetrics { float u0 = 0, v0 = 0, u1 = 0, v1 = 0, xAdvance = 0, w = 0, h = 0; };

struct FontAtlasBuilder {
    char pad[44];
    std::map<char, GlyphMetrics> glyphMap;
    char pad2[64];
    std::string textureName;
    char pad3[12];
    bool verbose = false;

    GlyphMetrics getGlyph (char character) const
    {
        auto iter = glyphMap.find (character);
        if (iter != glyphMap.end ())
            return iter->second;
        if (verbose)
            std::printf ("FontAtlasBuilder[%s]: glyph '%c' (%d) not found\n",
                         textureName.c_str (), character, (int) character);
        return GlyphMetrics ();
    }
    __attribute__ ((noipa)) float measureText (std::string_view text) const
    {
        float width = 0.0f;
        for (char c : text) {
            GlyphMetrics g = getGlyph (c);
            width += g.xAdvance;
        }
        return width;
    }
};
}

extern "C" int
class_measure (void)
{
    static FontAtlasBuilder atlas;
    for (char c : std::string_view ("Mosbrk"))
        atlas.glyphMap[c].xAdvance = 2;
    return (int) atlas.measureText ("Mossbrook");   // 9 glyphs of advance 2
}
