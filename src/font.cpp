#include "font.h"
#include "process_util.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

// ---- big-endian readers over a std::string buffer --------------------------
// Every one is bounds-checked and returns 0 past the end rather than throwing:
// a truncated or malformed font should degrade to "no glyphs" and let the
// caller fall back, not abort a conversion mid-way.

uint8_t rdU8(const std::string& b, size_t off) {
    return off < b.size() ? static_cast<uint8_t>(b[off]) : 0;
}
uint16_t rdU16(const std::string& b, size_t off) {
    if (off + 1 >= b.size()) return 0;
    return static_cast<uint16_t>((rdU8(b, off) << 8) | rdU8(b, off + 1));
}
int16_t rdS16(const std::string& b, size_t off) {
    return static_cast<int16_t>(rdU16(b, off));
}
uint32_t rdU32(const std::string& b, size_t off) {
    if (off + 3 >= b.size()) return 0;
    return (static_cast<uint32_t>(rdU8(b, off)) << 24) |
           (static_cast<uint32_t>(rdU8(b, off + 1)) << 16) |
           (static_cast<uint32_t>(rdU8(b, off + 2)) << 8) |
           static_cast<uint32_t>(rdU8(b, off + 3));
}

void putU16(std::string& out, uint16_t v) {
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>(v & 0xFF);
}
void putU32(std::string& out, uint32_t v) {
    out += static_cast<char>((v >> 24) & 0xFF);
    out += static_cast<char>((v >> 16) & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>(v & 0xFF);
}

std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- cmap ------------------------------------------------------------------

void parseCmapFormat4(const std::string& b, size_t sub, TrueTypeFont& font) {
    uint16_t segCountX2 = rdU16(b, sub + 6);
    uint16_t segCount = segCountX2 / 2;
    size_t endCodes = sub + 14;
    size_t startCodes = endCodes + segCountX2 + 2; // +2 skips reservedPad
    size_t idDeltas = startCodes + segCountX2;
    size_t idRangeOffsets = idDeltas + segCountX2;

    for (uint16_t s = 0; s < segCount; ++s) {
        uint16_t end = rdU16(b, endCodes + s * 2);
        uint16_t start = rdU16(b, startCodes + s * 2);
        int16_t delta = rdS16(b, idDeltas + s * 2);
        uint16_t rangeOff = rdU16(b, idRangeOffsets + s * 2);
        if (start > end) continue;
        for (uint32_t c = start; c <= end; ++c) {
            uint16_t gid;
            if (rangeOff == 0) {
                gid = static_cast<uint16_t>((c + delta) & 0xFFFF);
            } else {
                size_t addr = idRangeOffsets + s * 2 + rangeOff + (c - start) * 2;
                gid = rdU16(b, addr);
                if (gid != 0) gid = static_cast<uint16_t>((gid + delta) & 0xFFFF);
            }
            if (gid != 0) font.cmap.emplace(static_cast<char32_t>(c), gid);
            if (c == 0xFFFF) break; // guard against the sentinel segment wrapping
        }
    }
}

void parseCmapFormat12(const std::string& b, size_t sub, TrueTypeFont& font) {
    uint32_t nGroups = rdU32(b, sub + 12);
    // A malformed nGroups could otherwise spin for a very long time.
    if (nGroups > 200000) nGroups = 200000;
    for (uint32_t g = 0; g < nGroups; ++g) {
        size_t rec = sub + 16 + g * 12;
        uint32_t start = rdU32(b, rec);
        uint32_t end = rdU32(b, rec + 4);
        uint32_t startGid = rdU32(b, rec + 8);
        if (start > end || end - start > 0x10FFFF) continue;
        for (uint32_t c = start; c <= end; ++c) {
            font.cmap.emplace(static_cast<char32_t>(c),
                              static_cast<uint16_t>(startGid + (c - start)));
        }
    }
}

void parseCmap(const std::string& b, size_t cmapOff, TrueTypeFont& font) {
    uint16_t numTables = rdU16(b, cmapOff + 2);
    size_t best = 0;
    int bestScore = -1;
    for (uint16_t i = 0; i < numTables; ++i) {
        size_t rec = cmapOff + 4 + i * 8;
        uint16_t plat = rdU16(b, rec);
        uint16_t enc = rdU16(b, rec + 2);
        uint32_t off = rdU32(b, rec + 4);
        // Prefer full Unicode (format 12) over the BMP-only format 4.
        int score = -1;
        if (plat == 3 && enc == 10) score = 4;
        else if (plat == 0 && enc >= 4) score = 3;
        else if (plat == 3 && enc == 1) score = 2;
        else if (plat == 0) score = 1;
        if (score > bestScore) { bestScore = score; best = cmapOff + off; }
    }
    if (bestScore < 0) return;
    uint16_t format = rdU16(b, best);
    if (format == 4) parseCmapFormat4(b, best, font);
    else if (format == 12) parseCmapFormat12(b, best, font);
}

// ---- font loading ----------------------------------------------------------

bool loadFont(const std::string& path, TrueTypeFont& font) {
    font.raw = readWholeFile(path);
    if (font.raw.size() < 12) return false;
    const std::string& b = font.raw;

    uint32_t tag = rdU32(b, 0);
    // 0x00010000 = TrueType outlines, 'true' = older Apple flavour.
    // 'OTTO' (CFF outlines) is deliberately rejected: it needs a completely
    // different embedding path (FontFile3/Type1C), and shipping a half-working
    // one would produce PDFs that some viewers silently render blank.
    if (tag != 0x00010000 && tag != 0x74727565) return false;

    uint16_t numTables = rdU16(b, 4);
    for (uint16_t i = 0; i < numTables; ++i) {
        size_t rec = 12 + i * 16;
        if (rec + 16 > b.size()) break;
        std::string name = b.substr(rec, 4);
        uint32_t off = rdU32(b, rec + 8);
        uint32_t len = rdU32(b, rec + 12);
        if (off < b.size()) font.tables[name] = {off, len};
    }

    auto table = [&](const char* name) -> size_t {
        auto it = font.tables.find(name);
        return it == font.tables.end() ? 0 : it->second.first;
    };

    size_t head = table("head");
    size_t hhea = table("hhea");
    size_t maxp = table("maxp");
    size_t hmtx = table("hmtx");
    size_t locaOff = table("loca");
    if (!head || !hhea || !maxp || !hmtx || !locaOff || !table("glyf")) return false;

    font.unitsPerEm = rdU16(b, head + 18);
    if (font.unitsPerEm == 0) font.unitsPerEm = 1000;
    font.bboxXMin = rdS16(b, head + 36);
    font.bboxYMin = rdS16(b, head + 38);
    font.bboxXMax = rdS16(b, head + 40);
    font.bboxYMax = rdS16(b, head + 42);
    font.indexToLocFormat = rdS16(b, head + 50);

    font.ascender = rdS16(b, hhea + 4);
    font.descender = rdS16(b, hhea + 6);
    font.numberOfHMetrics = rdU16(b, hhea + 34);
    font.numGlyphs = rdU16(b, maxp + 4);
    if (font.numGlyphs == 0) return false;

    if (size_t os2 = table("OS/2")) {
        int16_t cap = rdS16(b, os2 + 88); // sCapHeight, only valid for version >= 2
        if (rdU16(b, os2) >= 2 && cap > 0) font.capHeight = cap;
    }
    if (size_t post = table("post")) {
        int32_t fixed = static_cast<int32_t>(rdU32(b, post + 4));
        font.italicAngle = fixed / 65536.0;
    }

    // hmtx: numberOfHMetrics full entries, then the last advance repeats.
    font.advance.assign(font.numGlyphs, 0);
    uint16_t last = 0;
    for (uint16_t g = 0; g < font.numGlyphs; ++g) {
        if (g < font.numberOfHMetrics) last = rdU16(b, hmtx + g * 4);
        font.advance[g] = last;
    }

    font.loca.assign(static_cast<size_t>(font.numGlyphs) + 1, 0);
    for (size_t i = 0; i <= font.numGlyphs; ++i) {
        font.loca[i] = font.indexToLocFormat == 0
                           ? static_cast<uint32_t>(rdU16(b, locaOff + i * 2)) * 2
                           : rdU32(b, locaOff + i * 4);
    }

    if (size_t cmapOff = table("cmap")) parseCmap(b, cmapOff, font);
    if (font.cmap.empty()) return false;

    font.path = path;
    font.ok = true;
    return true;
}

const char* kStyleQuery[4] = {
    "DejaVu Sans",
    "DejaVu Sans:bold",
    "DejaVu Sans:italic",
    "DejaVu Sans:bold:italic",
};

const char* kStylePath[4] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
};

