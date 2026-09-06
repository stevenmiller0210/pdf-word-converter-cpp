#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal TrueType support: just enough to embed a real, subsetted font in
// the generated PDF.
//
// Why this exists at all: the previous PDF writer used a base-14 Helvetica
// with a single-byte WinAnsi encoding, so every character outside Latin-1
// (Polish, Czech, Cyrillic, Greek, and even "€" or "→") was written out as a
// literal "?". A CID font with an embedded TrueType face and Identity-H
// encoding removes that ceiling entirely, and — because we can now read the
// font's real `hmtx` advance widths — also replaces the previous guessed
// character-width table, so line wrapping stops being an approximation.

enum class FaceStyle {
    Regular,
    Bold,
    Italic,
    BoldItalic,
};

struct TrueTypeFont {
    bool ok = false;
    std::string path;
    std::string raw;      // whole file
    std::string psName;   // PostScript name, used for /BaseFont

    uint16_t unitsPerEm = 1000;
    uint16_t numGlyphs = 0;
    uint16_t numberOfHMetrics = 0;
    int16_t ascender = 800;
    int16_t descender = -200;
    int16_t capHeight = 700;
    int16_t bboxXMin = 0, bboxYMin = 0, bboxXMax = 0, bboxYMax = 0;
    double italicAngle = 0;
    int indexToLocFormat = 0;

    std::vector<uint16_t> advance;                 // per glyph id, in font units
    std::unordered_map<char32_t, uint16_t> cmap;   // code point -> glyph id
    std::vector<uint32_t> loca;                    // numGlyphs + 1 entries
    std::map<std::string, std::pair<uint32_t, uint32_t>> tables; // tag -> (offset, length)
};

// Loads and caches the four faces. Resolution order: a small list of known
// system paths, then `fc-match`. Returns nullptr if the face genuinely can't
// be found, which callers must handle rather than assume away.
const TrueTypeFont* getFace(FaceStyle style);

// Glyph id for a code point, or 0 (.notdef) when the face has no glyph.
uint16_t glyphFor(const TrueTypeFont& font, char32_t cp);

// Advance width as a fraction of the em square.
double advanceEm(const TrueTypeFont& font, uint16_t gid);

// Builds a valid TrueType file containing only the requested glyphs.
//
// Glyph ids are deliberately NOT renumbered: `loca` keeps its full
// numGlyphs+1 length with zero-length entries for dropped glyphs, and only
// `glyf` shrinks. That keeps composite glyphs (á = a + acute, and most of the
// accented letters we care about) pointing at the right components with no
// remapping, and lets the PDF use /CIDToGIDMap /Identity. The cost is a
// ~25 KB loca table, which is nothing next to the ~750 KB face it replaces.
std::string subsetFont(const TrueTypeFont& font, const std::set<uint16_t>& glyphIds);

// zlib deflate, used for every PDF stream we emit.
std::string flateCompress(const std::string& data);
