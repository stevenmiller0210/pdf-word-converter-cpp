#include "pdf_writer.h"
#include "font.h"
#include "image_codec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Hand-written PDF output. No PDF library, but also no base-14 shortcut:
// every page uses a real embedded TrueType face in a CID font with
// Identity-H encoding, so any code point the system font covers survives
// the conversion. Everything is laid out here — wrapping, list markers,
// table geometry, image placement, page breaks — because a PDF has no
// concept of flowing content; it only knows where things sit on a page.

namespace {

// ---------- page geometry ---------------------------------------------------

constexpr double PAGE_W = 595.0;  // A4 at 72 dpi
constexpr double PAGE_H = 842.0;
constexpr double MARGIN = 56.0;
constexpr double CELL_PAD = 4.0;
constexpr double BORDER_W = 0.6;
constexpr double LIST_INDENT = 18.0;

double contentWidth() { return PAGE_W - 2 * MARGIN; }

// ---------- UTF-8 -----------------------------------------------------------

// Decodes to code points, substituting U+FFFD for malformed bytes rather
// than dropping them silently: a visible replacement character is a much
// better bug report than text that quietly goes missing.
std::vector<char32_t> decodeUtf8(const std::string& s) {
    std::vector<char32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }

        if (i + static_cast<size_t>(extra) >= s.size()) { out.push_back(0xFFFD); break; }
        bool bad = false;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::string encodeUtf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// ---------- number formatting ----------------------------------------------

// PDF wants plain decimals: no exponent, no locale separator, and "-0" is
// legal but ugly. Trimming trailing zeros keeps content streams small.
std::string num(double v) {
    if (std::fabs(v) < 0.0005) v = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.empty() || s == "-") s = "0";
    return s;
}

std::string num(int v) { return std::to_string(v); }
std::string num(long v) { return std::to_string(v); }

// ---------- faces -----------------------------------------------------------

int faceIndex(FaceStyle f) { return static_cast<int>(f); }

FaceStyle faceFor(bool bold, bool italic) {
    if (bold && italic) return FaceStyle::BoldItalic;
    if (bold) return FaceStyle::Bold;
    if (italic) return FaceStyle::Italic;
    return FaceStyle::Regular;
}

// Every face resolves through here so a missing Bold-Italic degrades to
// Bold rather than taking the document down with it.
struct FaceTable {
    const TrueTypeFont* face[4] = {nullptr, nullptr, nullptr, nullptr};
    FaceStyle resolved[4] = {FaceStyle::Regular, FaceStyle::Regular,
                             FaceStyle::Regular, FaceStyle::Regular};
    std::set<uint16_t> used[4];

    void load() {
        for (int i = 0; i < 4; ++i) face[i] = getFace(static_cast<FaceStyle>(i));
        if (!face[0]) {
            for (int i = 1; i < 4; ++i)
                if (face[i]) { face[0] = face[i]; break; }
        }
        if (!face[0])
            throw std::runtime_error(
                "Nem talalhato hasznalhato TrueType betutipus a rendszeren "
                "(telepitsd a DejaVu fontokat: fonts-dejavu).");

        static const int fallbackChain[4][3] = {
            {0, 0, 0},  // Regular
            {0, 0, 0},  // Bold -> Regular
            {0, 0, 0},  // Italic -> Regular
            {1, 2, 0},  // BoldItalic -> Bold -> Italic -> Regular
        };
        for (int i = 0; i < 4; ++i) {
            if (face[i]) { resolved[i] = static_cast<FaceStyle>(i); continue; }
            int pick = 0;
            for (int k = 0; k < 3; ++k) {
                int cand = fallbackChain[i][k];
                if (face[cand]) { pick = cand; break; }
            }
            resolved[i] = static_cast<FaceStyle>(pick);
            face[i] = face[pick];
        }
    }

    FaceStyle canonical(FaceStyle f) const { return resolved[faceIndex(f)]; }
    const TrueTypeFont& font(FaceStyle f) const { return *face[faceIndex(canonical(f))]; }
    void markUsed(FaceStyle f, uint16_t gid) { used[faceIndex(canonical(f))].insert(gid); }
    bool isUsed(int i) const { return !used[i].empty(); }
};

// ---------- laid-out output -------------------------------------------------

struct PlacedText {
    double x = 0, y = 0;
    double size = 0;
    FaceStyle face = FaceStyle::Regular;
    std::string text;
    std::string color;
};