const char* kPsName[4] = {
    "DejaVuSans",
    "DejaVuSans-Bold",
    "DejaVuSans-Oblique",
    "DejaVuSans-BoldOblique",
};

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Resolves a face via fc-match, so the converter still works on a system that
// puts its fonts somewhere other than the Debian/Ubuntu path.
std::string resolveViaFontconfig(int idx) {
    CommandResult r = runCommand({"fc-match", "-f", "%{file}", kStyleQuery[idx]});
    if (r.exitCode != 0) return {};
    std::string p = trimmed(r.stdoutData);
    if (p.size() < 5) return {};
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // fc-match always answers with *something*; only take it if it's a TTF,
    // otherwise we'd try to embed an OTF/CFF face we can't handle.
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".ttf") != 0) return {};
    return p;
}

} // namespace

const TrueTypeFont* getFace(FaceStyle style) {
    static TrueTypeFont cache[4];
    static bool tried[4] = {false, false, false, false};

    int idx = static_cast<int>(style);
    if (idx < 0 || idx > 3) idx = 0;
    if (tried[idx]) return cache[idx].ok ? &cache[idx] : nullptr;
    tried[idx] = true;

    if (!loadFont(kStylePath[idx], cache[idx])) {
        std::string alt = resolveViaFontconfig(idx);
        if (alt.empty() || !loadFont(alt, cache[idx])) {
            cache[idx] = TrueTypeFont{};
            return nullptr;
        }
    }
    cache[idx].psName = kPsName[idx];
    return &cache[idx];
}

