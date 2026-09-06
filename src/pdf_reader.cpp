#include "pdf_reader.h"
#include "image_codec.h"
#include "process_util.h"
#include "xml_lite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

// Recovering a document from a PDF is reconstruction, not parsing: a PDF
// stores glyphs at coordinates and has no notion of a paragraph, a heading,
// a list or a table. It does, however, carry more than the earlier versions
// of this reader used.
//
// `pdftotext -layout` gave only characters, so everything was flat text.
// `pdftotext -bbox-layout` added word boxes, which was enough to rebuild
// paragraphs and spot headings by size. This version uses `pdftohtml -xml`
// instead, which additionally reports:
//
//   * <b> / <i> markup, so bold and italic survive;
//   * a <fontspec> per style with its point size and colour, so headings
//     are judged on real font sizes rather than glyph heights, and text
//     colour is preserved;
//   * <image> elements with position and size, with the picture written out
//     alongside — a JPEG comes back as a JPEG, never re-encoded.
//
// Table structure is still not stored anywhere in a PDF, but a table leaves
// an unmistakable trace: several consecutive lines whose pieces start at the
// same few x positions. That is what `detectTables` looks for, deliberately
// conservatively — a missed table degrades to ordinary paragraphs, whereas a
// falsely detected one mangles otherwise fine text.
//
// If there is no text layer at all (a scanned page), the pages are rendered
// and run through Tesseract when it is installed; see `ocrDocument`.

namespace fs = std::filesystem;

namespace {

constexpr double COLUMN_TOLERANCE = 14.0; // points; how far a cell may sit
                                          // from its column's x position

bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

bool haveCommand(const std::string& name) {
    CommandResult res = runCommand({"which", name});
    return res.exitCode == 0 && !res.stdoutData.empty();
}

double dblAttr(const xmllite::Node& n, const char* name, double fallback) {
    const std::string* v = n.attr(name);
    if (!v) return fallback;
    try {
        return std::stod(*v);
    } catch (...) {
        return fallback;
    }
}

std::string readWholeFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string mimeForPath(const std::string& path) {
    std::string t = path;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto ends = [&](const char* s) {
        size_t n = std::string(s).size();
        return t.size() >= n && t.compare(t.size() - n, n, s) == 0;
    };
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

// ---------- the shape pdftohtml gives us -----------------------------------

struct FontSpec {
    double size = 11;
    std::string color; // "RRGGBB", empty for black
    std::string family;
};

struct Frag {
    double left = 0, top = 0, width = 0, height = 0;
    double fontSize = 11;
    std::string family;
    std::vector<Run> runs;

    double right() const { return left + width; }
    std::string text() const {
        std::string out;
        for (const auto& r : runs) out += r.text;
        return out;
    }
    bool empty() const { return isBlank(text()); }
};

// A "cell" is a group of fragments with no visible gap between them.
// Fragments split on styling too — a colour change alone starts a new one —
// so the fragment count says nothing about columns, but a real horizontal
// gap does: that is what separates table cells from a bold word mid-sentence.
struct Cell {
    double left = 0, right = 0;
    std::vector<Run> runs;
};

struct SrcLine {
    double top = 0, height = 0, xMin = 0, xMax = 0, fontSize = 11;
    std::vector<Frag> frags;
    std::vector<Cell> cells;
    Alignment alignment = Alignment::Default;
    double bottom() const { return top + height; }
};

struct SrcImage {
    double top = 0, left = 0, width = 0, height = 0;
    std::string path;
};

struct SrcPage {
    double width = 595, height = 842;
    std::vector<SrcLine> lines;
    std::vector<SrcImage> images;
};

// ---------- parsing --------------------------------------------------------

// A few families whose real name does not follow from splitting the file
// name at its capitals: "DejaVuSans" is "DejaVu Sans", not "Deja Vu Sans",
// and a word processor substitutes silently when the name is wrong.
std::string aliasFontFamily(const std::string& key) {
    static const std::map<std::string, std::string> kAliases = {
        {"dejavusans", "DejaVu Sans"},       {"dejavuserif", "DejaVu Serif"},
        {"dejavusansmono", "DejaVu Sans Mono"}, {"notosans", "Noto Sans"},
        {"notoserif", "Noto Serif"},         {"timesnewroman", "Times New Roman"},
        {"couriernew", "Courier New"},       {"arialunicodems", "Arial Unicode MS"},
    };
    auto it = kAliases.find(key);
    return it == kAliases.end() ? std::string() : it->second;
}

// pdftohtml's <fontspec family="..."> is the font's internal PostScript
// name: a subset prefix ("BAAAAA+"), the family, then a style suffix
// ("-Bold", "-Italic", "MT", "PS") that duplicates the <b>/<i> markup this
// reader already reads separately, so only the family itself is kept.
std::string normaliseFontFamily(const std::string& raw) {
    std::string name = raw;
    if (name.size() > 7 && name[6] == '+') {
        bool allUpper = std::all_of(name.begin(), name.begin() + 6,
                                    [](unsigned char c) { return std::isupper(c); });
        if (allUpper) name = name.substr(7);
    }
    size_t cut = name.find_first_of("-,");
    if (cut != std::string::npos) name = name.substr(0, cut);
    for (const char* suffix : {"PSMT", "MT", "PS"}) {
        size_t n = std::string(suffix).size();
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0)
            name = name.substr(0, name.size() - n);
    }
    if (name.empty()) return {};
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "sans" || lower == "serif" || lower == "monospace") return {};

    std::string key = lower;
    key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
    std::string alias = aliasFontFamily(key);
    if (!alias.empty()) return alias;

    // "LiberationSerif" is a font file's name; "Liberation Serif" is what a
    // word processor calls it — insert a space at each lowercase-to-
    // uppercase transition, unless the name already has one.
    if (name.find(' ') != std::string::npos) return name;
    std::string spaced;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i > 0 && std::islower(static_cast<unsigned char>(name[i - 1])) &&
            std::isupper(static_cast<unsigned char>(name[i])))
            spaced += ' ';
        spaced += name[i];
    }
    return spaced;
}

