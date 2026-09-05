// Direct smoke test for docx_writer's heading/title style output. Not
// exercised by cli_test.cpp, because the app's only real path that writes a
// .docx (PDF -> Word) always produces plain paragraphs — PDF text extraction
// has no concept of heading styles to recover (see README). This test
// exists so the DOCX style-writing code itself is still verified against a
// real Word-compatible reader (LibreOffice), independent of the app's
// current usage of it.
#include "../src/docx_writer.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output.docx>\n";
        return 2;
    }
    DocModel doc;
    doc.paragraphs.push_back({"Cim stilus teszt", ParagraphStyle::Title});
    doc.paragraphs.push_back({"Elso szintu fejezet", ParagraphStyle::Heading1});
    doc.paragraphs.push_back({"Ez egy sima bekezdes szoveg.", ParagraphStyle::Normal});
    doc.paragraphs.push_back({"Masodik szintu fejezet", ParagraphStyle::Heading2});
    doc.paragraphs.push_back({"Meg egy sima bekezdes.", ParagraphStyle::Normal});
    writeDocx(argv[1], doc);
    std::cout << "OK -> " << argv[1] << "\n";
    return 0;
}