uint16_t glyphFor(const TrueTypeFont& font, char32_t cp) {
    auto it = font.cmap.find(cp);
    return it == font.cmap.end() ? 0 : it->second;
}

double advanceEm(const TrueTypeFont& font, uint16_t gid) {
    if (gid >= font.advance.size()) return 0.5;
    return static_cast<double>(font.advance[gid]) / static_cast<double>(font.unitsPerEm);
}

namespace {

// A composite glyph draws itself out of other glyphs; dropping those
// components would turn every accented letter into a bare base letter, which
// is precisely the content this whole change exists to preserve.
void collectComponents(const TrueTypeFont& font, uint16_t gid,
                       std::set<uint16_t>& out, int depth) {
    if (depth > 8) return; // malformed fonts can nest cyclically
    if (gid + 1u >= font.loca.size()) return;
    size_t glyfOff = font.tables.at("glyf").first;
    uint32_t start = font.loca[gid], end = font.loca[gid + 1];
    if (end <= start) return; // empty glyph (e.g. space)
    size_t p = glyfOff + start;
    if (rdS16(font.raw, p) >= 0) return; // simple glyph, no components

    p += 10; // skip numberOfContours + bounding box
    for (int guard = 0; guard < 64; ++guard) {
        uint16_t flags = rdU16(font.raw, p);
        uint16_t comp = rdU16(font.raw, p + 2);
        p += 4;
        p += (flags & 0x0001) ? 4 : 2;        // ARG_1_AND_2_ARE_WORDS
        if (flags & 0x0008) p += 2;           // WE_HAVE_A_SCALE
        else if (flags & 0x0040) p += 4;      // WE_HAVE_AN_X_AND_Y_SCALE
        else if (flags & 0x0080) p += 8;      // WE_HAVE_A_TWO_BY_TWO
        if (out.insert(comp).second) collectComponents(font, comp, out, depth + 1);
        if (!(flags & 0x0020)) break;         // MORE_COMPONENTS
    }
}

uint32_t tableChecksum(const std::string& data, size_t off, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k) {
            v <<= 8;
            if (i + k < len) v |= static_cast<uint8_t>(data[off + i + k]);
        }
        sum += v;
    }
    return sum;
}

} // namespace

