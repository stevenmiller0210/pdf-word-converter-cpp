#include "docx_writer.h"
#include "image_codec.h"
#include "process_util.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Twentieths of a point: OOXML's unit for widths and indents.
constexpr int CONTENT_WIDTH_DXA = 9360; // 6.5in, Letter/A4 with 1in margins
// EMU per point, for picture extents.
constexpr double EMU_PER_POINT = 12700.0;
constexpr double MAX_IMAGE_WIDTH_PT = 468.0; // 6.5in

// Strips what XML 1.0 cannot represent and repairs invalid UTF-8.
//
// This is not defensive tidiness: the previous version passed bytes straight
// through, so a single control character picked up from a PDF's text layer
// produced a .docx that Word and LibreOffice both refuse to open — and the
// converter still reported success, because `zip` had no opinion about the
// XML inside. A silently corrupt output file is the worst possible failure
// mode, so the sanitising happens here, at the one place every string passes
// through on its way into the package.
std::string escapeXml(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 8);

    auto emitReplacement = [&]() { out += "\xEF\xBF\xBD"; }; // U+FFFD

    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);

        if (c < 0x80) {
            ++i;
            if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) continue; // illegal in XML 1.0
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out += static_cast<char>(c);
            }
            continue;
        }

        int extra;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else { emitReplacement(); ++i; continue; }

        if (i + static_cast<size_t>(extra) >= in.size()) { emitReplacement(); break; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) { emitReplacement(); ++i; continue; }

        // Overlong forms, surrogates and out-of-range values are all invalid
        // UTF-8, and a surrogate is invalid in XML even when well-formed.
        bool valid = !(extra == 1 && cp < 0x80) && !(extra == 2 && cp < 0x800) &&
                     !(extra == 3 && cp < 0x10000) && !(cp >= 0xD800 && cp <= 0xDFFF) &&
                     cp <= 0x10FFFF && cp != 0xFFFE && cp != 0xFFFF;
        if (valid) out.append(in, i, static_cast<size_t>(extra) + 1);
        else emitReplacement();
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Nem sikerult ideiglenes fajlt letrehozni: " + path.string());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) throw std::runtime_error("Nem sikerult kiirni az ideiglenes fajlt: " + path.string());
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

std::string extensionFor(const std::string& mime) {
    if (mime == "image/png") return "png";
    if (mime == "image/jpeg") return "jpeg";
    if (mime == "image/gif") return "gif";
    if (mime == "image/bmp") return "bmp";
    if (mime == "image/tiff") return "tiff";
    return "";
}

struct MediaEntry {
    std::string relId;
    std::string fileName; // inside word/media/
    std::string bytes;
};

// Builds word/document.xml, collecting the pictures it references as it goes
// so the relationship part and the media files stay in step with the body.
class DocumentBuilder {
public:
    std::string build(const DocModel& doc) {
        std::string body;
        for (const auto& b : doc.blocks) body += block(b);
        if (body.empty()) body = "<w:p/>";

        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
               "<w:document "
               "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
               "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
               "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\">"
               "<w:body>" +
               body +
               "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
               "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\"/>"
               "</w:sectPr></w:body></w:document>";
    }

    const std::vector<MediaEntry>& media() const { return media_; }

private:
    std::string block(const Block& b) {
        switch (b.kind) {
            case Block::Kind::Paragraph: return paragraph(b.paragraph);
            case Block::Kind::Table: return b.table ? table(*b.table) : std::string();
            case Block::Kind::Image: return b.image ? imageParagraph(*b.image) : std::string();
        }
        return {};
    }

    std::string paragraph(const Paragraph& p) {
        std::string out = "<w:p>";
        std::string props;
        if (const char* sid = styleId(p.style)) {
            props += "<w:pStyle w:val=\"";
            props += sid;
            props += "\"/>";
        } else if (p.list.kind != ListKind::None) {
            props += "<w:pStyle w:val=\"ListParagraph\"/>";
        }
        if (p.list.kind != ListKind::None) {
            // Word renumbers list items itself; the reader's `ordinal` is
            // only used by the PDF writer, which has to draw the marker.
            props += "<w:numPr><w:ilvl w:val=\"" + std::to_string(std::min(p.list.level, 8)) +
                     "\"/><w:numId w:val=\"" +
                     (p.list.kind == ListKind::Numbered ? "2" : "1") + "\"/></w:numPr>";
        }
        if (!props.empty()) out += "<w:pPr>" + props + "</w:pPr>";

        for (const auto& r : p.runs) {
            if (r.text.empty()) continue;
            out += "<w:r>";
            if (r.bold || r.italic) {
                out += "<w:rPr>";
                if (r.bold) out += "<w:b/>";
                if (r.italic) out += "<w:i/>";
                out += "</w:rPr>";
            }
            out += "<w:t xml:space=\"preserve\">" + escapeXml(r.text) + "</w:t></w:r>";
        }
        out += "</w:p>";
        return out;
    }