struct PlacedRule {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct PlacedFill {
    double x = 0, y = 0, w = 0, h = 0;
    double gray = 0.93;
};

struct PlacedImage {
    size_t imageIndex = 0;
    double x = 0, y = 0, w = 0, h = 0;
};

struct PageOut {
    std::vector<PlacedText> texts;
    std::vector<PlacedRule> rules;
    std::vector<PlacedFill> fills;
    std::vector<PlacedImage> images;
};

// ---------- line model ------------------------------------------------------

struct Piece {
    FaceStyle face = FaceStyle::Regular;
    double size = 11;
    double dx = 0; // offset from the line's left edge
    std::string text;
    std::string color; // "RRGGBB", empty for black
};

struct Line {
    std::vector<Piece> pieces;
    double height = 0;   // full line box, including leading
    double ascent = 0;   // baseline offset from the top of the box
};

// A paragraph reduced to lines, plus the vertical space around it.
struct LaidOutBlock {
    std::vector<Line> lines;
    double spaceBefore = 0;
    double spaceAfter = 0;
};

struct StyleMetrics {
    double size;
    bool bold;
    double spaceBefore;
    double spaceAfter;
    double leading; // multiple of font size
};

StyleMetrics metricsFor(ParagraphStyle style) {
    switch (style) {
        case ParagraphStyle::Title:    return {24, true, 6, 14, 1.25};
        case ParagraphStyle::Heading1: return {18, true, 14, 7, 1.28};
        case ParagraphStyle::Heading2: return {15, true, 12, 6, 1.30};
        case ParagraphStyle::Heading3: return {13, true, 10, 5, 1.32};
        case ParagraphStyle::Normal:
        default:                       return {11, false, 0, 7, 1.42};
    }
}

// ---------- layout engine ---------------------------------------------------

class Builder {
public:
    explicit Builder(FaceTable& faces) : faces_(faces) {}

    double measure(FaceStyle f, double size, const std::string& utf8) {
        const TrueTypeFont& font = faces_.font(f);
        double w = 0;
        for (char32_t cp : decodeUtf8(utf8)) w += advanceEm(font, glyphFor(font, cp)) * size;
        return w;
    }

    // Greedy word wrap over the paragraph's runs. Runs are tokenised
    // independently but wrap as one stream, so a bold word in the middle of
    // a sentence does not force a line break around itself.
    std::vector<Line> layoutParagraph(const Paragraph& p, double width, double baseSize,
                                      bool forceBold) {
        struct Token {
            FaceStyle face;
            std::string text;
            double width;
            bool spaceBefore;
            std::string color;
        };
        std::vector<Token> tokens;

        // The pending-space flag lives outside the run loop on purpose: a run
        // that ends with a space is followed by one that starts a new word,
        // and tracking this per-run silently glued "Ez " + "bold" into
        // "Ezbold".
        bool pendingSpace = false;

        for (const auto& run : p.runs) {
            FaceStyle f = faceFor(run.bold || forceBold, run.italic);
            auto cps = decodeUtf8(run.text);
            std::string word;
            auto flush = [&]() {
                if (word.empty()) return;
                tokens.push_back({f, word, measure(f, baseSize, word), pendingSpace, run.color});
                word.clear();
                pendingSpace = false;
            };
            for (char32_t cp : cps) {
                if (cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0x00A0) {
                    flush();
                    pendingSpace = true;
                } else {
                    word += encodeUtf8(cp);
                }
            }
            flush();
        }

        std::vector<Line> lines;
        double spaceW = measure(FaceStyle::Regular, baseSize, " ");
        double leading = metricsLeading_;

        std::vector<Piece> cur;
        double curW = 0;
        auto endLine = [&]() {
            Line ln;
            ln.pieces = cur;
            ln.height = baseSize * leading;
            ln.ascent = baseSize * 0.80;
            lines.push_back(std::move(ln));
            cur.clear();
            curW = 0;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            Token t = tokens[i];
            double lead = (t.spaceBefore && !cur.empty()) ? spaceW : 0;

            // A single token wider than the column can never fit; break it
            // by code point instead of letting it run off the page.
            if (t.width > width) {
                if (!cur.empty()) endLine();
                const TrueTypeFont& font = faces_.font(t.face);
                std::string chunk;
                double chunkW = 0;
                for (char32_t cp : decodeUtf8(t.text)) {
                    double cw = advanceEm(font, glyphFor(font, cp)) * baseSize;
                    if (chunkW + cw > width && !chunk.empty()) {
                        cur.push_back({t.face, baseSize, 0, chunk, t.color});
                        endLine();
                        chunk.clear();
                        chunkW = 0;
                    }
                    chunk += encodeUtf8(cp);
                    chunkW += cw;
                }
                if (!chunk.empty()) {
                    cur.push_back({t.face, baseSize, 0, chunk, t.color});
                    curW = chunkW;
                }
                continue;
            }

            if (!cur.empty() && curW + lead + t.width > width) {
                endLine();
                lead = 0;
            }
            std::string text = (lead > 0 ? std::string(" ") : std::string()) + t.text;
            double w = lead + t.width;

            if (!cur.empty() && cur.back().face == t.face && cur.back().color == t.color) {
                cur.back().text += text;
            } else {
                cur.push_back({t.face, baseSize, curW, text, t.color});
            }
            curW += w;
        }
        if (!cur.empty()) endLine();
        if (lines.empty()) {
            Line ln;
            ln.height = baseSize * leading;
            ln.ascent = baseSize * 0.80;
            lines.push_back(std::move(ln));
        }

        // Recompute piece offsets: merging changed where each one starts.
        for (auto& ln : lines) {
            double x = 0;
            for (auto& pc : ln.pieces) {
                pc.dx = x;
                x += measure(pc.face, pc.size, pc.text);
            }
        }
        for (const auto& ln : lines)
            for (const auto& pc : ln.pieces) registerGlyphs(pc.face, pc.text);
        return lines;
    }