// pdftohtml writes colours as "#rrggbb"; OOXML wants bare uppercase hex, and
// black is the default everywhere so it is left unset rather than written out
// on every single run.
std::string normaliseColor(const std::string& raw) {
    std::string hex;
    for (char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (hex.size() != 6) return {};
    if (hex == "000000") return {};
    return hex;
}

void collectRuns(const xmllite::Node& node, bool bold, bool italic, const std::string& color,
                 const std::string& family, std::vector<Run>& out) {
    for (const auto& c : node.children) {
        if (c.isText()) {
            if (c.text.empty()) continue;
            Run candidate{c.text, bold, italic, family, color};
            if (!out.empty() && out.back().sameStyle(candidate)) {
                out.back().text += c.text;
            } else {
                out.push_back(std::move(candidate));
            }
            continue;
        }
        bool b = bold || c.tag == "b";
        bool i = italic || c.tag == "i";
        collectRuns(c, b, i, color, family, out);
    }
}

void buildCells(SrcLine& line);

std::vector<SrcPage> parsePdfXml(const std::string& xml, const fs::path& dir) {
    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xml);
    } catch (...) {
        return {};
    }

    std::vector<SrcPage> pages;
    for (const auto& root : roots) {
        std::vector<const xmllite::Node*> pageNodes;
        xmllite::findAll(root, "page", pageNodes);
        for (const auto* p : pageNodes) {
            SrcPage page;
            page.width = dblAttr(*p, "width", 595);
            page.height = dblAttr(*p, "height", 842);

            std::map<std::string, FontSpec> fonts;
            std::vector<Frag> frags;

            for (const auto& child : p->children) {
                if (child.tag == "fontspec") {
                    const std::string* id = child.attr("id");
                    if (!id) continue;
                    FontSpec fs;
                    fs.size = dblAttr(child, "size", 11);
                    if (const std::string* col = child.attr("color"))
                        fs.color = normaliseColor(*col);
                    if (const std::string* fam = child.attr("family"))
                        fs.family = normaliseFontFamily(*fam);
                    fonts[*id] = fs;
                } else if (child.tag == "text") {
                    Frag f;
                    f.left = dblAttr(child, "left", 0);
                    f.top = dblAttr(child, "top", 0);
                    f.width = dblAttr(child, "width", 0);
                    f.height = dblAttr(child, "height", 0);
                    std::string color;
                    std::string family;
                    if (const std::string* fid = child.attr("font")) {
                        auto it = fonts.find(*fid);
                        if (it != fonts.end()) {
                            f.fontSize = it->second.size;
                            color = it->second.color;
                            family = it->second.family;
                        }
                    }
                    f.family = family;
                    collectRuns(child, false, false, color, family, f.runs);
                    if (!f.empty()) frags.push_back(std::move(f));
                } else if (child.tag == "image") {
                    SrcImage img;
                    img.left = dblAttr(child, "left", 0);
                    img.top = dblAttr(child, "top", 0);
                    img.width = dblAttr(child, "width", 0);
                    img.height = dblAttr(child, "height", 0);
                    if (const std::string* src = child.attr("src"))
                        img.path = (dir / *src).string();
                    if (!img.path.empty()) page.images.push_back(std::move(img));
                }
            }

            // Group fragments into visual lines. Fragments on one line can
            // have different tops when their font sizes differ, so the test
            // is vertical overlap, not equality.
            std::sort(frags.begin(), frags.end(), [](const Frag& a, const Frag& b) {
                if (std::fabs(a.top - b.top) > 1.0) return a.top < b.top;
                return a.left < b.left;
            });
            for (auto& f : frags) {
                if (page.lines.empty() ||
                    f.top >= page.lines.back().top + page.lines.back().height * 0.6) {
                    SrcLine line;
                    line.top = f.top;
                    line.height = f.height;
                    line.fontSize = f.fontSize;
                    line.xMin = f.left;
                    line.xMax = f.right();
                    line.frags.push_back(f);
                    page.lines.push_back(std::move(line));
                } else {
                    SrcLine& line = page.lines.back();
                    line.height = std::max(line.height, f.height);
                    line.fontSize = std::max(line.fontSize, f.fontSize);
                    line.xMin = std::min(line.xMin, f.left);
                    line.xMax = std::max(line.xMax, f.right());
                    line.frags.push_back(f);
                }
            }
            for (auto& line : page.lines) {
                std::sort(line.frags.begin(), line.frags.end(),
                          [](const Frag& a, const Frag& b) { return a.left < b.left; });
                buildCells(line);
            }

            pages.push_back(std::move(page));
        }
    }
    return pages;
}

void appendRunsTo(std::vector<Run>& into, const std::vector<Run>& from);

// Splits a line into cells wherever there is a gap wider than a word space.
// A run of ordinary prose ends up as exactly one cell no matter how many
// style changes it contains.
void buildCells(SrcLine& line) {
    line.cells.clear();
    for (const auto& f : line.frags) {
        double gapLimit = std::max(line.fontSize * 0.6, 6.0);
        if (line.cells.empty() || f.left - line.cells.back().right > gapLimit) {
            Cell c;
            c.left = f.left;
            c.right = f.right();
            c.runs = f.runs;
            line.cells.push_back(std::move(c));
        } else {
            Cell& c = line.cells.back();
            // Fragments that merely sit next to each other still need the
            // space the PDF drew between them.
            if (f.left - c.right > line.fontSize * 0.2) {
                std::string sofar;
                for (const auto& r : c.runs) sofar += r.text;
                bool endsSpace = !sofar.empty() &&
                                 std::isspace(static_cast<unsigned char>(sofar.back()));
                bool startsSpace = !f.runs.empty() && !f.runs.front().text.empty() &&
                                   std::isspace(static_cast<unsigned char>(f.runs.front().text.front()));
                if (!endsSpace && !startsSpace && !c.runs.empty()) c.runs.back().text += " ";
            }
            appendRunsTo(c.runs, f.runs);
            c.right = f.right();
        }
    }
}

// ---------- structure recovery ---------------------------------------------

// The most common font size in the document is the body text size;
// everything else is judged relative to it. A mean would be dragged around
// by a single large title, which is exactly the line that has to stand out.
double bodyFontSize(const std::vector<SrcPage>& pages) {
    std::map<int, int> histogram;
    for (const auto& p : pages)
        for (const auto& l : p.lines) histogram[static_cast<int>(std::lround(l.fontSize * 2))]++;
    int best = 0, bestCount = 0;
    for (const auto& kv : histogram)
        if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
    return best > 0 ? best / 2.0 : 11.0;
}

ParagraphStyle styleForSize(double size, double body, size_t lineCount) {
    // A long run of text is a paragraph even when set large; only short
    // blocks are plausible headings.
    if (lineCount > 3) return ParagraphStyle::Normal;
    double ratio = body > 0 ? size / body : 1.0;
    if (ratio >= 1.70) return ParagraphStyle::Title;
    if (ratio >= 1.40) return ParagraphStyle::Heading1;
    if (ratio >= 1.22) return ParagraphStyle::Heading2;
    if (ratio >= 1.10) return ParagraphStyle::Heading3;
    return ParagraphStyle::Normal;
}

// Centred and right-aligned text is unmistakable from where the lines sit;
// justified text is only claimed when every line but the last ends at
// exactly the same x, which ragged-right text never does.
Alignment computeAlignment(const std::vector<SrcLine>& lines, size_t first, size_t last,
                           double leftEdge, double rightEdge) {
    double width = rightEdge - leftEdge;
    if (width <= 20) return Alignment::Default;

    bool centred = true;
    bool right = true;
    for (size_t k = first; k < last; ++k) {
        double lg = lines[k].xMin - leftEdge;
        double rg = rightEdge - lines[k].xMax;
        if (lg < width * 0.05 || std::fabs(lg - rg) > width * 0.06) centred = false;
        if (lg < width * 0.05 || rg > 3.0) right = false;
    }
    if (centred) return Alignment::Center;
    if (right) return Alignment::Right;

    if (last - first > 1) {
        bool justified = true;
        for (size_t k = first; k < last - 1; ++k) {
            if (std::fabs(rightEdge - lines[k].xMax) > 1.0) { justified = false; break; }
        }
        if (justified) return Alignment::Justify;
    }
    return Alignment::Default;
}

