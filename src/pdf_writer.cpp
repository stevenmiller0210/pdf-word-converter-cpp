#include "pdf_writer.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <cstdio>

namespace {

constexpr double PAGE_W = 595.0;
constexpr double PAGE_H = 842.0;
constexpr double MARGIN = 56.0;
constexpr double USABLE_W = PAGE_W - 2 * MARGIN;
constexpr double USABLE_TOP_Y = PAGE_H - MARGIN;
constexpr double USABLE_BOTTOM_Y = MARGIN;

// ---- UTF-8 decoding (our doc_model text is always UTF-8) ----
std::vector<char32_t> decodeUtf8(const std::string& s) {
    std::vector<char32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        char32_t cp;
        int len;
        if ((c & 0x80) == 0) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; } // invalid leading byte, skip
        if (i + static_cast<size_t>(len) > s.size()) break;
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { ++i; continue; }
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// Maps a Unicode code point to a single output byte in our custom
// PDF font encoding (WinAnsiEncoding + a /Differences block — see
// kEncodingDict below). Returns 0 if the character has no mapping.
unsigned char encodeGlyphByte(char32_t cp) {
    if (cp < 0x80) return static_cast<unsigned char>(cp);
    if (cp >= 0xA0 && cp <= 0xFF) return static_cast<unsigned char>(cp); // WinAnsi == Latin-1 here
    switch (cp) {
        case 0x0150: return 0x80; // Ő
        case 0x0151: return 0x81; // ő
        case 0x0170: return 0x82; // Ű
        case 0x0171: return 0x83; // ű
        case 0x2018: return 0x91; // ‘
        case 0x2019: return 0x92; // ’
        case 0x201C: return 0x93; // “
        case 0x201D: return 0x94; // ”
        case 0x2013: return 0x96; // –
        case 0x2014: return 0x97; // —
        default: return 0;
    }
}

std::string toPdfBytes(const std::string& utf8) {
    std::string out;
    for (char32_t cp : decodeUtf8(utf8)) {
        unsigned char b = encodeGlyphByte(cp);
        out += (b != 0) ? static_cast<char>(b) : '?';
    }
    return out;
}

// Approximate glyph width as a fraction of font size — deliberately not a
// real AFM metrics table (see README): categorized widths, always rounded
// toward "wider than reality" for anything uncertain, so wrapping can never
// under-estimate and overflow the page. A little extra whitespace at line
// ends is the acceptable trade-off.
double charWidthEm(unsigned char b, bool bold) {
    double w;
    switch (b) {
        case ' ': case '\'': case '.': case ',': case ':': case ';':
        case '!': case 'i': case 'l': case 'I': case 'j': case 't': case 'f':
        case '|': case '(': case ')': case '[': case ']':
            w = 0.30; break;
        case 'r': case '"':
            w = 0.36; break;
        case 'm': case 'M': case 'w': case 'W':
            w = 0.90; break;
        default:
            if (b >= '0' && b <= '9') w = 0.56;
            else if (b >= 'A' && b <= 'Z') w = 0.70;
            else if (b >= 'a' && b <= 'z') w = 0.52;
            else w = 0.62; // punctuation, accented letters, symbols
    }
    return bold ? w * 1.08 : w;
}

double textWidthPts(const std::string& pdfBytes, double fontSize, bool bold) {
    double total = 0;
    for (unsigned char b : pdfBytes) total += charWidthEm(b, bold);
    return total * fontSize;
}

// Greedy word-wrap on spaces, with a guaranteed-safe hard-break fallback so
// a single "word" wider than the page (e.g. a long URL) can never overflow.
std::vector<std::string> wrapLine(const std::string& pdfBytes, double maxWidth,
                                   double fontSize, bool bold) {
    std::vector<std::string> lines;
    std::string current;
    std::string word;

    auto flushWord = [&](const std::string& w) {
        if (w.empty()) return;
        std::string candidate = current.empty() ? w : current + " " + w;
        if (current.empty() || textWidthPts(candidate, fontSize, bold) <= maxWidth) {
            current = candidate;
        } else {
            lines.push_back(current);
            current = w;
        }
    };

    for (size_t i = 0; i <= pdfBytes.size(); ++i) {
        if (i == pdfBytes.size() || pdfBytes[i] == ' ') {
            flushWord(word);
            word.clear();
        } else {
            word += pdfBytes[i];
        }
    }
    if (!current.empty() || lines.empty()) lines.push_back(current);

    std::vector<std::string> out;
    for (auto& ln : lines) {
        if (ln.empty() || textWidthPts(ln, fontSize, bold) <= maxWidth) { out.push_back(ln); continue; }
        std::string piece;
        for (char c : ln) {
            if (!piece.empty() && textWidthPts(piece + c, fontSize, bold) > maxWidth) {
                out.push_back(piece);
                piece.clear();
            }
            piece += c;
        }
        if (!piece.empty()) out.push_back(piece);
    }
    return out;
}

void styleMetrics(ParagraphStyle style, double& fontSize, bool& bold) {
    switch (style) {
        case ParagraphStyle::Title:    fontSize = 24; bold = true; break;
        case ParagraphStyle::Heading1: fontSize = 18; bold = true; break;
        case ParagraphStyle::Heading2: fontSize = 15; bold = true; break;
        case ParagraphStyle::Heading3: fontSize = 13; bold = true; break;
        default:                       fontSize = 11; bold = false; break;
    }
}

std::string escapePdfLiteral(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char c : bytes) {
        if (c == '\\' || c == '(' || c == ')') out += '\\';
        out += static_cast<char>(c);
    }
    return out;
}