    void setLeading(double l) { metricsLeading_ = l; }

    void registerGlyphs(FaceStyle f, const std::string& utf8) {
        const TrueTypeFont& font = faces_.font(f);
        for (char32_t cp : decodeUtf8(utf8)) {
            uint16_t gid = glyphFor(font, cp);
            faces_.markUsed(f, gid);
            glyphText_[faceIndex(faces_.canonical(f))][gid] = cp;
        }
    }

    const std::map<uint16_t, char32_t>& glyphText(int faceIdx) const {
        return glyphText_[faceIdx];
    }

private:
    FaceTable& faces_;
    double metricsLeading_ = 1.42;
    std::map<uint16_t, char32_t> glyphText_[4];
};

// A list marker, rendered in the paragraph's own size so nesting reads
// consistently. Level cycles through the three usual bullet shapes.
std::string bulletFor(const ListInfo& list) {
    if (list.kind == ListKind::Numbered)
        return std::to_string(list.ordinal > 0 ? list.ordinal : 1) + ".";
    static const char* kBullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"}; // • ◦ ▪
    return kBullets[std::min(std::max(list.level, 0), 2)];
}

// ---------- pager -----------------------------------------------------------

class Pager {
public:
    Pager() { pages_.emplace_back(); }

    double remaining() const { return y_ - MARGIN; }
    double cursor() const { return y_; }
    bool atPageTop() const { return std::fabs(y_ - (PAGE_H - MARGIN)) < 0.01; }

    void newPage() {
        pages_.emplace_back();
        y_ = PAGE_H - MARGIN;
    }

    void advance(double dy) { y_ -= dy; }

    // Reserves vertical space, starting a new page if the block will not
    // fit. Returns the top y of the reserved area.
    double reserve(double height) {
        if (height > remaining() && !atPageTop()) newPage();
        double top = y_;
        y_ -= height;
        return top;
    }

    void putLine(const Line& line, double x, double topY) {
        double baseline = topY - line.ascent;
        for (const auto& pc : line.pieces) {
            if (pc.text.empty()) continue;
            PlacedText t;
            t.x = x + pc.dx;
            t.y = baseline;
            t.size = pc.size;
            t.face = pc.face;
            t.text = pc.text;
            t.color = pc.color;
            pages_.back().texts.push_back(std::move(t));
        }
    }

    void putText(const std::string& text, FaceStyle face, double size, double x, double baseline) {
        pages_.back().texts.push_back({x, baseline, size, face, text, {}});
    }

    void rule(double x1, double y1, double x2, double y2) {
        pages_.back().rules.push_back({x1, y1, x2, y2});
    }

    void fill(double x, double y, double w, double h, double gray) {
        pages_.back().fills.push_back({x, y, w, h, gray});
    }

    void image(size_t idx, double x, double y, double w, double h) {
        pages_.back().images.push_back({idx, x, y, w, h});
    }