bool continuesParagraph(const SrcLine& prev, const SrcLine& line, double leftEdge,
                        double rightEdge, double columnWidth) {
    if (std::fabs(line.fontSize - prev.fontSize) > prev.fontSize * 0.12) return false;
    if (prev.xMax < rightEdge - columnWidth * 0.10) return false;
    if (line.xMin > prev.xMin + columnWidth * 0.02 + 3.0) return false;
    if (line.top - prev.bottom() > prev.height * 0.9) return false;
    // A line that starts well clear of the left margin — a short, centred or
    // right-aligned line — is essentially always a paragraph by itself, not
    // a wrapped continuation. Its own wrapped lines would sit at a similar
    // indent, not flush against the margin, so a line that abruptly starts
    // at the margin right after it is a new paragraph, however close the
    // two sit vertically.
    if (prev.xMin > leftEdge + columnWidth * 0.15 && line.xMin < leftEdge + columnWidth * 0.05)
        return false;
    return true;
}

// Strips a leading list marker from the first run, reporting what it was.
bool stripListMarker(std::vector<Run>& runs, ListInfo& info) {
    if (runs.empty()) return false;
    std::string& text = runs.front().text;

    static const char* kBullets[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA",
                                     "\xE2\x96\xAC", "\xE2\x80\x93", "\xC2\xB7", "-", "*"};
    auto trimFront = [&]() {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());
    };
    for (const char* b : kBullets) {
        size_t n = std::string(b).size();
        if (text.size() > n && text.compare(0, n, b) == 0 &&
            std::isspace(static_cast<unsigned char>(text[n]))) {
            text = text.substr(n);
            trimFront();
            info.kind = ListKind::Bullet;
            return true;
        }
    }

    size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (i > 0 && i <= 3 && i + 1 < text.size() && (text[i] == '.' || text[i] == ')') &&
        std::isspace(static_cast<unsigned char>(text[i + 1]))) {
        info.kind = ListKind::Numbered;
        info.ordinal = std::stoi(text.substr(0, i));
        text = text.substr(i + 1);
        trimFront();
        return true;
    }
    return false;
}

void appendRunsTo(std::vector<Run>& into, const std::vector<Run>& from) {
    for (const auto& r : from) {
        if (r.text.empty()) continue;
        if (!into.empty() && into.back().sameStyle(r)) into.back().text += r.text;
        else into.push_back(r);
    }
}

void appendSeparator(std::vector<Run>& runs, const std::string& sep) {
    if (runs.empty() || sep.empty()) return;
    runs.back().text += sep;
}

// Joins one line's fragments. pdftohtml already keeps the spaces that belong
// to the text, so a space is only inserted across a real horizontal gap.
std::vector<Run> lineRuns(const SrcLine& line) {
    std::vector<Run> out;
    for (const auto& c : line.cells) {
        if (!out.empty()) appendSeparator(out, " ");
        appendRunsTo(out, c.runs);
    }
    return out;
}

// ---------- tables ----------------------------------------------------------

struct TableSpan {
    size_t first = 0, last = 0;      // inclusive line indices
    std::vector<double> columns;     // x position of each column
};

std::vector<double> clusterLefts(const std::vector<SrcLine>& lines, size_t first, size_t last) {
    std::vector<double> lefts;
    for (size_t i = first; i <= last; ++i)
        for (const auto& c : lines[i].cells) lefts.push_back(c.left);
    std::sort(lefts.begin(), lefts.end());

    std::vector<double> columns;
    size_t i = 0;
    while (i < lefts.size()) {
        size_t j = i;
        double sum = 0;
        while (j < lefts.size() && lefts[j] - lefts[i] <= COLUMN_TOLERANCE) { sum += lefts[j]; ++j; }
        columns.push_back(sum / static_cast<double>(j - i));
        i = j;
    }
    return columns;
}

int columnFor(const std::vector<double>& columns, double left) {
    int best = -1;
    double bestDist = COLUMN_TOLERANCE;
    for (size_t i = 0; i < columns.size(); ++i) {
        double d = std::fabs(columns[i] - left);
        if (d <= bestDist) { bestDist = d; best = static_cast<int>(i); }
    }
    return best;
}

// Looks for runs of consecutive lines that split into the same few columns.
// Deliberately conservative: a missed table degrades into ordinary
// paragraphs, while a false positive mangles perfectly good prose.
std::vector<TableSpan> detectTables(const std::vector<SrcLine>& lines) {
    std::vector<TableSpan> spans;
    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].cells.size() < 2) { ++i; continue; }
        size_t j = i;
        while (j + 1 < lines.size() && lines[j + 1].cells.size() >= 2 &&
               lines[j + 1].top - lines[j].bottom() <= lines[j].height * 1.6) {
            ++j;
        }
        if (j > i) {
            std::vector<double> columns = clusterLefts(lines, i, j);
            bool everyFragPlaced = true;
            for (size_t k = i; k <= j && everyFragPlaced; ++k)
                for (const auto& c : lines[k].cells)
                    if (columnFor(columns, c.left) < 0) { everyFragPlaced = false; break; }
            // Two columns is the minimum that means anything, and a "table"
            // with as many columns as it has pieces is just a line of text.
            if (everyFragPlaced && columns.size() >= 2) {
                TableSpan span;
                span.first = i;
                span.last = j;
                span.columns = std::move(columns);
                spans.push_back(std::move(span));
            }
        }
        i = j + 1;
    }
    return spans;
}

std::shared_ptr<Table> buildTable(const std::vector<SrcLine>& lines, const TableSpan& span,
                                  double pageRight) {
    auto table = std::make_shared<Table>();
    size_t cols = span.columns.size();

    for (size_t i = span.first; i <= span.last; ++i) {
        TableRow row;
        std::vector<std::vector<Run>> cells(cols);
        for (const auto& src : lines[i].cells) {
            int c = columnFor(span.columns, src.left);
            if (c < 0) continue;
            if (!cells[c].empty()) appendSeparator(cells[c], " ");
            appendRunsTo(cells[c], src.runs);
        }
        bool allBold = true;
        bool anyText = false;
        for (auto& runs : cells) {
            TableCell cell;
            Paragraph p;
            p.runs = runs;
            for (const auto& r : runs) {
                if (!isBlank(r.text)) {
                    anyText = true;
                    if (!r.bold) allBold = false;
                }
            }
            Block b;
            b.kind = Block::Kind::Paragraph;
            b.paragraph = std::move(p);
            cell.blocks.push_back(std::move(b));
            row.cells.push_back(std::move(cell));
        }
        // A first row set entirely in bold is a header row in every document
        // that has one; anything else would need styling a PDF doesn't keep.
        row.isHeader = (i == span.first) && anyText && allBold;
        table->rows.push_back(std::move(row));
    }

    for (size_t c = 0; c < cols; ++c) {
        double next = (c + 1 < cols) ? span.columns[c + 1] : pageRight;
        table->columnWidths.push_back(std::max(1.0, next - span.columns[c]));
    }
    return table;
}