    std::string table(const Table& t) {
        size_t cols = 0;
        for (const auto& row : t.rows) {
            size_t n = 0;
            for (const auto& c : row.cells) n += static_cast<size_t>(std::max(1, c.gridSpan));
            cols = std::max(cols, n);
        }
        if (cols == 0) return {};

        std::vector<int> widths(cols, CONTENT_WIDTH_DXA / static_cast<int>(cols));
        if (t.columnWidths.size() == cols) {
            double total = 0;
            for (double w : t.columnWidths) total += w;
            if (total > 0)
                for (size_t i = 0; i < cols; ++i)
                    widths[i] = static_cast<int>(CONTENT_WIDTH_DXA * t.columnWidths[i] / total);
        }

        std::string out =
            "<w:tbl><w:tblPr><w:tblW w:w=\"" + std::to_string(CONTENT_WIDTH_DXA) +
            "\" w:type=\"dxa\"/><w:tblBorders>"
            "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "<w:left w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "<w:right w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "<w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"999999\"/>"
            "</w:tblBorders></w:tblPr><w:tblGrid>";
        for (size_t i = 0; i < cols; ++i)
            out += "<w:gridCol w:w=\"" + std::to_string(std::max(1, widths[i])) + "\"/>";
        out += "</w:tblGrid>";

        for (const auto& row : t.rows) {
            out += "<w:tr>";
            if (row.isHeader) out += "<w:trPr><w:tblHeader/></w:trPr>";
            size_t col = 0;
            for (const auto& cell : row.cells) {
                int span = std::max(1, cell.gridSpan);
                int w = 0;
                for (int k = 0; k < span && col + static_cast<size_t>(k) < widths.size(); ++k)
                    w += widths[col + k];
                out += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(std::max(1, w)) +
                       "\" w:type=\"dxa\"/>";
                if (span > 1) out += "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>";
                out += "</w:tcPr>";

                std::string content;
                for (const auto& b : cell.blocks) content += block(b);
                // A cell with no <w:p> at all is invalid OOXML — Word rejects
                // the whole document, not just the cell.
                if (content.empty()) content = "<w:p/>";
                out += content + "</w:tc>";
                col += static_cast<size_t>(span);
            }
            out += "</w:tr>";
        }
        out += "</w:tbl>";
        // A table must be followed by a paragraph, or two adjacent tables
        // merge into one when the file is opened.
        out += "<w:p/>";
        return out;
    }

    std::string imageParagraph(const Image& img) {
        std::string ext = extensionFor(img.mime);
        if (ext.empty() || img.bytes.empty()) return {};

        int pw = img.pixelWidth, ph = img.pixelHeight;
        if (pw <= 0 || ph <= 0) probeImageSize(img.bytes, img.mime, pw, ph);

        double wPt = img.displayWidthPt > 0 ? img.displayWidthPt : pw * 0.75;
        double hPt = img.displayHeightPt > 0 ? img.displayHeightPt : ph * 0.75;
        if (wPt <= 0 || hPt <= 0) return {};
        if (wPt > MAX_IMAGE_WIDTH_PT) {
            hPt *= MAX_IMAGE_WIDTH_PT / wPt;
            wPt = MAX_IMAGE_WIDTH_PT;
        }

        int id = static_cast<int>(media_.size()) + 1;
        MediaEntry entry;
        entry.relId = "rIdImg" + std::to_string(id);
        entry.fileName = "image" + std::to_string(id) + "." + ext;
        entry.bytes = img.bytes;
        media_.push_back(entry);

        long cx = static_cast<long>(wPt * EMU_PER_POINT);
        long cy = static_cast<long>(hPt * EMU_PER_POINT);

        std::ostringstream s;
        s << "<w:p><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
          << "<wp:extent cx=\"" << cx << "\" cy=\"" << cy << "\"/>"
          << "<wp:docPr id=\"" << id << "\" name=\"Kep " << id << "\"/>"
          << "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
          << "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
          << "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
          << "<pic:nvPicPr><pic:cNvPr id=\"" << id << "\" name=\"" << entry.fileName << "\"/>"
          << "<pic:cNvPicPr/></pic:nvPicPr>"
          << "<pic:blipFill><a:blip r:embed=\"" << entry.relId << "\"/>"
          << "<a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
          << "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" << cx << "\" cy=\"" << cy
          << "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
          << "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>";
        return s.str();
    }

