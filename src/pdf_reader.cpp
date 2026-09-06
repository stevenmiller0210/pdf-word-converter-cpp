#include "pdf_reader.h"
#include "process_util.h"
#include "xml_lite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Recovering a document from a PDF is inherently reconstruction, not
// parsing: a PDF stores glyphs at coordinates and has no notion of a
// paragraph, a heading or a list. What it does still carry is geometry, and
// poppler's `-bbox-layout` hands that over — every word with its bounding
// box. From those boxes this reader rebuilds the three things that actually
// matter for a readable .docx: where paragraphs start and end, which lines
// are headings (bigger type), and which are list items.
//
// The previous version used `-layout` and made one paragraph per *line*,
// which meant every wrapped sentence arrived in Word as a separate
// paragraph that would never reflow.

namespace {

bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
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

struct SrcLine {
    std::string text;
    double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    double height() const { return yMax - yMin; }
};

struct SrcBlock {
    std::vector<SrcLine> lines;
    double xMax = 0;
    double xMin = 0;
};

std::vector<SrcBlock> parseBBox(const std::string& xhtml) {
    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xhtml);
    } catch (...) {
        return {};
    }
    std::vector<SrcBlock> blocks;
    for (const auto& root : roots) {
        std::vector<const xmllite::Node*> blockNodes;
        xmllite::findAll(root, "block", blockNodes);
        for (const auto* b : blockNodes) {
            SrcBlock sb;
            sb.xMin = dblAttr(*b, "xMin", 0);
            sb.xMax = dblAttr(*b, "xMax", 0);
            std::vector<const xmllite::Node*> lineNodes;
            xmllite::findAll(*b, "line", lineNodes);
            for (const auto* l : lineNodes) {
                SrcLine sl;
                sl.xMin = dblAttr(*l, "xMin", 0);
                sl.xMax = dblAttr(*l, "xMax", 0);
                sl.yMin = dblAttr(*l, "yMin", 0);
                sl.yMax = dblAttr(*l, "yMax", 0);
                std::vector<const xmllite::Node*> wordNodes;
                xmllite::findAll(*l, "word", wordNodes);
                for (const auto* w : wordNodes) {
                    std::string t = w->allText();
                    if (t.empty()) continue;
                    if (!sl.text.empty()) sl.text += ' ';
                    sl.text += t;
                }
                if (!isBlank(sl.text)) sb.lines.push_back(std::move(sl));
            }
            if (!sb.lines.empty()) blocks.push_back(std::move(sb));
        }
    }
    return blocks;
}

// The most common line height across the document is the body text size;
// everything else is judged relative to it. A mean would be dragged around
// by a single huge title, which is exactly the line we need to stand out.
double bodyLineHeight(const std::vector<SrcBlock>& blocks) {
    std::map<int, int> histogram;
    for (const auto& b : blocks)
        for (const auto& l : b.lines) histogram[static_cast<int>(std::lround(l.height() * 2))]++;
    int best = 0, bestCount = 0;
    for (const auto& kv : histogram) {
        if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
    }
    return best > 0 ? best / 2.0 : 12.0;
}

ParagraphStyle styleForHeight(double height, double body, size_t lineCount) {
    // A long run of text is a paragraph even when it is set large; only
    // short blocks are plausible headings.
    if (lineCount > 3) return ParagraphStyle::Normal;
    double ratio = body > 0 ? height / body : 1.0;
    if (ratio >= 1.70) return ParagraphStyle::Title;
    if (ratio >= 1.40) return ParagraphStyle::Heading1;
    if (ratio >= 1.22) return ParagraphStyle::Heading2;
    if (ratio >= 1.10) return ParagraphStyle::Heading3;
    return ParagraphStyle::Normal;
}

// Strips a leading list marker, reporting what it was. Bullets are matched
// as literal UTF-8 because that is how they arrive from the PDF — the
// original list structure is long gone by then.
bool stripListMarker(std::string& text, ListInfo& info) {
    static const char* kBullets[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA",
                                     "\xE2\x96\xAC", "\xE2\x80\x93", "\xC2\xB7", "-", "*"};
    for (const char* b : kBullets) {
        size_t n = std::string(b).size();
        if (text.size() > n && text.compare(0, n, b) == 0 &&
            std::isspace(static_cast<unsigned char>(text[n]))) {
            text = text.substr(n);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.erase(text.begin());
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
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());
        return true;
    }
    return false;
}

