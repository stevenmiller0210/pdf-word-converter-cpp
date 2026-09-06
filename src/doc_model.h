#pragma once
#include <memory>
#include <string>
#include <vector>

// The in-memory document model shared by both conversion directions.
//
// It is deliberately a *document* model, not a *layout* model: it records what
// the content is (runs of styled text, list membership, table structure,
// pictures) and lets each writer decide how to place it on a page. Anything
// purely presentational in the source — exact line breaks, column positions,
// kerning — is not represented, because neither writer could honour it
// faithfully and pretending otherwise would produce output that is subtly
// wrong rather than honestly simplified.

enum class ParagraphStyle {
    Normal,
    Title,
    Heading1,
    Heading2,
    Heading3,
};

// A contiguous stretch of text sharing one set of character properties.
// Splitting a paragraph into runs is what lets "normal **bold** normal"
// survive a conversion instead of flattening to one uniform style.
struct Run {
    std::string text; // UTF-8
    bool bold = false;
    bool italic = false;
    // "RRGGBB", uppercase hex, empty for the default (black). Kept as a
    // string because that is what OOXML's w:color wants and what a PDF
    // reader hands back; parsing it into components here would just mean
    // both writers reassembling it.
    std::string color;

    bool sameStyle(const Run& other) const {
        return bold == other.bold && italic == other.italic && color == other.color;
    }
};

enum class ListKind {
    None,
    Bullet,
    Numbered,
};

struct ListInfo {
    ListKind kind = ListKind::None;
    int level = 0;   // 0-based nesting depth
    int ordinal = 0; // 1-based position among siblings, assigned by the reader
};

struct Paragraph {
    std::vector<Run> runs;
    ParagraphStyle style = ParagraphStyle::Normal;
    ListInfo list;

    std::string text() const {
        std::string out;
        for (const auto& r : runs) out += r.text;
        return out;
    }
    bool empty() const {
        for (const auto& r : runs)
            if (!r.text.empty()) return false;
        return true;
    }
};

// A picture, kept as the original encoded bytes plus just enough metadata to
// place it. A JPEG is never re-encoded (PDF carries JFIF data directly via
// DCTDecode); PNG is decoded and re-compressed as a Flate image.
struct Image {
    std::string bytes; // original file content
    std::string mime;  // "image/jpeg", "image/png", ...
    int pixelWidth = 0;
    int pixelHeight = 0;
    double displayWidthPt = 0;  // 0 = derive from pixel size
    double displayHeightPt = 0;
};

struct Table;

// One block of document content. A single flat vector of these preserves
// document order across paragraphs, tables and pictures, which a set of
// parallel lists could not.
struct Block {
    enum class Kind { Paragraph, Table, Image } kind = Kind::Paragraph;
    Paragraph paragraph;
    std::shared_ptr<Table> table; // shared_ptr so Block stays copyable while
    std::shared_ptr<Image> image; // Table is still only forward-declared here
};

struct TableCell {
    std::vector<Block> blocks; // cells hold blocks, so nested content works
    int gridSpan = 1;
};

struct TableRow {
    std::vector<TableCell> cells;
    bool isHeader = false;
};

struct Table {
    std::vector<TableRow> rows;
    std::vector<double> columnWidths; // relative weights; empty = equal columns
};

struct DocModel {
    std::vector<Block> blocks;

    void addBlock(Block b) { blocks.push_back(std::move(b)); }

    void addParagraph(Paragraph p) {
        Block b;
        b.kind = Block::Kind::Paragraph;
        b.paragraph = std::move(p);
        blocks.push_back(std::move(b));
    }

    void addParagraph(const std::string& text, ParagraphStyle style = ParagraphStyle::Normal) {
        Paragraph p;
        p.style = style;
        if (!text.empty()) p.runs.push_back(Run{text, false, false, {}});
        addParagraph(std::move(p));
    }
};