// ---------- tables drawn with real rules -----------------------------------
//
// Guessing a table from where text happens to line up only ever worked for
// the simple case, and could never see a merged cell, a border colour or
// shading — none of which leave a trace in the text. A ruled table *is*
// drawn, though: `pdftocairo -svg` reports its lines as ordinary stroked
// paths and its shading as filled rectangles, and reading those back gives
// the grid exactly, including merged cells (a missing line between two
// neighbours is precisely what a merged cell is).
//
// pdftocairo puts every glyph outline inside <defs>, referenced later by
// <use>; nothing there is a table line. Everything the drawing actually
// paints comes after </defs>, as flat <path> elements: shading fills carry
// no `transform` and are already in the top-down coordinate space this file
// uses everywhere else; ruling strokes carry `transform="matrix(1,0,0,-1,0,H)"`
// and are in bottom-up PDF space, needing that flip applied by hand. Curves
// are skipped — no table border is ever a curve — which keeps this file from
// needing anything resembling a real SVG engine.

constexpr double RULE_TOLERANCE = 2.0; // pt — how far apart two lines can be
                                       // and still count as one edge

struct VecRule {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    std::string color;
};

struct VecShade {
    double x = 0, y = 0, width = 0, height = 0;
    std::string color;
};

// "rgb(86.665344%, 89.802551%, 94.116211%)" -> "DDE5F0". Cairo always
// writes percentages, never 0-255 or hex, so this is the one format that
// needs handling.
std::string svgColorToHex(const std::string& raw) {
    std::vector<double> pct;
    size_t pos = 0;
    while (pct.size() < 3) {
        size_t start = raw.find_first_of("0123456789.", pos);
        if (start == std::string::npos) break;
        size_t end = raw.find_first_not_of("0123456789.", start);
        if (end == std::string::npos) end = raw.size();
        try {
            pct.push_back(std::stod(raw.substr(start, end - start)));
        } catch (...) {
            return {};
        }
        pos = end;
    }
    if (pct.size() != 3) return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X",
                  static_cast<int>(std::lround(pct[0] * 2.55)),
                  static_cast<int>(std::lround(pct[1] * 2.55)),
                  static_cast<int>(std::lround(pct[2] * 2.55)));
    return buf;
}

std::string svgAttr(const std::string& tag, const std::string& name) {
    std::string needle = name + "=\"";
    size_t pos = tag.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return {};
    return tag.substr(pos, end - pos);
}

std::vector<std::pair<double, double>> svgPathPoints(const std::string& d) {
    std::vector<std::pair<double, double>> pts;
    std::istringstream iss(d);
    std::string tok;
    double x = 0, y = 0;
    bool haveX = false;
    while (iss >> tok) {
        if (tok == "M" || tok == "L") continue;
        if (tok == "Z" || tok == "z") continue;
        try {
            double v = std::stod(tok);
            if (!haveX) { x = v; haveX = true; }
            else { y = v; pts.push_back({x, y}); haveX = false; }
        } catch (...) {
            haveX = false;
        }
    }
    return pts;
}

// Reads one page's ruling lines and shaded rectangles out of a standalone
// SVG render of that page. Returns false (rather than throwing) on any
// failure — a page whose vectors can't be read just falls back to the
// text-alignment table heuristic, same as before this existed.
bool extractPageVectors(const std::string& pdfPath, int pageNumber, double pageHeight,
                        const fs::path& scratchDir, std::vector<VecRule>& rules,
                        std::vector<VecShade>& shades) {
    fs::path svgPath = scratchDir / ("vec" + std::to_string(pageNumber) + ".svg");
    std::string pageArg = std::to_string(pageNumber);
    CommandResult res = runCommand({"pdftocairo", "-svg", "-f", pageArg, "-l", pageArg,
                                    pdfPath, svgPath.string()});
    if (res.exitCode != 0) return false;
    std::string svg = readWholeFile(svgPath);
    std::error_code ec;
    fs::remove(svgPath, ec);
    if (svg.empty()) return false;

    size_t defsEnd = svg.find("</defs>");
    size_t start = defsEnd == std::string::npos ? 0 : defsEnd + 7;

    size_t pos = start;
    while (true) {
        size_t open = svg.find("<path ", pos);
        if (open == std::string::npos) break;
        size_t close = svg.find("/>", open);
        if (close == std::string::npos) break;
        std::string tag = svg.substr(open, close - open + 2);
        pos = close + 2;

        std::string fill = svgAttr(tag, "fill");
        std::string stroke = svgAttr(tag, "stroke");
        std::string d = svgAttr(tag, "d");
        bool hasTransform = tag.find("transform=") != std::string::npos;
        if (d.empty()) continue;
        auto points = svgPathPoints(d);
        if (points.size() < 2) continue;

        if (!stroke.empty() && stroke != "none") {
            std::string color = svgColorToHex(stroke);
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                double x1 = points[i].first, y1 = points[i].second;
                double x2 = points[i + 1].first, y2 = points[i + 1].second;
                if (hasTransform) { y1 = pageHeight - y1; y2 = pageHeight - y2; }
                double dx = std::fabs(x1 - x2), dy = std::fabs(y1 - y2);
                // Only axis-aligned segments can be a cell border; a diagonal
                // is decorative and would corrupt the grid it got fitted to.
                if (dx <= 0.6 && dy > 1.0) rules.push_back({(x1 + x2) / 2, y1, (x1 + x2) / 2, y2, color});
                else if (dy <= 0.6 && dx > 1.0) rules.push_back({x1, (y1 + y2) / 2, x2, (y1 + y2) / 2, color});
            }
        } else if (!fill.empty() && fill != "none") {
            double xMin = points[0].first, xMax = points[0].first;
            double yMin = points[0].second, yMax = points[0].second;
            for (const auto& pt : points) {
                xMin = std::min(xMin, pt.first);
                xMax = std::max(xMax, pt.first);
                yMin = std::min(yMin, pt.second);
                yMax = std::max(yMax, pt.second);
            }
            double w = xMax - xMin, h = yMax - yMin;
            if (w < 1.0 || h < 1.0) continue;
            if (hasTransform) yMin = pageHeight - yMax;
            shades.push_back({xMin, yMin, w, h, svgColorToHex(fill)});
        }
    }
    return true;
}

struct RuleLine {
    double pos = 0, a = 0, b = 0;
    std::string color;
};

// Collapses the many overlapping fragments a producer emits (LibreOffice
// draws each border as two parallel hairlines) into one line per edge.
void mergeRuleLines(std::vector<RuleLine>& lines) {
    std::sort(lines.begin(), lines.end(), [](const RuleLine& a, const RuleLine& b) {
        return a.pos != b.pos ? a.pos < b.pos : a.a < b.a;
    });
    std::vector<RuleLine> out;
    for (auto& line : lines) {
        if (!out.empty() && std::fabs(out.back().pos - line.pos) <= RULE_TOLERANCE &&
            line.a <= out.back().b + RULE_TOLERANCE) {
            out.back().b = std::max(out.back().b, line.b);
            out.back().a = std::min(out.back().a, line.a);
            if (out.back().color.empty()) out.back().color = line.color;
        } else {
            out.push_back(line);
        }
    }
    lines = std::move(out);
}