    std::vector<PageOut>& pages() { return pages_; }

private:
    std::vector<PageOut> pages_;
    double y_ = PAGE_H - MARGIN;
};

// ---------- document rendering ---------------------------------------------

struct ImageSlot {
    EncodedImage enc;
    double widthPt = 0;
    double heightPt = 0;
};

class Renderer {
public:
    Renderer(FaceTable& faces, Builder& builder) : faces_(faces), builder_(builder) {}

    void render(const DocModel& doc) {
        renderBlocks(doc.blocks, MARGIN, contentWidth(), true);
    }

    Pager& pager() { return pager_; }
    std::vector<ImageSlot>& images() { return images_; }

private:
    // Lays out one run of blocks inside a column. `allowBreaks` is false
    // inside table cells, where the caller has already reserved the space.
    void renderBlocks(const std::vector<Block>& blocks, double x, double width, bool allowBreaks) {
        for (const auto& b : blocks) {
            switch (b.kind) {
                case Block::Kind::Paragraph: renderParagraph(b.paragraph, x, width, allowBreaks); break;
                case Block::Kind::Table:     if (b.table) renderTable(*b.table, x, width); break;
                case Block::Kind::Image:     if (b.image) renderImage(*b.image, x, width); break;
            }
        }
    }

    void renderParagraph(const Paragraph& p, double x, double width, bool allowBreaks) {
        StyleMetrics m = metricsFor(p.style);
        builder_.setLeading(m.leading);

        double indent = 0;
        std::string marker;
        if (p.list.kind != ListKind::None) {
            indent = LIST_INDENT * (p.list.level + 1);
            marker = bulletFor(p.list);
            builder_.registerGlyphs(FaceStyle::Regular, marker);
        }

        double textWidth = std::max(40.0, width - indent);
        auto lines = builder_.layoutParagraph(p, textWidth, m.size, m.bold);

        if (allowBreaks && m.spaceBefore > 0 && !pager_.atPageTop()) pager_.advance(m.spaceBefore);

        for (size_t i = 0; i < lines.size(); ++i) {
            double top;
            if (allowBreaks) {
                top = pager_.reserve(lines[i].height);
            } else {
                top = pager_.cursor();
                pager_.advance(lines[i].height);
            }
            if (i == 0 && !marker.empty()) {
                // The marker sits in the indent gutter, on the first line's
                // baseline, so wrapped lines stay flush with the text.
                double markerW = builder_.measure(FaceStyle::Regular, m.size, marker);
                double gutter = std::max(0.0, LIST_INDENT - 4.0);
                pager_.putText(marker, FaceStyle::Regular, m.size,
                               x + indent - std::min(markerW + 4.0, gutter + 4.0),
                               top - lines[i].ascent);
            }
            pager_.putLine(lines[i], x + indent, top);
        }
        if (allowBreaks) pager_.advance(m.spaceAfter);
        else pager_.advance(m.spaceAfter * 0.5);
    }

    void renderImage(const Image& img, double x, double width) {
        EncodedImage enc;
        // An unsupported picture is skipped, not fatal: losing one image is
        // a far better outcome than refusing to convert the document.
        if (!encodeImageForPdf(img, enc)) return;

        double pw = img.displayWidthPt > 0 ? img.displayWidthPt : enc.width * 0.75;
        double ph = img.displayHeightPt > 0 ? img.displayHeightPt : enc.height * 0.75;
        if (pw <= 0 || ph <= 0) return;

        double maxH = PAGE_H - 2 * MARGIN;
        if (pw > width) { ph *= width / pw; pw = width; }
        if (ph > maxH) { pw *= maxH / ph; ph = maxH; }

        double top = pager_.reserve(ph + 6);
        // While measuring a table cell the picture must not be registered:
        // the same block is rendered again for real straight afterwards, and
        // a second copy would be embedded in the file for nothing.
        if (measuring_) return;
        size_t idx = images_.size();
        images_.push_back({enc, pw, ph});
        pager_.image(idx, x, top - ph, pw, ph);
    }