// Decides whether `line` continues the paragraph that `prev` belongs to.
// Four independent signals, any of which ends a paragraph: the type size
// changed, the previous line stopped well short of the column edge, this
// line is indented, or there is an unusually large vertical gap.
bool continuesParagraph(const SrcLine& prev, const SrcLine& line, const SrcBlock& block) {
    double h = std::max(prev.height(), 1.0);
    if (std::fabs(line.height() - prev.height()) > 0.18 * h) return false;

    double blockWidth = std::max(1.0, block.xMax - block.xMin);
    if (prev.xMax < block.xMax - 0.10 * blockWidth) return false;
    if (line.xMin > prev.xMin + 0.02 * blockWidth + 3.0) return false;

    double gap = line.yMin - prev.yMax;
    if (gap > 0.75 * h) return false;
    return true;
}

DocModel fromBlocks(const std::vector<SrcBlock>& blocks) {
    DocModel doc;
    double body = bodyLineHeight(blocks);

    for (const auto& block : blocks) {
        size_t i = 0;
        while (i < block.lines.size()) {
            size_t j = i + 1;
            while (j < block.lines.size() &&
                   continuesParagraph(block.lines[j - 1], block.lines[j], block))
                ++j;

            std::string text;
            double heightSum = 0;
            for (size_t k = i; k < j; ++k) {
                if (!text.empty()) {
                    // A hyphen at a line break is almost always a hyphenated
                    // word being rejoined, not a real hyphen.
                    if (text.size() > 1 && text.back() == '-')
                        text.pop_back();
                    else
                        text += ' ';
                }
                text += block.lines[k].text;
                heightSum += block.lines[k].height();
            }
            double avgHeight = heightSum / static_cast<double>(j - i);

            Paragraph p;
            p.style = styleForHeight(avgHeight, body, j - i);
            ListInfo list;
            if (stripListMarker(text, list) && p.style == ParagraphStyle::Normal) p.list = list;
            if (!isBlank(text)) {
                p.runs.push_back(Run{text, false, false});
                doc.addParagraph(std::move(p));
            }
            i = j;
        }
    }
    return doc;
}

// The plain-text path, kept as a fallback for PDFs whose bbox output poppler
// can't produce (or that this build of poppler renders without one).
DocModel fromPlainText(const std::string& text) {
    DocModel doc;
    std::istringstream iss(text);
    std::string line;
    std::string para;
    auto flush = [&]() {
        if (isBlank(para)) { para.clear(); return; }
        doc.addParagraph(para, ParagraphStyle::Normal);
        para.clear();
    };
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (isBlank(line)) { flush(); continue; }
        if (!para.empty()) para += ' ';
        // Leading layout padding is a column position, not content.
        size_t start = line.find_first_not_of(" \t");
        para += line.substr(start == std::string::npos ? 0 : start);
    }
    flush();
    return doc;
}

} // namespace

DocModel readPdf(const std::string& path) {
    CommandResult bbox = runCommand({"pdftotext", "-bbox-layout", path, "-"});
    if (bbox.exitCode == 0 && !bbox.stdoutData.empty()) {
        std::vector<SrcBlock> blocks = parseBBox(bbox.stdoutData);
        if (!blocks.empty()) {
            DocModel doc = fromBlocks(blocks);
            if (!doc.blocks.empty()) return doc;
        }
    }

    CommandResult res = runCommand({"pdftotext", "-layout", path, "-"});
    if (res.exitCode != 0) {
        throw std::runtime_error(
            "Nem sikerult megnyitni a PDF fajlt (nem valos vagy serult PDF).");
    }

    std::string text = res.stdoutData;
    // A form feed is a page boundary; the flat model has no pages, so it
    // becomes a paragraph break rather than disappearing mid-sentence.
    std::replace(text.begin(), text.end(), '\f', '\n');
    if (isBlank(text)) throw std::runtime_error("NO_TEXT_EXTRACTED");

    DocModel doc = fromPlainText(text);
    if (doc.blocks.empty()) throw std::runtime_error("NO_TEXT_EXTRACTED");
    return doc;
}