std::vector<double> clusterPositions(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::vector<double> out;
    for (double v : values) {
        if (out.empty() || v - out.back() > RULE_TOLERANCE * 2) out.push_back(v);
        else out.back() = (out.back() + v) / 2;
    }
    return out;
}

struct RuleComponent {
    std::vector<RuleLine> horizontal, vertical;
};

// One component of touching lines is one table. Two lines belong together
// when they cross or overlap, which keeps two unrelated tables on the same
// page from being merged into one impossible grid.
std::vector<RuleComponent> ruleComponents(std::vector<RuleLine> horizontal,
                                          std::vector<RuleLine> vertical) {
    struct Tagged { RuleLine line; bool horiz; };
    std::vector<Tagged> all;
    for (auto& l : horizontal) all.push_back({l, true});
    for (auto& l : vertical) all.push_back({l, false});

    std::vector<size_t> parent(all.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t i) {
        while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
        return i;
    };
    auto unite = [&](size_t i, size_t j) {
        size_t a = find(i), b = find(j);
        if (a != b) parent[a] = b;
    };

    auto touches = [](const Tagged& p, const Tagged& q) {
        if (p.horiz == q.horiz) {
            return std::fabs(p.line.pos - q.line.pos) <= RULE_TOLERANCE &&
                   q.line.a <= p.line.b + RULE_TOLERANCE && p.line.a <= q.line.b + RULE_TOLERANCE;
        }
        const Tagged& h = p.horiz ? p : q;
        const Tagged& v = p.horiz ? q : p;
        return v.line.pos >= h.line.a - RULE_TOLERANCE && v.line.pos <= h.line.b + RULE_TOLERANCE &&
               h.line.pos >= v.line.a - RULE_TOLERANCE && h.line.pos <= v.line.b + RULE_TOLERANCE;
    };

    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            if (touches(all[i], all[j])) unite(i, j);

    std::map<size_t, RuleComponent> groups;
    for (size_t i = 0; i < all.size(); ++i) {
        auto& comp = groups[find(i)];
        (all[i].horiz ? comp.horizontal : comp.vertical).push_back(all[i].line);
    }
    std::vector<RuleComponent> out;
    for (auto& kv : groups) out.push_back(std::move(kv.second));
    return out;
}

struct GridCell {
    int r = 0, c = 0, rowSpan = 1, colSpan = 1;
    double left = 0, right = 0, top = 0, bottom = 0;
    std::string shading;
    CellBorders borders;
};

struct RuledGrid {
    std::vector<double> rowY; // descending: rowY[0] is the top edge
    std::vector<double> colX; // ascending
    int rows = 0, cols = 0;
    std::vector<GridCell> cells;
    std::string ruleColor;
};

// Builds one ruled table's cell grid, including spans: a cell extends
// across the next boundary exactly when the line that would separate it
// from its neighbour was never drawn.
bool buildGrid(const RuleComponent& component, const std::vector<VecShade>& shades,
              RuledGrid& grid) {
    std::vector<double> rowY = clusterPositions([&] {
        std::vector<double> v;
        for (auto& l : component.horizontal) v.push_back(l.pos);
        return v;
    }());
    std::vector<double> colX = clusterPositions([&] {
        std::vector<double> v;
        for (auto& l : component.vertical) v.push_back(l.pos);
        return v;
    }());
    std::sort(rowY.begin(), rowY.end()); // ascending (top-down y), row 0 = smallest y = top
    if (rowY.size() < 2 || colX.size() < 2) return false;

    std::map<std::string, int> colourCount;
    for (auto& l : component.horizontal) colourCount[l.color.empty() ? "999999" : l.color]++;
    for (auto& l : component.vertical) colourCount[l.color.empty() ? "999999" : l.color]++;
    std::string ruleColor = "999999";
    int best = 0;
    for (auto& kv : colourCount) if (kv.second > best) { best = kv.second; ruleColor = kv.first; }

    auto hasH = [&](int rowIdx, int colIdx) {
        for (auto& l : component.horizontal) {
            if (std::fabs(l.pos - rowY[rowIdx]) <= RULE_TOLERANCE &&
                l.a <= colX[colIdx] + RULE_TOLERANCE && l.b >= colX[colIdx + 1] - RULE_TOLERANCE)
                return true;
        }
        return false;
    };
    // rowY is ascending (row 0's top is the smallest y — see the sort
    // below), so a row's own span is [rowY[rowIdx], rowY[rowIdx+1]] with the
    // *top* the smaller value; containment needs the line to start at or
    // above that top and end at or below that bottom.
    auto hasV = [&](int colIdx, int rowIdx) {
        for (auto& l : component.vertical) {
            if (std::fabs(l.pos - colX[colIdx]) <= RULE_TOLERANCE &&
                l.a <= rowY[rowIdx] + RULE_TOLERANCE && l.b >= rowY[rowIdx + 1] - RULE_TOLERANCE)
                return true;
        }
        return false;
    };

    int rows = static_cast<int>(rowY.size()) - 1;
    int cols = static_cast<int>(colX.size()) - 1;
    std::vector<std::vector<bool>> covered(rows, std::vector<bool>(cols, false));
    std::vector<GridCell> cells;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (covered[r][c]) continue;
            int colSpan = 1;
            while (c + colSpan < cols && !hasV(c + colSpan, r)) ++colSpan;
            int rowSpan = 1;
            while (r + rowSpan < rows) {
                bool separated = false;
                for (int k = 0; k < colSpan; ++k)
                    if (hasH(r + rowSpan, c + k)) { separated = true; break; }
                if (separated) break;
                ++rowSpan;
            }
            for (int dr = 0; dr < rowSpan; ++dr)
                for (int dc = 0; dc < colSpan; ++dc) covered[r + dr][c + dc] = true;

            GridCell cell;
            cell.r = r;
            cell.c = c;
            cell.rowSpan = rowSpan;
            cell.colSpan = colSpan;
            cell.left = colX[c];
            cell.right = colX[c + colSpan];
            cell.top = rowY[r];
            cell.bottom = rowY[r + rowSpan];
            double midX = (cell.left + cell.right) / 2;
            double midY = (cell.top + cell.bottom) / 2;
            for (const auto& sh : shades) {
                if (sh.color.empty()) continue;
                if (midX >= sh.x && midX <= sh.x + sh.width && midY >= sh.y &&
                    midY <= sh.y + sh.height) {
                    cell.shading = sh.color;
                    break;
                }
            }
            cell.borders.top = hasH(r, c);
            cell.borders.bottom = hasH(r + rowSpan, c);
            cell.borders.left = hasV(c, r);
            cell.borders.right = hasV(c + colSpan, r);
            cells.push_back(cell);
        }
    }
    if (cells.size() < 2) return false; // one cell is a box, not a table

    // rowY stays ascending: row 0's top is its smallest y, matching the
    // top-down convention this whole file uses (smaller y = higher up the
    // page). linesInGrid and buildRuledTable both rely on that ordering.
    grid.rowY = rowY;
    grid.colX = colX;
    grid.rows = rows;
    grid.cols = cols;
    grid.cells = std::move(cells);
    grid.ruleColor = ruleColor;
    return true;
}