    // ---- tables ----
    //
    // Column widths come from the table's own grid when it has one, and are
    // otherwise split evenly. Rows are measured before being placed so a row
    // never straddles a page boundary — a half-drawn row with its borders cut
    // in two is the single ugliest thing this writer could emit.
    void renderTable(const Table& table, double x, double width) {
        size_t cols = 0;
        for (const auto& row : table.rows) {
            size_t n = 0;
            for (const auto& c : row.cells) n += std::max(1, c.gridSpan);
            cols = std::max(cols, n);
        }
        if (cols == 0) return;

        std::vector<double> colW(cols, width / static_cast<double>(cols));
        if (table.columnWidths.size() == cols) {
            double total = 0;
            for (double w : table.columnWidths) total += w;
            if (total > 0)
                for (size_t i = 0; i < cols; ++i) colW[i] = width * table.columnWidths[i] / total;
        }

        pager_.advance(4);

        for (const auto& row : table.rows) {
            std::vector<double> cellHeights;
            double rowH = 0;
            size_t col = 0;
            for (const auto& cell : row.cells) {
                double cw = spanWidth(colW, col, cell.gridSpan);
                double h = measureCell(cell, cw, row.isHeader);
                cellHeights.push_back(h);
                rowH = std::max(rowH, h);
                col += std::max(1, cell.gridSpan);
            }
            rowH = std::max(rowH, 16.0);

            // A row taller than a whole page cannot be made to fit; placing
            // it anyway (and letting it overflow) beats dropping the content.
            double top = pager_.reserve(rowH);

            if (row.isHeader) pager_.fill(x, top - rowH, width, rowH, 0.92);

            col = 0;
            double cx = x;
            for (const auto& cell : row.cells) {
                double cw = spanWidth(colW, col, cell.gridSpan);
                drawCell(cell, cx, cw, top, rowH, row.isHeader);
                cx += cw;
                col += std::max(1, cell.gridSpan);
            }

            // Borders
            pager_.rule(x, top, x + width, top);
            pager_.rule(x, top - rowH, x + width, top - rowH);
            cx = x;
            pager_.rule(cx, top, cx, top - rowH);
            col = 0;
            for (const auto& cell : row.cells) {
                cx += spanWidth(colW, col, cell.gridSpan);
                col += std::max(1, cell.gridSpan);
                pager_.rule(cx, top, cx, top - rowH);
            }
        }
        pager_.advance(8);
    }

    double spanWidth(const std::vector<double>& colW, size_t start, int span) const {
        double w = 0;
        int n = std::max(1, span);
        for (int i = 0; i < n && start + static_cast<size_t>(i) < colW.size(); ++i)
            w += colW[start + i];
        return w > 0 ? w : (colW.empty() ? 0 : colW[0]);
    }

    // Measures a cell by laying it out into a throwaway pager and reading how
    // far the cursor moved — the same code path that will actually draw it,
    // so the measurement can't drift from the result.
    double measureCell(const TableCell& cell, double cw, bool header) {
        Pager scratch;
        std::swap(scratch, pager_);
        bool wasMeasuring = measuring_;
        measuring_ = true;
        double start = pager_.cursor();
        renderCellContent(cell, MARGIN, cw - 2 * CELL_PAD, header);
        double used = start - pager_.cursor();
        measuring_ = wasMeasuring;
        std::swap(scratch, pager_);
        return used + 2 * CELL_PAD;
    }

    void drawCell(const TableCell& cell, double cx, double cw, double top, double rowH,
                  bool header) {
        (void)rowH;
        double saved = pager_.cursor();
        pager_.advance(saved - (top - CELL_PAD)); // move the cursor to the cell's top
        renderCellContent(cell, cx + CELL_PAD, cw - 2 * CELL_PAD, header);
        pager_.advance(pager_.cursor() - saved); // restore for the row's own accounting
    }

    void renderCellContent(const TableCell& cell, double x, double w, bool header) {
        if (w < 8) w = 8;
        for (const auto& b : cell.blocks) {
            if (b.kind == Block::Kind::Paragraph) {
                Paragraph p = b.paragraph;
                if (header)
                    for (auto& r : p.runs) r.bold = true;
                renderParagraph(p, x, w, false);
            } else if (b.kind == Block::Kind::Image && b.image) {
                // Nested pictures are laid out, but never allowed to force a
                // page break from inside a cell.
                renderImage(*b.image, x, w);
            } else if (b.kind == Block::Kind::Table && b.table) {
                renderTable(*b.table, x, w);
            }
        }
    }

    FaceTable& faces_;
    Builder& builder_;
    Pager pager_;
    std::vector<ImageSlot> images_;
    bool measuring_ = false;
};

// ---------- PDF object emission --------------------------------------------

std::string hex4(unsigned v) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s += d[(v >> 12) & 0xF];
    s += d[(v >> 8) & 0xF];
    s += d[(v >> 4) & 0xF];
    s += d[v & 0xF];
    return s;
}

