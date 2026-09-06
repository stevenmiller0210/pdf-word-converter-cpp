// Direct smoke test for docx_writer, covering the parts of the document
// model that the PDF -> Word path cannot reach on its own.
//
// PDF text extraction recovers paragraphs, headings and simple lists, but
// never bold/italic runs or tables — the geometry a PDF carries just does
// not say which glyphs were bold. Those writer paths are still real code
// that has to produce a package Word will open, so this test builds a model
// containing all of them directly and writes it out. Verify by opening the
// result (LibreOffice/Word), or with `soffice --headless --convert-to pdf`.
#include "../src/docx_writer.h"
#include <iostream>
#include <memory>

namespace {

Paragraph para(const std::string& text, ParagraphStyle style = ParagraphStyle::Normal) {
    Paragraph p;
    p.style = style;
    p.runs.push_back(Run{text, false, false, {}});
    return p;
}

Block paraBlock(const std::string& text, ParagraphStyle style = ParagraphStyle::Normal) {
    Block b;
    b.kind = Block::Kind::Paragraph;
    b.paragraph = para(text, style);
    return b;
}

TableCell cell(const std::string& text) {
    TableCell c;
    c.blocks.push_back(paraBlock(text));
    return c;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output.docx>\n";
        return 2;
    }
    DocModel doc;

    doc.addParagraph("Cim stilus teszt", ParagraphStyle::Title);
    doc.addParagraph("Elso szintu fejezet", ParagraphStyle::Heading1);

    {
        Paragraph p;
        p.runs.push_back(Run{"Ez ", false, false, {}});
        p.runs.push_back(Run{"felkover", true, false, {}});
        p.runs.push_back(Run{", ez ", false, false, {}});
        p.runs.push_back(Run{"dolt", false, true, {}});
        p.runs.push_back(Run{", ez pedig ", false, false, {}});
        p.runs.push_back(Run{"mindketto", true, true, {}});
        p.runs.push_back(Run{". Arvizturo tukorfurogep.", false, false, {}});
        doc.addParagraph(std::move(p));
    }

    {
        Paragraph p;
        p.runs.push_back(Run{"Szines szoveg: ", false, false, {}});
        p.runs.push_back(Run{"piros", false, false, "C0392B"});
        p.runs.push_back(Run{", ", false, false, {}});
        p.runs.push_back(Run{"kek", false, false, "2980B9"});
        p.runs.push_back(Run{", ", false, false, {}});
        p.runs.push_back(Run{"zold felkover", true, false, "27AE60"});
        p.runs.push_back(Run{".", false, false, {}});
        doc.addParagraph(std::move(p));
    }

    doc.addParagraph("Masodik szintu fejezet", ParagraphStyle::Heading2);
    for (int i = 0; i < 3; ++i) {
        Paragraph p = para("Felsorolas elem " + std::to_string(i + 1));
        p.list.kind = ListKind::Bullet;
        p.list.level = i == 2 ? 1 : 0;
        doc.addParagraph(std::move(p));
    }
    for (int i = 0; i < 2; ++i) {
        Paragraph p = para("Szamozott elem " + std::to_string(i + 1));
        p.list.kind = ListKind::Numbered;
        p.list.ordinal = i + 1;
        doc.addParagraph(std::move(p));
    }

    doc.addParagraph("Tablazat", ParagraphStyle::Heading3);
    {
        Block b;
        b.kind = Block::Kind::Table;
        b.table = std::make_shared<Table>();
        TableRow header;
        header.isHeader = true;
        header.cells = {cell("Targy"), cell("Nap"), cell("Terem")};
        b.table->rows.push_back(header);
        b.table->rows.push_back({{cell("Programozas"), cell("Hetfo"), cell("A/12")}, false});
        b.table->rows.push_back({{cell("Halozatok"), cell("Szerda"), cell("B/03")}, false});
        b.table->columnWidths = {3, 1.5, 1.5};
        doc.addBlock(std::move(b));
    }

    doc.addParagraph("Vege.", ParagraphStyle::Normal);

    writeDocx(argv[1], doc);
    std::cout << "OK -> " << argv[1] << "\n";
    return 0;
}