std::vector<RuledGrid> detectRuledTables(const std::vector<VecRule>& rules,
                                         const std::vector<VecShade>& shades) {
    std::vector<RuledGrid> tables;
    if (rules.empty()) return tables;

    std::vector<RuleLine> horizontal, vertical;
    for (const auto& r : rules) {
        if (std::fabs(r.y1 - r.y2) <= 0.6)
            horizontal.push_back({(r.y1 + r.y2) / 2, std::min(r.x1, r.x2), std::max(r.x1, r.x2), r.color});
        else if (std::fabs(r.x1 - r.x2) <= 0.6)
            vertical.push_back({(r.x1 + r.x2) / 2, std::min(r.y1, r.y2), std::max(r.y1, r.y2), r.color});
    }
    mergeRuleLines(horizontal);
    mergeRuleLines(vertical);

    for (auto& component : ruleComponents(horizontal, vertical)) {
        if (component.horizontal.size() < 2 || component.vertical.size() < 2) continue;
        RuledGrid grid;
        if (buildGrid(component, shades, grid)) tables.push_back(std::move(grid));
    }
    return tables;
}

// Which lines fall inside a ruled table's bounding box, so the paragraph
// pass can skip them instead of printing the table's own contents again.
std::set<size_t> linesInGrid(const RuledGrid& grid, const std::vector<SrcLine>& lines) {
    double left = grid.colX.front();
    double right = grid.colX.back();
    double top = grid.rowY.front();    // smallest y = highest on the page
    double bottom = grid.rowY.back();
    std::set<size_t> inside;
    for (size_t i = 0; i < lines.size(); ++i) {
        const SrcLine& line = lines[i];
        if (line.top >= top - RULE_TOLERANCE && line.bottom() <= bottom + RULE_TOLERANCE &&
            line.xMax >= left - RULE_TOLERANCE && line.xMin <= right + RULE_TOLERANCE) {
            inside.insert(i);
        }
    }
    return inside;
}

