#include "pdf_reader.h"
#include "image_codec.h"
#include "process_util.h"
#include "xml_lite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
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
};

struct Frag {
    double left = 0, top = 0, width = 0, height = 0;
    double fontSize = 11;
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
                 std::vector<Run>& out) {
    for (const auto& c : node.children) {
        if (c.isText()) {
            if (c.text.empty()) continue;
            if (!out.empty() && out.back().bold == bold && out.back().italic == italic &&
                out.back().color == color) {
                out.back().text += c.text;
            } else {
                out.push_back(Run{c.text, bold, italic, color});
            }
            continue;
        }
        bool b = bold || c.tag == "b";
        bool i = italic || c.tag == "i";
        collectRuns(c, b, i, color, out);
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
                    fonts[*id] = fs;
                } else if (child.tag == "text") {
                    Frag f;
                    f.left = dblAttr(child, "left", 0);
                    f.top = dblAttr(child, "top", 0);
                    f.width = dblAttr(child, "width", 0);
                    f.height = dblAttr(child, "height", 0);
                    std::string color;
                    if (const std::string* fid = child.attr("font")) {
                        auto it = fonts.find(*fid);
                        if (it != fonts.end()) {
                            f.fontSize = it->second.size;
                            color = it->second.color;
                        }
                    }
                    collectRuns(child, false, false, color, f.runs);
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

bool continuesParagraph(const SrcLine& prev, const SrcLine& line, double rightEdge,
                        double columnWidth) {
    if (std::fabs(line.fontSize - prev.fontSize) > prev.fontSize * 0.12) return false;
    if (prev.xMax < rightEdge - columnWidth * 0.10) return false;
    if (line.xMin > prev.xMin + columnWidth * 0.02 + 3.0) return false;
    if (line.top - prev.bottom() > prev.height * 0.9) return false;
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

DocModel assemble(const std::vector<SrcPage>& pages) {
    DocModel doc;
    double body = bodyFontSize(pages);

    for (const auto& page : pages) {
        const std::vector<SrcLine>& lines = page.lines;
        std::vector<TableSpan> tables = detectTables(lines);
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
            while (j < lines.size() &&
                   !(tableIdx < tables.size() && tables[tableIdx].first == j) &&
                   continuesParagraph(lines[j - 1], lines[j], rightEdge, columnWidth)) {
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

        DocModel doc = assemble(pages);
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