// "RRGGBB" -> the three 0..1 components PDF's `rg` operator wants.
bool parseHexColor(const std::string& hex, double& r, double& g, double& b) {
    if (hex.size() != 6) return false;
    auto comp = [&](size_t off) {
        return static_cast<double>(std::stoi(hex.substr(off, 2), nullptr, 16)) / 255.0;
    };
    try {
        r = comp(0);
        g = comp(2);
        b = comp(4);
    } catch (...) {
        return false;
    }
    return true;
}

std::string escapeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+') out += c;
    }
    return out.empty() ? std::string("Embedded") : out;
}

// Maps glyph ids back to text so the PDF stays selectable and searchable.
// Without this a reader can render the page perfectly and still copy out
// nothing but mojibake, because Identity-H glyph ids carry no meaning.
std::string buildToUnicode(const std::map<uint16_t, char32_t>& glyphs) {
    std::ostringstream cmap;
    cmap << "/CIDInit /ProcSet findresource begin\n"
            "12 dict begin\nbegincmap\n/CIDSystemInfo\n"
            "<< /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";

    std::vector<std::pair<uint16_t, char32_t>> items(glyphs.begin(), glyphs.end());
    for (size_t i = 0; i < items.size(); i += 100) {
        size_t n = std::min<size_t>(100, items.size() - i);
        cmap << n << " beginbfchar\n";
        for (size_t k = 0; k < n; ++k) {
            char32_t cp = items[i + k].second;
            cmap << "<" << hex4(items[i + k].first) << "> <";
            if (cp > 0xFFFF) {
                // Outside the BMP the destination must be a UTF-16 surrogate
                // pair, not a truncated code point.
                char32_t v = cp - 0x10000;
                cmap << hex4(static_cast<unsigned>(0xD800 + (v >> 10)))
                     << hex4(static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
            } else {
                cmap << hex4(static_cast<unsigned>(cp));
            }
            cmap << ">\n";
        }
        cmap << "endbfchar\n";
    }
    cmap << "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return cmap.str();
}

// PDF's /W array is run-length-ish: consecutive ids share one bracketed list.
std::string buildWidthArray(const TrueTypeFont& font, const std::set<uint16_t>& gids) {
    std::string out = "[";
    std::vector<uint16_t> ids(gids.begin(), gids.end());
    size_t i = 0;
    while (i < ids.size()) {
        size_t j = i;
        while (j + 1 < ids.size() && ids[j + 1] == ids[j] + 1) ++j;
        out += " " + num(static_cast<int>(ids[i])) + " [";
        for (size_t k = i; k <= j; ++k) {
            out += " " + num(std::lround(advanceEm(font, ids[k]) * 1000.0));
        }
        out += " ]";
        i = j + 1;
    }
    out += " ]";
    return out;
}

std::string streamObject(const std::string& extraDict, const std::string& payload,
                         bool compress) {
    std::string data = compress ? flateCompress(payload) : payload;
    bool flated = compress && !data.empty();
    if (!flated) data = payload;
    std::string out = "<< " + extraDict;
    if (flated) out += " /Filter /FlateDecode";
    out += " /Length " + num(static_cast<int>(data.size())) + " >>\nstream\n";
    out += data;
    out += "\nendstream";
    return out;
}

} // namespace