std::shared_ptr<Table> buildRuledTable(const RuledGrid& grid, const std::vector<SrcLine>& lines,
                                       const std::set<size_t>& lineIdx) {
    auto table = std::make_shared<Table>();
    double total = grid.colX.back() - grid.colX.front();

    // Every line inside the grid is assigned to the cell whose box contains
    // its vertical centre and whose column range contains its cell group.
    std::map<int, std::vector<std::pair<double, std::vector<Run>>>> byCellKey;
    auto keyFor = [](int r, int c) { return r * 10000 + c; };

    for (size_t idx : lineIdx) {
        const SrcLine& line = lines[idx];
        double midY = line.top + line.height / 2;
        for (const auto& group : line.cells) {
            double midX = (group.left + group.right) / 2;
            const GridCell* target = nullptr;
            for (const auto& cell : grid.cells) {
                if (midX >= cell.left - RULE_TOLERANCE && midX <= cell.right + RULE_TOLERANCE &&
                    midY >= cell.top - RULE_TOLERANCE && midY <= cell.bottom + RULE_TOLERANCE) {
                    target = &cell;
                    break;
                }
            }
            if (!target) continue;
            byCellKey[keyFor(target->r, target->c)].push_back({line.top, group.runs});
        }
    }

    // Word needs every row to carry the same number of grid columns: a
    // row-spanning cell requires an explicit placeholder in each row it
    // continues through, marked vMerge-continue, not just an absence. So the
    // rows are built column by column, tracking which columns are still
    // covered by a merge that started above.
    struct ActiveMerge {
        int rowsLeft;
        int colSpan;
        CellBorders borders;
        std::string shading;
    };
    std::map<int, ActiveMerge> active; // key: starting column

    for (int r = 0; r < grid.rows; ++r) {
        TableRow row;
        bool anyText = false, allBold = true;
        int c = 0;
        while (c < grid.cols) {
            auto activeIt = active.find(c);
            if (activeIt != active.end()) {
                TableCell tc;
                tc.gridSpan = activeIt->second.colSpan;
                tc.verticallyMerged = true;
                tc.borders = activeIt->second.borders;
                tc.shading = activeIt->second.shading;
                Block b;
                b.kind = Block::Kind::Paragraph;
                tc.blocks.push_back(std::move(b));
                row.cells.push_back(std::move(tc));

                c += activeIt->second.colSpan;
                if (--activeIt->second.rowsLeft <= 0) active.erase(activeIt);
                continue;
            }

            const GridCell* cell = nullptr;
            for (const auto& gc : grid.cells)
                if (gc.r == r && gc.c == c) { cell = &gc; break; }
            if (!cell) { ++c; continue; } // should not happen for a sound grid

            auto it = byCellKey.find(keyFor(cell->r, cell->c));
            std::vector<std::pair<double, std::vector<Run>>> pieces;
            if (it != byCellKey.end()) pieces = it->second;
            std::sort(pieces.begin(), pieces.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            std::vector<Run> runs;
            double lastY = -1e18;
            std::vector<Paragraph> paragraphs;
            auto flush = [&]() {
                if (!runs.empty()) {
                    Paragraph p;
                    p.runs = runs;
                    paragraphs.push_back(std::move(p));
                    runs.clear();
                }
            };
            for (auto& piece : pieces) {
                if (lastY != -1e18 && std::fabs(piece.first - lastY) > 1.0) flush();
                else if (!runs.empty()) appendSeparator(runs, " ");
                lastY = piece.first;
                appendRunsTo(runs, piece.second);
            }
            flush();
            if (paragraphs.empty()) paragraphs.push_back(Paragraph{});

            for (const auto& p : paragraphs)
                for (const auto& rn : p.runs)
                    if (!isBlank(rn.text)) { anyText = true; if (!rn.bold) allBold = false; }

            TableCell tc;
            tc.gridSpan = cell->colSpan;
            tc.rowSpan = cell->rowSpan;
            tc.borders.top = cell->borders.top;
            tc.borders.bottom = cell->borders.bottom;
            tc.borders.left = cell->borders.left;
            tc.borders.right = cell->borders.right;
            tc.shading = cell->shading;
            for (auto& p : paragraphs) {
                Block b;
                b.kind = Block::Kind::Paragraph;
                b.paragraph = std::move(p);
                tc.blocks.push_back(std::move(b));
            }
            row.cells.push_back(std::move(tc));

            if (cell->rowSpan > 1)
                active[c] = {cell->rowSpan - 1, cell->colSpan, cell->borders, cell->shading};
            c += cell->colSpan;
        }
        row.isHeader = (r == 0) && anyText && allBold;
        table->rows.push_back(std::move(row));
    }

    for (size_t c = 0; c + 1 < grid.colX.size(); ++c)
        table->columnWidths.push_back(std::max(1.0, (grid.colX[c + 1] - grid.colX[c]) / total * 100.0));
    return table;
}

// ---------- running headers and footers -------------------------------------
//
// A PDF has no notion of a header or a footer; it just draws that text on
// every page like anything else. What gives it away is exactly that: the
// same line, at the same height, in the top or bottom margin, page after
// page. One page can never be enough to tell a header from a first line, so
// this only runs on documents with at least two.
struct RunningText {
    std::vector<Paragraph> header, footer;
    std::vector<std::set<size_t>> drop; // per page, indices into page.lines
};

// Page numbers change from page to page, so a line is compared with its
// digits masked out.
std::string maskDigits(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out += std::isdigit(static_cast<unsigned char>(c)) ? '#' : c;
    return out;
}

RunningText findRunningText(const std::vector<SrcPage>& pages) {
    RunningText result;
    result.drop.resize(pages.size());
    if (pages.size() < 2) return result;

    struct Hit { size_t pageIndex, lineIndex; const SrcLine* line; };
    for (int zone = 0; zone < 2; ++zone) {
        bool isHeader = (zone == 0);
        std::map<std::string, std::vector<Hit>> buckets;
        for (size_t pi = 0; pi < pages.size(); ++pi) {
            const SrcPage& page = pages[pi];
            for (size_t li = 0; li < page.lines.size(); ++li) {
                const SrcLine& line = page.lines[li];
                bool inZone = isHeader ? (line.top < page.height * 0.10)
                                       : (line.bottom() > page.height * 0.90);
                if (!inZone) continue;
                std::string text;
                for (const auto& c : line.cells) for (const auto& r : c.runs) text += r.text;
                std::string key = std::to_string(static_cast<int>(std::lround(line.top))) + "|" +
                                  maskDigits(text);
                buckets[key].push_back({pi, li, &line});
            }
        }
        // Candidate buckets pass the repetition test on their own, but
        // whether keeping them is safe can only be judged once every
        // candidate in this zone is known: a page whose one paragraph
        // happens to sit near the margin and happens to repeat after its
        // digits are masked looks exactly like a real header on its own,
        // and only shows up as a problem once the header, the footer and
        // this false positive are added up and found to cover the page's
        // entire content.
        std::vector<std::pair<Paragraph, std::vector<Hit>>> accepted;
        std::map<size_t, std::set<size_t>> candidateDrop; // page -> line indices
        for (auto& kv : buckets) {
            std::set<size_t> distinctPages;
            for (auto& h : kv.second) distinctPages.insert(h.pageIndex);
            if (distinctPages.size() < 2 || distinctPages.size() < pages.size() / 2) continue;
            for (auto& h : kv.second) candidateDrop[h.pageIndex].insert(h.lineIndex);
            Paragraph p;
            p.runs = lineRuns(*kv.second.front().line);
            accepted.push_back({std::move(p), kv.second});
        }

        // A header/footer always coexists with body content: never let this
        // zone's candidates, combined, remove every line a page has.
        std::set<size_t> emptiedPages;
        for (size_t pi = 0; pi < pages.size(); ++pi) {
            auto it = candidateDrop.find(pi);
            if (it != candidateDrop.end() && it->second.size() >= pages[pi].lines.size())
                emptiedPages.insert(pi);
        }

        for (auto& entry : accepted) {
            bool touchesEmptiedPage = false;
            for (auto& h : entry.second)
                if (emptiedPages.count(h.pageIndex)) { touchesEmptiedPage = true; break; }
            if (touchesEmptiedPage) continue;
            for (auto& h : entry.second) result.drop[h.pageIndex].insert(h.lineIndex);
            (isHeader ? result.header : result.footer).push_back(std::move(entry.first));
        }
    }
    return result;
}

// ---------- assembly --------------------------------------------------------

Block imageBlock(const SrcImage& src) {
    Block b;
    b.kind = Block::Kind::Image;
    auto img = std::make_shared<Image>();
    img->bytes = readWholeFile(src.path);
    img->mime = mimeForPath(src.path);
    probeImageSize(img->bytes, img->mime, img->pixelWidth, img->pixelHeight);
    img->displayWidthPt = src.width;
    img->displayHeightPt = src.height;
    b.image = img;
    return b;
}

DocModel assemble(const std::vector<SrcPage>& pages, const std::string& pdfPath,
                  const fs::path& scratchDir) {
    DocModel doc;
    double body = bodyFontSize(pages);
    RunningText running = findRunningText(pages);
    doc.header = running.header;
    doc.footer = running.footer;

    for (size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
        const SrcPage& page = pages[pageIndex];
        const std::vector<SrcLine>& lines = page.lines;

        // A table that was actually drawn beats one guessed from where the
        // text lines up: the rules give the grid exactly, including merged
        // cells, borders and shading, none of which the text alone reveals.
        std::vector<VecRule> vecRules;
        std::vector<VecShade> vecShades;
        extractPageVectors(pdfPath, static_cast<int>(pageIndex) + 1, page.height, scratchDir,
                           vecRules, vecShades);
        std::vector<RuledGrid> grids = detectRuledTables(vecRules, vecShades);

        std::map<size_t, std::pair<const RuledGrid*, std::set<size_t>>> ruled;
        std::set<size_t> consumed = running.drop[pageIndex];
        for (const auto& grid : grids) {
            std::set<size_t> inside = linesInGrid(grid, lines);
            if (inside.empty()) continue;
            size_t first = *inside.begin();
            ruled[first] = {&grid, inside};
            for (size_t idx : inside) consumed.insert(idx);
        }

        // The text-alignment fallback only covers what no rule already
        // claimed — a table drawn without any visible rules at all.
        std::vector<TableSpan> tables;
        for (auto& span : detectTables(lines)) {
            bool overlaps = false;
            for (size_t k = span.first; k <= span.last && !overlaps; ++k)
                if (consumed.count(k)) overlaps = true;
            if (!overlaps) tables.push_back(std::move(span));
        }

        // Images are interleaved by their vertical position, so a picture
        // between two paragraphs stays between them.
        size_t nextImage = 0;
        std::vector<SrcImage> images = page.images;
        std::sort(images.begin(), images.end(),
                  [](const SrcImage& a, const SrcImage& b) { return a.top < b.top; });

        auto flushImagesBefore = [&](double top) {
            while (nextImage < images.size() && images[nextImage].top <= top) {
                Block b = imageBlock(images[nextImage++]);
                if (b.image && !b.image->bytes.empty()) doc.addBlock(std::move(b));
            }
        };

        double rightEdge = 0, leftEdge = page.width;
        for (const auto& l : lines) {
            rightEdge = std::max(rightEdge, l.xMax);
            leftEdge = std::min(leftEdge, l.xMin);
        }
        double columnWidth = std::max(1.0, rightEdge - leftEdge);

        size_t tableIdx = 0;
        size_t i = 0;
        while (i < lines.size()) {
            auto ruledIt = ruled.find(i);
            if (ruledIt != ruled.end()) {
                flushImagesBefore(lines[i].top);
                std::shared_ptr<Table> table =
                    buildRuledTable(*ruledIt->second.first, lines, ruledIt->second.second);
                if (table) {
                    Block b;
                    b.kind = Block::Kind::Table;
                    b.table = table;
                    doc.addBlock(std::move(b));
                }
                i = *ruledIt->second.second.rbegin() + 1;
                continue;
            }
            if (consumed.count(i)) { ++i; continue; }
            if (tableIdx < tables.size() && tables[tableIdx].first == i) {
                flushImagesBefore(lines[i].top);
                Block b;
                b.kind = Block::Kind::Table;
                b.table = buildTable(lines, tables[tableIdx], rightEdge);
                doc.addBlock(std::move(b));
                i = tables[tableIdx].last + 1;
                ++tableIdx;
                continue;
            }

            size_t j = i + 1;
            while (j < lines.size() && !consumed.count(j) && ruled.find(j) == ruled.end() &&
                   !(tableIdx < tables.size() && tables[tableIdx].first == j) &&
                   continuesParagraph(lines[j - 1], lines[j], leftEdge, rightEdge, columnWidth)) {
                ++j;
            }

            flushImagesBefore(lines[i].top);

            Paragraph para;
            double sizeSum = 0;
            for (size_t k = i; k < j; ++k) {
                std::vector<Run> runs = lineRuns(lines[k]);
                if (!para.runs.empty()) {
                    std::string sofar;
                    for (const auto& r : para.runs) sofar += r.text;
                    // A line ending in a hyphen is joined without a space,
                    // but the hyphen is kept: word processors do not
                    // auto-hyphenate unless it is switched on, so it is
                    // almost always a real one from a compound word.
                    if (!(sofar.size() > 1 && sofar.back() == '-')) appendSeparator(para.runs, " ");
                }
                appendRunsTo(para.runs, runs);
                sizeSum += lines[k].fontSize;
            }
            double avgSize = sizeSum / static_cast<double>(j - i);
            para.style = styleForSize(avgSize, body, j - i);
            para.alignment = computeAlignment(lines, i, j, leftEdge, rightEdge);
            if (para.style == ParagraphStyle::Normal) {
                ListInfo list;
                if (stripListMarker(para.runs, list)) para.list = list;
            }
            if (!para.empty()) doc.addParagraph(std::move(para));
            i = j;
        }
        flushImagesBefore(page.height);
    }
    return doc;
}

bool hasText(const DocModel& doc) {
    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Kind::Paragraph && !isBlank(b.paragraph.text())) return true;
        if (b.kind == Block::Kind::Table && b.table) {
            for (const auto& row : b.table->rows)
                for (const auto& cell : row.cells)
                    for (const auto& inner : cell.blocks)
                        if (inner.kind == Block::Kind::Paragraph &&
                            !isBlank(inner.paragraph.text()))
                            return true;
        }
    }
    return false;
}