    std::vector<MediaEntry> media_;
};

std::string contentTypes(const std::vector<MediaEntry>& media) {
    std::string defaults;
    bool png = false, jpeg = false, gif = false, bmp = false, tiff = false;
    for (const auto& m : media) {
        if (m.fileName.find(".png") != std::string::npos) png = true;
        else if (m.fileName.find(".jpeg") != std::string::npos) jpeg = true;
        else if (m.fileName.find(".gif") != std::string::npos) gif = true;
        else if (m.fileName.find(".bmp") != std::string::npos) bmp = true;
        else if (m.fileName.find(".tiff") != std::string::npos) tiff = true;
    }
    if (png) defaults += "<Default Extension=\"png\" ContentType=\"image/png\"/>";
    if (jpeg) defaults += "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>";
    if (gif) defaults += "<Default Extension=\"gif\" ContentType=\"image/gif\"/>";
    if (bmp) defaults += "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>";
    if (tiff) defaults += "<Default Extension=\"tiff\" ContentType=\"image/tiff\"/>";

    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
           "<Default Extension=\"rels\" "
           "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" +
           defaults +
           "<Override PartName=\"/word/document.xml\" "
           "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
           "<Override PartName=\"/word/styles.xml\" "
           "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
           "<Override PartName=\"/word/numbering.xml\" "
           "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
           "<Override PartName=\"/docProps/core.xml\" "
           "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
           "<Override PartName=\"/docProps/app.xml\" "
           "ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
           "</Types>";
}

std::string documentRels(const std::vector<MediaEntry>& media) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
        "Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" "
        "Target=\"numbering.xml\"/>";
    for (const auto& m : media) {
        out += "<Relationship Id=\"" + m.relId +
               "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
               "image\" Target=\"media/" + m.fileName + "\"/>";
    }
    out += "</Relationships>";
    return out;
}

// Two abstract lists — one bulleted, one decimal — with nine levels each, so
// any nesting depth the reader produced has somewhere to land.
std::string numberingXml() {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
    const char* bullets[3] = {"\xEF\x82\xB7", "o", "\xEF\x82\xA7"};
    for (int abs = 0; abs < 2; ++abs) {
        out += "<w:abstractNum w:abstractNumId=\"" + std::to_string(abs) + "\">";
        for (int lvl = 0; lvl < 9; ++lvl) {
            int indent = 720 * (lvl + 1);
            out += "<w:lvl w:ilvl=\"" + std::to_string(lvl) + "\"><w:start w:val=\"1\"/>";
            if (abs == 0) {
                out += "<w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"";
                out += bullets[lvl % 3];
                out += "\"/>";
            } else {
                out += "<w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%" +
                       std::to_string(lvl + 1) + ".\"/>";
            }
            out += "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(indent) +
                   "\" w:hanging=\"360\"/></w:pPr>";
            if (abs == 0)
                out += "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\" "
                       "w:hint=\"default\"/></w:rPr>";
            out += "</w:lvl>";
        }
        out += "</w:abstractNum>";
    }
    out += "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
           "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
           "</w:numbering>";
    return out;
}

const char* kRootRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
    "</Relationships>";

const char* kStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"56\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"28\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/></w:style>"
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
        DocumentBuilder builder;
        std::string documentXml = builder.build(doc);
        const std::vector<MediaEntry>& media = builder.media();

        writeFile(root / "[Content_Types].xml", contentTypes(media));
        writeFile(root / "_rels" / ".rels", kRootRels);
        writeFile(root / "word" / "document.xml", documentXml);
        writeFile(root / "word" / "styles.xml", kStyles);
        writeFile(root / "word" / "numbering.xml", numberingXml());
        writeFile(root / "word" / "_rels" / "document.xml.rels", documentRels(media));
        writeFile(root / "docProps" / "core.xml", kCoreProps);
        writeFile(root / "docProps" / "app.xml", kAppProps);
        for (const auto& m : media) writeFile(root / "word" / "media" / m.fileName, m.bytes);

        fs::path absOutput = fs::absolute(path);
        std::error_code ec;
        fs::remove(absOutput, ec); // zip would otherwise update an existing archive in place

        std::string cwd = root.string();
        CommandResult res = runCommand({"zip", "-X", "-r", absOutput.string(), "."}, &cwd);
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
