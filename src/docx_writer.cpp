#include "docx_writer.h"
#include "process_util.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string escapeXml(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Nem sikerult ideiglenes fajlt letrehozni: " + path.string());
    f << content;
}

const char* styleId(ParagraphStyle style) {
    switch (style) {
        case ParagraphStyle::Title: return "Title";
        case ParagraphStyle::Heading1: return "Heading1";
        case ParagraphStyle::Heading2: return "Heading2";
        case ParagraphStyle::Heading3: return "Heading3";
        default: return nullptr;
    }
}

std::string buildDocumentXml(const DocModel& doc) {
    std::string body;
    for (const auto& p : doc.paragraphs) {
        body += "<w:p>";
        if (const char* sid = styleId(p.style)) {
            body += "<w:pPr><w:pStyle w:val=\"";
            body += sid;
            body += "\"/></w:pPr>";
        }
        body += "<w:r><w:t xml:space=\"preserve\">";
        body += escapeXml(p.text);
        body += "</w:t></w:r></w:p>";
    }
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>" + body + "<w:sectPr/></w:body></w:document>";
}

const char* kContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
    "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
    "</Types>";

const char* kRootRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
    "</Relationships>";

const char* kDocumentRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "</Relationships>";

const char* kStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"56\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"28\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr></w:style>"
    "</w:styles>";

const char* kCoreProps =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
    "<dc:creator>pdf-word-converter-cpp</dc:creator><cp:revision>1</cp:revision>"
    "</cp:coreProperties>";

const char* kAppProps =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
    "<Application>pdf-word-converter-cpp</Application>"
    "</Properties>";

} // namespace

void writeDocx(const std::string& path, const DocModel& doc) {
    char tmpl[] = "/tmp/pdfwordconv_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) throw std::runtime_error("Nem sikerult ideiglenes konyvtarat letrehozni.");
    fs::path root(dir);

    try {
        writeFile(root / "[Content_Types].xml", kContentTypes);
        writeFile(root / "_rels" / ".rels", kRootRels);
        writeFile(root / "word" / "document.xml", buildDocumentXml(doc));
        writeFile(root / "word" / "styles.xml", kStyles);
        writeFile(root / "word" / "_rels" / "document.xml.rels", kDocumentRels);
        writeFile(root / "docProps" / "core.xml", kCoreProps);
        writeFile(root / "docProps" / "app.xml", kAppProps);

        fs::path absOutput = fs::absolute(path);
        std::error_code ec;
        fs::remove(absOutput, ec); // zip would otherwise update an existing archive in place

        std::string cwd = root.string();
        CommandResult res = runCommand(
            {"zip", "-X", "-r", absOutput.string(), "."}, &cwd);
        if (res.exitCode != 0 || !fs::exists(absOutput)) {
            throw std::runtime_error("A `zip` parancs nem tudta osszecsomagolni a .docx fajlt.");
        }
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