std::string subsetFont(const TrueTypeFont& font, const std::set<uint16_t>& glyphIds) {
    std::set<uint16_t> keep = glyphIds;
    keep.insert(0); // .notdef must always be present
    for (uint16_t g : glyphIds) collectComponents(font, g, keep, 0);

    size_t glyfOff = font.tables.at("glyf").first;

    // Rebuild glyf with only the kept glyphs, and a full-length long loca.
    std::string glyf;
    std::vector<uint32_t> newLoca(static_cast<size_t>(font.numGlyphs) + 1, 0);
    for (uint16_t g = 0; g < font.numGlyphs; ++g) {
        newLoca[g] = static_cast<uint32_t>(glyf.size());
        if (keep.count(g) && g + 1u < font.loca.size()) {
            uint32_t s = font.loca[g], e = font.loca[g + 1];
            if (e > s && glyfOff + e <= font.raw.size()) {
                glyf.append(font.raw, glyfOff + s, e - s);
                while (glyf.size() % 4) glyf += '\0'; // keep entries 4-aligned
            }
        }
    }
    newLoca[font.numGlyphs] = static_cast<uint32_t>(glyf.size());

    std::string loca;
    loca.reserve(newLoca.size() * 4);
    for (uint32_t v : newLoca) putU32(loca, v);

    // head must advertise the long loca format we just wrote, and its
    // checkSumAdjustment is meaningless for an embedded subset.
    std::string head = font.raw.substr(font.tables.at("head").first,
                                       font.tables.at("head").second);
    if (head.size() >= 54) {
        head[8] = head[9] = head[10] = head[11] = '\0'; // checkSumAdjustment
        head[50] = '\0';
        head[51] = '\1'; // indexToLocFormat = 1 (long)
    }

    struct Out { std::string tag, data; };
    std::vector<Out> out;
    auto copyTable = [&](const char* tag) {
        auto it = font.tables.find(tag);
        if (it == font.tables.end()) return;
        if (it->second.first + it->second.second > font.raw.size()) return;
        out.push_back({tag, font.raw.substr(it->second.first, it->second.second)});
    };

    out.push_back({"head", head});
    copyTable("hhea");
    copyTable("maxp");
    copyTable("hmtx");
    copyTable("cvt ");
    copyTable("fpgm");
    copyTable("prep");
    out.push_back({"loca", loca});
    out.push_back({"glyf", glyf});

    std::sort(out.begin(), out.end(),
              [](const Out& a, const Out& b) { return a.tag < b.tag; });

    uint16_t n = static_cast<uint16_t>(out.size());
    uint16_t entrySelector = 0;
    while ((1u << (entrySelector + 1)) <= n) ++entrySelector;
    uint16_t searchRange = static_cast<uint16_t>((1u << entrySelector) * 16);
    uint16_t rangeShift = static_cast<uint16_t>(n * 16 - searchRange);

    size_t offset = 12 + static_cast<size_t>(n) * 16;
    std::string dir, body;
    for (auto& t : out) {
        while (body.size() % 4) body += '\0';
        size_t here = offset + body.size();
        dir += t.tag;
        putU32(dir, tableChecksum(t.data, 0, t.data.size()));
        putU32(dir, static_cast<uint32_t>(here));
        putU32(dir, static_cast<uint32_t>(t.data.size()));
        body += t.data;
    }

    std::string result;
    putU32(result, 0x00010000);
    putU16(result, n);
    putU16(result, searchRange);
    putU16(result, entrySelector);
    putU16(result, rangeShift);
    result += dir;
    result += body;
    return result;
}

std::string flateCompress(const std::string& data) {
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    std::string out(bound, '\0');
    int rc = compress2(reinterpret_cast<Bytef*>(&out[0]), &bound,
                       reinterpret_cast<const Bytef*>(data.data()),
                       static_cast<uLong>(data.size()), Z_BEST_COMPRESSION);
    if (rc != Z_OK) return {};
    out.resize(bound);
    return out;
}
