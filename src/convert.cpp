#include "convert.h"
#include "docx_reader.h"
#include "docx_writer.h"
#include "pdf_reader.h"
#include "pdf_writer.h"

void convertWordToPdf(const std::string& inputPath, const std::string& outputPath) {
    DocModel doc = readDocx(inputPath);
    writePdf(outputPath, doc);
}

void convertPdfToWord(const std::string& inputPath, const std::string& outputPath) {
    DocModel doc = readPdf(inputPath); // may throw "NO_TEXT_EXTRACTED"
    writeDocx(outputPath, doc);
}
