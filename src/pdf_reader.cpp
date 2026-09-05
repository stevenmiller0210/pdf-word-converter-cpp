#include "pdf_reader.h"
#include "process_util.h"
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace {

bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

std::string rstripCr(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\r')) --end;
    return s.substr(0, end);
}

} // namespace

DocModel readPdf(const std::string& path) {
    CommandResult res = runCommand({"pdftotext", "-layout", path, "-"});
    if (res.exitCode != 0) {
        throw std::runtime_error(
            "Nem sikerult megnyitni a PDF fajlt (nem valos vagy serult PDF).");
    }

    std::string text = res.stdoutData;
    // pdftotext separates pages with a form-feed; treat it as a plain line
    // break for our flat paragraph model (page boundaries aren't preserved).
    std::replace(text.begin(), text.end(), '\f', '\n');

    if (isBlank(text)) {
        throw std::runtime_error("NO_TEXT_EXTRACTED");
    }

    DocModel doc;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = rstripCr(line);
        if (isBlank(line)) continue;
        Paragraph p;
        p.text = line;
        p.style = ParagraphStyle::Normal;
        doc.paragraphs.push_back(std::move(p));
    }
    return doc;
}
