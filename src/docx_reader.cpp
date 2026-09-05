#include "docx_reader.h"
#include "xml_lite.h"
#include "process_util.h"
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace {

// Walks a <w:p> paragraph's children in document order, concatenating run
// text (<w:t>), and approximating <w:tab/> as a tab character and
// <w:br/>/<w:cr/> as a space (so words on either side of a manual line
// break don't get glued together). This intentionally does not attempt to
// reproduce exact Word line breaks — see doc_model.h.
void extractRunText(const xmllite::Node& node, std::string& out) {
    if (node.tag == "w:t") {
        out += node.allText();
        return;
    }
    if (node.tag == "w:tab") { out += '\t'; return; }
    if (node.tag == "w:br" || node.tag == "w:cr") { out += ' '; return; }
    for (const auto& child : node.children) extractRunText(child, out);
}

std::string paragraphText(const xmllite::Node& p) {
    std::string text;
    extractRunText(p, text);
    return text;
}

ParagraphStyle styleFromId(const std::string& styleId) {
    if (styleId == "Title") return ParagraphStyle::Title;
    if (styleId == "Heading1") return ParagraphStyle::Heading1;
    if (styleId == "Heading2") return ParagraphStyle::Heading2;
    if (styleId == "Heading3") return ParagraphStyle::Heading3;
    return ParagraphStyle::Normal;
}

ParagraphStyle paragraphStyle(const xmllite::Node& p) {
    for (const auto& child : p.children) {
        if (child.tag != "w:pPr") continue;
        for (const auto& pprChild : child.children) {
            if (pprChild.tag == "w:pStyle") {
                const std::string* val = pprChild.attr("w:val");
                if (val) return styleFromId(*val);
            }
        }
    }
    return ParagraphStyle::Normal;
}

} // namespace

DocModel readDocx(const std::string& path) {
    CommandResult res = runCommand({"unzip", "-p", path, "word/document.xml"});
    if (res.exitCode != 0 || res.stdoutData.empty()) {
        throw std::runtime_error(
            "Nem sikerult megnyitni a .docx fajlt (nem valos vagy serult DOCX csomag).");
    }

    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(res.stdoutData);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Hibas a dokumentum XML tartalma: ") + e.what());
    }

    const xmllite::Node* documentRoot = nullptr;
    for (const auto& r : roots) {
        if (r.tag == "w:document") { documentRoot = &r; break; }
    }
    if (!documentRoot) {
        throw std::runtime_error("A document.xml nem tartalmaz varhato <w:document> gyokeret.");
    }

    std::vector<const xmllite::Node*> paragraphNodes;
    xmllite::findAll(*documentRoot, "w:p", paragraphNodes);

    DocModel doc;
    doc.paragraphs.reserve(paragraphNodes.size());
    for (const auto* p : paragraphNodes) {
        std::string text = paragraphText(*p);
        // Skip paragraphs with no actual text. This matters a lot in
        // practice: tables aren't understood as tables at all (this reader
        // has no concept of rows/columns), so every empty table cell shows
        // up as its own <w:p> with no text — without this filter, a table
        // with several empty cells turns into a run of blank output lines
        // that reads as a broken dead zone rather than an (expected,
        // documented) lack of table support.
        bool isBlank = std::all_of(text.begin(), text.end(),
                                    [](unsigned char c) { return std::isspace(c); });
        if (isBlank) continue;
        Paragraph para;
        para.text = std::move(text);
        para.style = paragraphStyle(*p);
        doc.paragraphs.push_back(std::move(para));
    }
    return doc;
}