// ---------- OCR -------------------------------------------------------------

// A scanned page has no text layer at all, so there is nothing to recover —
// unless the pixels are read back. Tesseract is optional: when it isn't
// installed the caller still gets the honest "no text in this PDF" error
// rather than a silent empty document.
DocModel ocrDocument(const std::string& path) {
    if (!haveCommand("tesseract") || !haveCommand("pdftoppm"))
        throw std::runtime_error("NO_TEXT_EXTRACTED");

    // Prefer Hungarian + English when the language data is installed; asking
    // for a language Tesseract doesn't have makes it fail outright.
    std::string langs;
    CommandResult list = runCommand({"tesseract", "--list-langs"});
    bool hun = list.stdoutData.find("hun") != std::string::npos;
    bool eng = list.stdoutData.find("eng") != std::string::npos;
    if (hun && eng) langs = "hun+eng";
    else if (hun) langs = "hun";
    else if (eng) langs = "eng";

    char tmpl[] = "/tmp/pdfwordocr_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) throw std::runtime_error("NO_TEXT_EXTRACTED");
    fs::path root(dir);

    std::string text;
    try {
        // 300 dpi is the resolution Tesseract's models are trained around;
        // below roughly 200 the accuracy falls off a cliff.
        std::string base = (root / "page").string();
        CommandResult render = runCommand({"pdftoppm", "-r", "300", "-png", path, base});
        if (render.exitCode != 0) throw std::runtime_error("NO_TEXT_EXTRACTED");

        std::vector<fs::path> pages;
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.path().extension() == ".png") pages.push_back(entry.path());
        std::sort(pages.begin(), pages.end());
        if (pages.empty()) throw std::runtime_error("NO_TEXT_EXTRACTED");

        for (const auto& page : pages) {
            std::vector<std::string> argv{"tesseract", page.string(), "stdout"};
            if (!langs.empty()) { argv.push_back("-l"); argv.push_back(langs); }
            CommandResult ocr = runCommand(argv);
            if (ocr.exitCode == 0) {
                if (!text.empty()) text += "\n\n";
                text += ocr.stdoutData;
            }
        }
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(root, ec);

    if (isBlank(text)) throw std::runtime_error("NO_TEXT_EXTRACTED");

    // OCR output is plain lines; a blank line ends a paragraph.
    DocModel doc;
    std::istringstream iss(text);
    std::string line, para;
    auto flush = [&]() {
        if (!isBlank(para)) doc.addParagraph(para, ParagraphStyle::Normal);
        para.clear();
    };
    while (std::getline(iss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (isBlank(line)) { flush(); continue; }
        if (!para.empty()) {
            if (para.size() > 1 && para.back() == '-') { /* keep the hyphen */ }
            else para += ' ';
        }
        size_t start = line.find_first_not_of(" \t");
        para += line.substr(start == std::string::npos ? 0 : start);
    }
    flush();
    if (doc.blocks.empty()) throw std::runtime_error("NO_TEXT_EXTRACTED");
    return doc;
}

} // namespace

DocModel readPdf(const std::string& path) {
    if (!haveCommand("pdftohtml"))
        throw std::runtime_error(
            "Hianyzik a `pdftohtml` (poppler-utils) - enelkul nem tudom feldolgozni a PDF-et.");

    char tmpl[] = "/tmp/pdfwordread_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) throw std::runtime_error("Nem sikerult ideiglenes konyvtarat letrehozni.");
    fs::path root(dir);

    std::vector<SrcPage> pages;
    try {
        // -zoom 1 keeps every coordinate and font size in PDF points, so no
        // scale factor has to be tracked through the rest of this file.
        std::string base = (root / "out").string();
        CommandResult res = runCommand({"pdftohtml", "-xml", "-zoom", "1", "-nodrm", path, base});
        std::string xml = readWholeFile(root / "out.xml");
        if (res.exitCode != 0 && xml.empty()) {
            throw std::runtime_error(
                "Nem sikerult megnyitni a PDF fajlt (nem valos vagy serult PDF).");
        }
        if (!xml.empty()) pages = parsePdfXml(xml, root);

        DocModel doc = assemble(pages, path, root);
        // Images alone are not a conversion: a .docx holding nothing but a
        // picture of a page looks converted while none of it is editable.
        // A page like that is exactly the scanned case, so it goes to OCR,
        // and if that can't run the caller gets the honest "no text here"
        // error instead.
        if (hasText(doc)) {
            std::error_code ec;
            fs::remove_all(root, ec);
            return doc;
        }
    } catch (const std::exception&) {
        std::error_code ec;
        fs::remove_all(root, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(root, ec);

    // No text layer: fall back to OCR, which itself throws
    // NO_TEXT_EXTRACTED when it can't run or finds nothing.
    return ocrDocument(path);
}