struct StyledLine {
    std::string pdfBytes;
    double fontSize;
    bool bold;
};

struct Page {
    std::vector<StyledLine> lines;
    std::vector<double> yPositions; // baseline y per line, top-down
};

std::vector<Page> layoutPages(const DocModel& doc) {
    std::vector<Page> pages;
    Page current;
    double y = USABLE_TOP_Y;

    auto newPage = [&]() {
        if (!current.lines.empty()) pages.push_back(std::move(current));
        current = Page{};
        y = USABLE_TOP_Y;
    };

    for (const auto& para : doc.paragraphs) {
        double fontSize;
        bool bold;
        styleMetrics(para.style, fontSize, bold);
        double leading = fontSize * 1.35;

        std::string pdfBytes = toPdfBytes(para.text);
        std::vector<std::string> wrapped = wrapLine(pdfBytes, USABLE_W, fontSize, bold);

        for (const auto& ln : wrapped) {
            if (y - leading < USABLE_BOTTOM_Y) newPage();
            current.lines.push_back(StyledLine{ln, fontSize, bold});
            current.yPositions.push_back(y);
            y -= leading;
        }
        y -= leading * 0.35; // paragraph gap
        if (!wrapped.empty() && y < USABLE_BOTTOM_Y) newPage();
    }
    if (!current.lines.empty()) pages.push_back(std::move(current));
    if (pages.empty()) pages.push_back(Page{});
    return pages;
}

std::string buildContentStream(const Page& page) {
    std::ostringstream ss;
    ss << "BT\n";
    double prevY = 0;
    bool first = true;
    std::string curFont;
    double curSize = -1;
    for (size_t i = 0; i < page.lines.size(); ++i) {
        const auto& ln = page.lines[i];
        std::string font = ln.bold ? "/F2" : "/F1";
        if (font != curFont || ln.fontSize != curSize) {
            ss << font << " " << ln.fontSize << " Tf\n";
            curFont = font;
            curSize = ln.fontSize;
        }
        if (first) {
            ss << MARGIN << " " << page.yPositions[i] << " Td\n";
            first = false;
        } else {
            ss << "0 " << (page.yPositions[i] - prevY) << " Td\n";
        }
        prevY = page.yPositions[i];
        ss << "(" << escapePdfLiteral(ln.pdfBytes) << ") Tj\n";
    }
    ss << "ET\n";
    return ss.str();
}

// WinAnsiEncoding as a base, extended via /Differences with the Hungarian
// double-acute letters and a few typographic marks that WinAnsiEncoding
// doesn't define by default — all standard Adobe glyph names that the
// base-14 fonts already contain, so no font embedding is needed.
const char* kEncodingDict =
    "<< /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences "
    "[128 /Ohungarumlaut /ohungarumlaut /Uhungarumlaut /uhungarumlaut "
    "145 /quoteleft /quoteright /quotedblleft /quotedblright /space /endash /emdash] >>";

} // namespace

void writePdf(const std::string& path, const DocModel& doc) {
    std::vector<Page> pages = layoutPages(doc);

    std::vector<std::string> contentStreams;
    contentStreams.reserve(pages.size());
    for (const auto& p : pages) contentStreams.push_back(buildContentStream(p));

    // Object numbers: 1=Catalog 2=Pages 3=FontRegular 4=FontBold, then for
    // page i (0-based): (5+2i)=Page, (6+2i)=its content stream.
    size_t pageCount = pages.size();
    std::vector<std::string> objects(4 + 2 * pageCount);

    std::ostringstream kids;
    kids << "[";
    for (size_t i = 0; i < pageCount; ++i) {
        if (i) kids << " ";
        kids << (5 + 2 * i) << " 0 R";
    }
    kids << "]";

    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[1] = "<< /Type /Pages /Kids " + kids.str() + " /Count " + std::to_string(pageCount) + " >>";
    objects[2] = std::string("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding ") + kEncodingDict + " >>";
    objects[3] = std::string("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding ") + kEncodingDict + " >>";

    for (size_t i = 0; i < pageCount; ++i) {
        objects[4 + 2 * i] =
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(static_cast<int>(PAGE_W)) +
            " " + std::to_string(static_cast<int>(PAGE_H)) +
            "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
            std::to_string(6 + 2 * i) + " 0 R >>";
        const std::string& stream = contentStreams[i];
        objects[5 + 2 * i] = "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" +
                             stream + "endstream";
    }

    std::string pdfOut;
    pdfOut += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(objects.size() + 1, 0); // 1-based

    for (size_t i = 0; i < objects.size(); ++i) {
        offsets[i + 1] = pdfOut.size();
        pdfOut += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    size_t xrefOffset = pdfOut.size();
    pdfOut += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    pdfOut += "0000000000 65535 f \n";
    char buf[32];
    for (size_t i = 1; i <= objects.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[i]);
        pdfOut += buf;
    }
    pdfOut += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\n";
    pdfOut += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Nem sikerult letrehozni a kimeneti PDF fajlt: " + path);
    f << pdfOut;
}