void writePdf(const std::string& path, const DocModel& doc) {
    FaceTable faces;
    faces.load();

    Builder builder(faces);
    Renderer renderer(faces, builder);
    renderer.render(doc);

    std::vector<PageOut>& pages = renderer.pager().pages();
    if (pages.empty()) pages.emplace_back();
    std::vector<ImageSlot>& images = renderer.images();

    // ---- object numbering ----
    int nextObj = 3; // 1 = Catalog, 2 = Pages
    struct FaceObjs { int fontFile = 0, descriptor = 0, toUnicode = 0, cidFont = 0, type0 = 0; };
    FaceObjs faceObjs[4];
    for (int i = 0; i < 4; ++i) {
        if (!faces.isUsed(i)) continue;
        faceObjs[i].fontFile = nextObj++;
        faceObjs[i].descriptor = nextObj++;
        faceObjs[i].toUnicode = nextObj++;
        faceObjs[i].cidFont = nextObj++;
        faceObjs[i].type0 = nextObj++;
    }
    std::vector<int> imageObjs(images.size(), 0);
    std::vector<int> smaskObjs(images.size(), 0);
    for (size_t i = 0; i < images.size(); ++i) {
        imageObjs[i] = nextObj++;
        if (!images[i].enc.smask.empty()) smaskObjs[i] = nextObj++;
    }
    std::vector<int> pageObjs(pages.size(), 0), contentObjs(pages.size(), 0);
    for (size_t i = 0; i < pages.size(); ++i) {
        pageObjs[i] = nextObj++;
        contentObjs[i] = nextObj++;
    }

    std::map<int, std::string> bodies;

    // ---- fonts ----
    for (int i = 0; i < 4; ++i) {
        if (!faces.isUsed(i)) continue;
        FaceStyle style = static_cast<FaceStyle>(i);
        const TrueTypeFont& font = faces.font(style);
        const std::set<uint16_t>& gids = faces.used[i];

        std::string subset = subsetFont(font, gids);
        if (subset.empty()) subset = font.raw; // better a fat PDF than a broken one

        bodies[faceObjs[i].fontFile] =
            streamObject("/Length1 " + num(static_cast<int>(subset.size())), subset, true);

        double scale = 1000.0 / static_cast<double>(font.unitsPerEm ? font.unitsPerEm : 1000);
        std::string baseName = escapeName(font.psName);
        std::ostringstream desc;
        desc << "<< /Type /FontDescriptor /FontName /" << baseName
             << " /Flags " << (font.italicAngle != 0 ? 68 : 4)
             << " /FontBBox [" << num(std::lround(font.bboxXMin * scale)) << " "
             << num(std::lround(font.bboxYMin * scale)) << " "
             << num(std::lround(font.bboxXMax * scale)) << " "
             << num(std::lround(font.bboxYMax * scale)) << "]"
             << " /ItalicAngle " << num(font.italicAngle)
             << " /Ascent " << num(std::lround(font.ascender * scale))
             << " /Descent " << num(std::lround(font.descender * scale))
             << " /CapHeight " << num(std::lround(font.capHeight * scale))
             << " /StemV 80 /FontFile2 " << faceObjs[i].fontFile << " 0 R >>";
        bodies[faceObjs[i].descriptor] = desc.str();

        bodies[faceObjs[i].toUnicode] =
            streamObject("", buildToUnicode(builder.glyphText(i)), true);

        std::ostringstream cid;
        cid << "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" << baseName
            << " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
            << " /FontDescriptor " << faceObjs[i].descriptor << " 0 R"
            << " /DW 1000 /CIDToGIDMap /Identity /W " << buildWidthArray(font, gids) << " >>";
        bodies[faceObjs[i].cidFont] = cid.str();

        std::ostringstream t0;
        t0 << "<< /Type /Font /Subtype /Type0 /BaseFont /" << baseName
           << " /Encoding /Identity-H /DescendantFonts [" << faceObjs[i].cidFont << " 0 R]"
           << " /ToUnicode " << faceObjs[i].toUnicode << " 0 R >>";
        bodies[faceObjs[i].type0] = t0.str();
    }

    // ---- images ----
    for (size_t i = 0; i < images.size(); ++i) {
        const EncodedImage& e = images[i].enc;
        std::ostringstream dict;
        dict << "<< /Type /XObject /Subtype /Image /Width " << e.width << " /Height " << e.height
             << " /ColorSpace /" << e.colorSpace << " /BitsPerComponent 8 /Filter /" << e.filter;
        if (smaskObjs[i]) dict << " /SMask " << smaskObjs[i] << " 0 R";
        dict << " /Length " << e.data.size() << " >>\nstream\n";
        bodies[imageObjs[i]] = dict.str() + e.data + "\nendstream";

        if (smaskObjs[i]) {
            std::ostringstream sm;
            sm << "<< /Type /XObject /Subtype /Image /Width " << e.width << " /Height " << e.height
               << " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length "
               << e.smask.size() << " >>\nstream\n";
            bodies[smaskObjs[i]] = sm.str() + e.smask + "\nendstream";
        }
    }

    // ---- pages ----
    std::string fontRes;
    for (int i = 0; i < 4; ++i) {
        if (!faces.isUsed(i)) continue;
        fontRes += " /F" + num(i) + " " + num(faceObjs[i].type0) + " 0 R";
    }
    // Fallback aliases: a face that was never used still gets referenced if a
    // rounding of the style map ever points at it, so alias it to Regular.
    for (int i = 0; i < 4; ++i) {
        if (faces.isUsed(i)) continue;
        int canon = faceIndex(faces.canonical(static_cast<FaceStyle>(i)));
        if (faces.isUsed(canon))
            fontRes += " /F" + num(i) + " " + num(faceObjs[canon].type0) + " 0 R";
    }

    for (size_t i = 0; i < pages.size(); ++i) {
        std::string xobj;
        std::set<size_t> usedImages;
        for (const auto& im : pages[i].images) usedImages.insert(im.imageIndex);
        for (size_t idx : usedImages) {
            if (idx < imageObjs.size())
                xobj += " /Im" + num(static_cast<int>(idx)) + " " + num(imageObjs[idx]) + " 0 R";
        }

        std::ostringstream pg;
        pg << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << num(PAGE_W) << " " << num(PAGE_H)
           << "] /Resources << /Font <<" << fontRes << " >>";
        if (!xobj.empty()) pg << " /XObject <<" << xobj << " >>";
        pg << " >> /Contents " << contentObjs[i] << " 0 R >>";
        bodies[pageObjs[i]] = pg.str();

        // Text is written out as glyph ids only at this point, once the face
        // tables are final.
        std::ostringstream cs;
        for (const auto& f : pages[i].fills) {
            cs << num(f.gray) << " g\n"
               << num(f.x) << " " << num(f.y) << " " << num(f.w) << " " << num(f.h) << " re f\n";
        }
        if (!pages[i].fills.empty()) cs << "0 g\n";
        if (!pages[i].rules.empty()) {
            cs << num(BORDER_W) << " w\n0.4 G\n";
            for (const auto& r : pages[i].rules)
                cs << num(r.x1) << " " << num(r.y1) << " m " << num(r.x2) << " " << num(r.y2)
                   << " l S\n";
            cs << "0 G\n";
        }
        for (const auto& im : pages[i].images) {
            if (im.imageIndex >= images.size()) continue;
            cs << "q\n"
               << num(im.w) << " 0 0 " << num(im.h) << " " << num(im.x) << " " << num(im.y)
               << " cm\n/Im" << im.imageIndex << " Do\nQ\n";
        }
        for (const auto& t : pages[i].texts) {
            if (t.text.empty()) continue;
            const TrueTypeFont& font = faces.font(t.face);
            std::string glyphs;
            for (char32_t cp : decodeUtf8(t.text)) glyphs += hex4(glyphFor(font, cp));
            if (glyphs.empty()) continue;
            cs << "BT\n/F" << faceIndex(faces.canonical(t.face)) << " " << num(t.size) << " Tf\n";
            // The fill colour is graphics state, not part of the text
            // object, so it has to be reset afterwards or every later run
            // inherits it.
            double r = 0, g = 0, b = 0;
            bool colored = parseHexColor(t.color, r, g, b);
            if (colored) cs << num(r) << " " << num(g) << " " << num(b) << " rg\n";
            cs << "1 0 0 1 " << num(t.x) << " " << num(t.y) << " Tm\n<" << glyphs << "> Tj\nET\n";
            if (colored) cs << "0 g\n";
        }
        bodies[contentObjs[i]] = streamObject("", cs.str(), true);
    }

    // ---- catalog + page tree ----
    bodies[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    {
        std::ostringstream kids;
        kids << "<< /Type /Pages /Count " << pages.size() << " /Kids [";
        for (size_t i = 0; i < pageObjs.size(); ++i) kids << " " << pageObjs[i] << " 0 R";
        kids << " ] >>";
        bodies[2] = kids.str();
    }

    // ---- serialise ----
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(nextObj, 0);
    for (int i = 1; i < nextObj; ++i) {
        auto it = bodies.find(i);
        offsets[i] = out.size();
        out += num(i) + " 0 obj\n";
        out += it == bodies.end() ? "<< >>" : it->second;
        out += "\nendobj\n";
    }

    size_t xrefPos = out.size();
    out += "xref\n0 " + num(nextObj) + "\n";
    out += "0000000000 65535 f \n";
    for (int i = 1; i < nextObj; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[i]);
        out += buf;
    }
    out += "trailer\n<< /Size " + num(nextObj) + " /Root 1 0 R >>\nstartxref\n" + num(static_cast<int>(xrefPos)) + "\n%%EOF\n";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("Nem sikerult letrehozni a PDF fajlt: " + path);
    size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (written != out.size()) throw std::runtime_error("Nem sikerult kiirni a PDF tartalmat: " + path);
}
