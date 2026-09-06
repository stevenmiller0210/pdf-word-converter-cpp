#include "docx_reader.h"
#include "xml_lite.h"
#include "process_util.h"
#include "image_codec.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {

// EMU (English Metric Units) are OOXML's internal length unit: 914400 to the
// inch, and a PDF point is 1/72 inch.
constexpr double EMU_PER_POINT = 12700.0;

bool onFlag(const xmllite::Node& n) {
    // <w:b/> means on; <w:b w:val="0"/> means explicitly off. Word writes
    // both, and treating the second as "on" makes every un-bolded run inside
    // a bold style come out bold.
    const std::string* v = n.attr("w:val");
    if (!v) return true;
    return !(*v == "0" || *v == "false" || *v == "off");
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isBlankText(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

// w:color carries bare hex, or the literal "auto" meaning "whatever the
// reader thinks is readable" — which is not a colour we can carry anywhere.
std::string normaliseColor(const std::string& raw) {
    std::string hex;
    for (char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (hex.size() != 6 || hex == "000000") return {};
    return hex;
}

ParagraphStyle styleFromId(const std::string& styleId) {
    std::string id = toLower(styleId);
    // Word writes "Heading1"; LibreOffice writes "Heading_20_1". Normalising
    // to lowercase and stripping separators covers both without a lookup
    // table per producer.
    std::string norm;
    for (char c : id)
        if (std::isalnum(static_cast<unsigned char>(c))) norm += c;
    if (norm == "title") return ParagraphStyle::Title;
    if (norm == "subtitle") return ParagraphStyle::Heading2;
    if (norm == "heading1" || norm == "heading201") return ParagraphStyle::Heading1;
    if (norm == "heading2" || norm == "heading202") return ParagraphStyle::Heading2;
    if (norm == "heading3" || norm == "heading203") return ParagraphStyle::Heading3;
    return ParagraphStyle::Normal;
}

const xmllite::Node* child(const xmllite::Node& n, const std::string& tag) {
    for (const auto& c : n.children)
        if (c.tag == tag) return &c;
    return nullptr;
}

int intAttr(const xmllite::Node& n, const std::string& name, int fallback) {
    const std::string* v = n.attr(name);
    if (!v) return fallback;
    try {
        return std::stoi(*v);
    } catch (...) {
        return fallback;
    }
}

// ---------- package access --------------------------------------------------

// One unzip process per part. Slower than opening the archive once, but this
// tool already shells out to unzip for document.xml and a real zip reader is
// a lot of code to own for three or four small parts.
struct Package {
    std::string path;

    std::string part(const std::string& name) const {
        CommandResult res = runCommand({"unzip", "-p", path, name});
        if (res.exitCode != 0) return {};
        return res.stdoutData;
    }
};

// ---------- numbering -------------------------------------------------------

// numId -> level -> is this level numbered (rather than bulleted)?
struct Numbering {
    std::map<int, std::map<int, bool>> numbered;

    bool isNumbered(int numId, int level) const {
        auto n = numbered.find(numId);
        if (n == numbered.end()) return false;
        auto l = n->second.find(level);
        if (l == n->second.end()) return false;
        return l->second;
    }
    bool known(int numId) const { return numbering_known.count(numId) > 0; }
    std::set<int> numbering_known;
};

Numbering readNumbering(const Package& pkg) {
    Numbering out;
    std::string xml = pkg.part("word/numbering.xml");
    if (xml.empty()) return out;

    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xml);
    } catch (...) {
        return out; // a list that renders as bullets beats failing the file
    }
    if (roots.empty()) return out;

    // abstractNumId -> level -> numbered?
    std::map<int, std::map<int, bool>> abstracts;
    std::vector<const xmllite::Node*> absNodes;
    xmllite::findAll(roots[0], "w:abstractNum", absNodes);
    for (const auto* a : absNodes) {
        int absId = intAttr(*a, "w:abstractNumId", -1);
        if (absId < 0) continue;
        std::vector<const xmllite::Node*> lvls;
        xmllite::findAll(*a, "w:lvl", lvls);
        for (const auto* l : lvls) {
            int ilvl = intAttr(*l, "w:ilvl", 0);
            const xmllite::Node* fmt = child(*l, "w:numFmt");
            std::string val = fmt && fmt->attr("w:val") ? *fmt->attr("w:val") : "bullet";
            abstracts[absId][ilvl] = (val != "bullet" && val != "none");
        }
    }

    std::vector<const xmllite::Node*> numNodes;
    xmllite::findAll(roots[0], "w:num", numNodes);
    for (const auto* n : numNodes) {
        int numId = intAttr(*n, "w:numId", -1);
        if (numId < 0) continue;
        const xmllite::Node* ref = child(*n, "w:abstractNumId");
        int absId = ref ? intAttr(*ref, "w:val", -1) : -1;
        out.numbering_known.insert(numId);
        auto it = abstracts.find(absId);
        if (it != abstracts.end()) out.numbered[numId] = it->second;
    }
    return out;
}

// ---------- relationships ---------------------------------------------------

std::map<std::string, std::string> readRels(const Package& pkg) {
    std::map<std::string, std::string> out;
    std::string xml = pkg.part("word/_rels/document.xml.rels");
    if (xml.empty()) return out;
    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xml);
    } catch (...) {
        return out;
    }
    if (roots.empty()) return out;
    std::vector<const xmllite::Node*> rels;
    xmllite::findAll(roots[0], "Relationship", rels);
    for (const auto* r : rels) {
        const std::string* id = r->attr("Id");
        const std::string* target = r->attr("Target");
        if (id && target) out[*id] = *target;
    }
    return out;
}

std::string mimeForTarget(const std::string& target) {
    std::string t = toLower(target);
    auto ends = [&](const char* suffix) {
        size_t n = std::string(suffix).size();
        return t.size() >= n && t.compare(t.size() - n, n, suffix) == 0;
    };
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".gif")) return "image/gif";
    if (ends(".bmp")) return "image/bmp";
    if (ends(".tif") || ends(".tiff")) return "image/tiff";
    if (ends(".emf")) return "image/x-emf";
    if (ends(".wmf")) return "image/x-wmf";
    return "application/octet-stream";
}

// ---------- the reader itself ----------------------------------------------

class Reader {
public:
    Reader(const Package& pkg, const Numbering& numbering,
           const std::map<std::string, std::string>& rels)
        : pkg_(pkg), numbering_(numbering), rels_(rels) {}

    void readBody(const xmllite::Node& body, std::vector<Block>& out) {
        for (const auto& node : body.children) {
            if (node.tag == "w:p") {
                readParagraph(node, out);
            } else if (node.tag == "w:tbl") {
                Block b;
                b.kind = Block::Kind::Table;
                b.table = std::make_shared<Table>();
                readTable(node, *b.table);
                if (!b.table->rows.empty()) out.push_back(std::move(b));
            }
            // w:sectPr, w:bookmarkStart, ... carry no content we can render.
        }
    }

private:
    // A paragraph can produce more than one block: Word puts inline pictures
    // inside a run, so "text, picture, more text" is one <w:p> that has to
    // come out as three blocks in the right order.
    void readParagraph(const xmllite::Node& p, std::vector<Block>& out) {
        Paragraph para;
        para.style = paragraphStyle(p);
        para.alignment = paragraphAlignment(p);
        applyListInfo(p, para);

        std::vector<Block> pending;
        collectRuns(p, para, pending, defaultBold(para.style));

        if (!para.empty()) {
            Block b;
            b.kind = Block::Kind::Paragraph;
            b.paragraph = std::move(para);
            out.push_back(std::move(b));
        }
        for (auto& b : pending) out.push_back(std::move(b));
    }

    static bool defaultBold(ParagraphStyle s) { return s != ParagraphStyle::Normal; }

    ParagraphStyle paragraphStyle(const xmllite::Node& p) {
        const xmllite::Node* ppr = child(p, "w:pPr");
        if (!ppr) return ParagraphStyle::Normal;
        const xmllite::Node* st = child(*ppr, "w:pStyle");
        if (st && st->attr("w:val")) return styleFromId(*st->attr("w:val"));
        return ParagraphStyle::Normal;
    }

    Alignment paragraphAlignment(const xmllite::Node& p) {
        const xmllite::Node* ppr = child(p, "w:pPr");
        if (!ppr) return Alignment::Default;
        const xmllite::Node* jc = child(*ppr, "w:jc");
        if (!jc || !jc->attr("w:val")) return Alignment::Default;
        const std::string& v = *jc->attr("w:val");
        if (v == "center") return Alignment::Center;
        if (v == "right" || v == "end") return Alignment::Right;
        if (v == "both" || v == "distribute") return Alignment::Justify;
        if (v == "left" || v == "start") return Alignment::Left;
        return Alignment::Default;
    }

    void applyListInfo(const xmllite::Node& p, Paragraph& para) {
        const xmllite::Node* ppr = child(p, "w:pPr");
        if (!ppr) return;
        const xmllite::Node* numPr = child(*ppr, "w:numPr");
        if (!numPr) return;
        const xmllite::Node* ilvlNode = child(*numPr, "w:ilvl");
        const xmllite::Node* numIdNode = child(*numPr, "w:numId");
        int level = ilvlNode ? intAttr(*ilvlNode, "w:val", 0) : 0;
        int numId = numIdNode ? intAttr(*numIdNode, "w:val", 0) : 0;
        if (numId <= 0) return;

        para.list.level = std::max(0, level);
        para.list.kind = numbering_.isNumbered(numId, para.list.level) ? ListKind::Numbered
                                                                       : ListKind::Bullet;
        if (para.list.kind == ListKind::Numbered) {
            auto key = std::make_pair(numId, para.list.level);
            para.list.ordinal = ++counters_[key];
            // Starting a new item at this level restarts everything nested
            // under it, which is what Word does and what makes 1.1 / 1.2
            // come out right after a new 2.
            for (auto it = counters_.begin(); it != counters_.end();) {
                if (it->first.first == numId && it->first.second > para.list.level)
                    it = counters_.erase(it);
                else
                    ++it;
            }
        }
    }

    // Walks a paragraph's children in document order, so runs, hyperlinks and
    // inline pictures keep their relative positions.
    void collectRuns(const xmllite::Node& node, Paragraph& para, std::vector<Block>& pending,
                     bool inheritedBold, bool inheritedItalic = false,
                     const std::string& inheritedColor = std::string(),
                     const std::string& inheritedFont = std::string()) {
        for (const auto& c : node.children) {
            if (c.tag == "w:pPr") continue;
            if (c.tag == "w:r") {
                bool bold = inheritedBold, italic = inheritedItalic;
                std::string color = inheritedColor;
                std::string font = inheritedFont;
                const xmllite::Node* rpr = child(c, "w:rPr");
                if (rpr) {
                    if (const xmllite::Node* b = child(*rpr, "w:b")) bold = onFlag(*b);
                    if (const xmllite::Node* i = child(*rpr, "w:i")) italic = onFlag(*i);
                    if (const xmllite::Node* col = child(*rpr, "w:color"))
                        if (const std::string* v = col->attr("w:val")) color = normaliseColor(*v);
                    if (const xmllite::Node* rf = child(*rpr, "w:rFonts"))
                        if (const std::string* v = rf->attr("w:ascii")) font = *v;
                }
                appendRunContent(c, para, pending, bold, italic, color, font);
            } else if (c.tag == "w:hyperlink" || c.tag == "w:smartTag" || c.tag == "w:ins" ||
                       c.tag == "w:sdt" || c.tag == "w:sdtContent" || c.tag == "w:bdo") {
                collectRuns(c, para, pending, inheritedBold, inheritedItalic, inheritedColor,
                           inheritedFont);
            } else if (c.tag == "w:del") {
                // Tracked deletions are not part of the document's text.
                continue;
            }
        }
    }

    void appendRunContent(const xmllite::Node& run, Paragraph& para, std::vector<Block>& pending,
                          bool bold, bool italic, const std::string& color,
                          const std::string& font) {
        for (const auto& c : run.children) {
            if (c.tag == "w:t") {
                addText(para, c.allText(), bold, italic, color, font);
            } else if (c.tag == "w:tab") {
                addText(para, "\t", bold, italic, color, font);
            } else if (c.tag == "w:br" || c.tag == "w:cr") {
                addText(para, " ", bold, italic, color, font);
            } else if (c.tag == "w:noBreakHyphen") {
                addText(para, "-", bold, italic, color, font);
            } else if (c.tag == "w:sym") {
                // A symbol-font character has no Unicode meaning we can
                // recover reliably; a space keeps the surrounding words apart.
                addText(para, " ", bold, italic, color, font);
            } else if (c.tag == "w:drawing" || c.tag == "w:pict" || c.tag == "w:object") {
                readDrawing(c, pending);
            }
        }
    }

    void addText(Paragraph& para, const std::string& text, bool bold, bool italic,
                 const std::string& color, const std::string& font) {
        if (text.empty()) return;
        Run candidate{text, bold, italic, font, color};
        if (!para.runs.empty() && para.runs.back().sameStyle(candidate)) {
            para.runs.back().text += text;
        } else {
            para.runs.push_back(std::move(candidate));
        }
    }

    void readDrawing(const xmllite::Node& drawing, std::vector<Block>& pending) {
        std::string relId;
        std::vector<const xmllite::Node*> blips;
        xmllite::findAll(drawing, "a:blip", blips);
        for (const auto* b : blips) {
            if (const std::string* e = b->attr("r:embed")) { relId = *e; break; }
            if (const std::string* l = b->attr("r:link")) { relId = *l; break; }
        }
        if (relId.empty()) {
            std::vector<const xmllite::Node*> vml;
            xmllite::findAll(drawing, "v:imagedata", vml);
            for (const auto* v : vml)
                if (const std::string* id = v->attr("r:id")) { relId = *id; break; }
        }
        if (relId.empty()) return;

        auto rel = rels_.find(relId);
        if (rel == rels_.end()) return;
        std::string target = rel->second;
        if (target.rfind("../", 0) == 0) target = target.substr(3);
        if (target.rfind("/", 0) == 0) target = target.substr(1);
        else if (target.rfind("word/", 0) != 0) target = "word/" + target;

        auto cached = mediaCache_.find(target);
        std::string bytes;
        if (cached != mediaCache_.end()) {
            bytes = cached->second;
        } else {
            bytes = pkg_.part(target);
            mediaCache_[target] = bytes;
        }
        if (bytes.empty()) return;

        auto img = std::make_shared<Image>();
        img->bytes = std::move(bytes);
        img->mime = mimeForTarget(target);
        probeImageSize(img->bytes, img->mime, img->pixelWidth, img->pixelHeight);

        // wp:extent is the size Word actually displays the picture at, which
        // is usually not its pixel size; honouring it keeps a deliberately
        // shrunk photo shrunk.
        std::vector<const xmllite::Node*> extents;
        xmllite::findAll(drawing, "wp:extent", extents);
        if (extents.empty()) xmllite::findAll(drawing, "a:ext", extents);
        if (!extents.empty()) {
            double cx = intAttr(*extents[0], "cx", 0);
            double cy = intAttr(*extents[0], "cy", 0);
            if (cx > 0 && cy > 0) {
                img->displayWidthPt = cx / EMU_PER_POINT;
                img->displayHeightPt = cy / EMU_PER_POINT;
            }
        }

        Block b;
        b.kind = Block::Kind::Image;
        b.image = img;
        pending.push_back(std::move(b));
    }

    void readTable(const xmllite::Node& tbl, Table& table) {
        if (const xmllite::Node* grid = child(tbl, "w:tblGrid")) {
            for (const auto& col : grid->children) {
                if (col.tag != "w:gridCol") continue;
                table.columnWidths.push_back(std::max(1, intAttr(col, "w:w", 1)));
            }
        }

        // Column position of each cell, tracked alongside table.rows so a
        // later pass can collapse w:vMerge continuation cells into the
        // "restart" cell's rowSpan — Word represents a vertically merged
        // cell as one real cell plus a same-column placeholder in every row
        // it covers, and doc_model.h wants that folded into a single
        // TableCell instead.
        std::vector<std::vector<int>> colOf;

        for (const auto& tr : tbl.children) {
            if (tr.tag != "w:tr") continue;
            TableRow row;
            std::vector<int> rowColOf;
            int nextCol = 0;
            if (const xmllite::Node* trPr = child(tr, "w:trPr"))
                row.isHeader = child(*trPr, "w:tblHeader") != nullptr;

            for (const auto& tc : tr.children) {
                if (tc.tag != "w:tc") continue;
                TableCell cell;
                if (const xmllite::Node* tcPr = child(tc, "w:tcPr")) {
                    if (const xmllite::Node* span = child(*tcPr, "w:gridSpan"))
                        cell.gridSpan = std::max(1, intAttr(*span, "w:val", 1));
                    if (const xmllite::Node* shd = child(*tcPr, "w:shd"))
                        if (const std::string* fill = shd->attr("w:fill"))
                            cell.shading = normaliseColor(*fill);
                    if (const xmllite::Node* borders = child(*tcPr, "w:tcBorders")) {
                        auto present = [&](const char* tag) {
                            const xmllite::Node* edge = child(*borders, tag);
                            if (!edge) return true; // unspecified defaults to the table border
                            const std::string* v = edge->attr("w:val");
                            return !(v && *v == "nil");
                        };
                        cell.borders.top = present("w:top");
                        cell.borders.bottom = present("w:bottom");
                        cell.borders.left = present("w:left");
                        cell.borders.right = present("w:right");
                    }
                    if (const xmllite::Node* vMerge = child(*tcPr, "w:vMerge")) {
                        const std::string* v = vMerge->attr("w:val");
                        cell.verticallyMerged = !(v && *v == "restart");
                    }
                }
                for (const auto& inner : tc.children) {
                    if (inner.tag == "w:p") {
                        readParagraph(inner, cell.blocks);
                    } else if (inner.tag == "w:tbl") {
                        Block b;
                        b.kind = Block::Kind::Table;
                        b.table = std::make_shared<Table>();
                        readTable(inner, *b.table);
                        if (!b.table->rows.empty()) cell.blocks.push_back(std::move(b));
                    }
                }
                rowColOf.push_back(nextCol);
                nextCol += cell.gridSpan;
                row.cells.push_back(std::move(cell));
            }
            if (!row.cells.empty()) {
                table.rows.push_back(std::move(row));
                colOf.push_back(std::move(rowColOf));
            }
        }

        // Fold each vMerge run's row count onto the cell that started it.
        // The continuation cells themselves stay in place, emptied out —
        // both writers expect every row to carry a full set of grid
        // columns, a placeholder included, the same shape a ruled PDF table
        // produces; removing them here would leave later rows short a
        // column with nothing to say why.
        std::map<int, std::pair<size_t, size_t>> openOrigin; // column -> (row, cell)
        for (size_t r = 0; r < table.rows.size(); ++r) {
            for (size_t ci = 0; ci < table.rows[r].cells.size(); ++ci) {
                int col = colOf[r][ci];
                TableCell& cell = table.rows[r].cells[ci];
                if (cell.verticallyMerged) {
                    auto it = openOrigin.find(col);
                    if (it != openOrigin.end()) {
                        table.rows[it->second.first].cells[it->second.second].rowSpan++;
                        cell.blocks.clear();
                        continue;
                    }
                    // No matching restart cell above: treat it as an
                    // ordinary cell rather than silently dropping it.
                    cell.verticallyMerged = false;
                }
                openOrigin[col] = {r, ci};
            }
        }

        // The grid and the actual cells disagree in plenty of real documents
        // (merged cells, stale grids). An unusable grid is worse than none,
        // because the writer would size columns against the wrong total.
        size_t widest = 0;
        for (const auto& r : table.rows) {
            size_t n = 0;
            for (const auto& c : r.cells) n += static_cast<size_t>(std::max(1, c.gridSpan));
            widest = std::max(widest, n);
        }
        if (table.columnWidths.size() != widest) table.columnWidths.clear();
    }

    const Package& pkg_;
    const Numbering& numbering_;
    const std::map<std::string, std::string>& rels_;
    std::map<std::pair<int, int>, int> counters_;
    std::map<std::string, std::string> mediaCache_;
};

} // namespace

// Reads a header/footer part (word/header1.xml, word/footer1.xml, ...):
// same run/paragraph shape as the body, just under a different root
// element, so it goes through the same Reader.
std::vector<Paragraph> readRunningPart(Reader& reader, const Package& pkg,
                                       const std::string& target) {
    std::string xml = pkg.part(target);
    if (xml.empty()) return {};
    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xml);
    } catch (...) {
        return {};
    }
    if (roots.empty()) return {};

    std::vector<Block> blocks;
    reader.readBody(roots[0], blocks);
    std::vector<Paragraph> paragraphs;
    for (auto& b : blocks)
        if (b.kind == Block::Kind::Paragraph) paragraphs.push_back(std::move(b.paragraph));
    return paragraphs;
}

DocModel readDocx(const std::string& path) {
    Package pkg{path};
    std::string xml = pkg.part("word/document.xml");
    if (xml.empty()) {
        throw std::runtime_error(
            "Nem sikerult megnyitni a .docx fajlt (nem valos vagy serult DOCX csomag).");
    }

    std::vector<xmllite::Node> roots;
    try {
        roots = xmllite::parse(xml);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Hibas a dokumentum XML tartalma: ") + e.what());
    }

    const xmllite::Node* documentRoot = nullptr;
    for (const auto& r : roots)
        if (r.tag == "w:document") { documentRoot = &r; break; }
    if (!documentRoot)
        throw std::runtime_error("A document.xml nem tartalmaz varhato <w:document> gyokeret.");

    const xmllite::Node* body = child(*documentRoot, "w:body");
    if (!body) body = documentRoot;

    Numbering numbering = readNumbering(pkg);
    std::map<std::string, std::string> rels = readRels(pkg);

    Reader reader(pkg, numbering, rels);
    DocModel doc;
    reader.readBody(*body, doc.blocks);

    // A section can reference a header/footer for its first/even/default
    // variant; only "default" is worth recovering into a flat model that
    // has no notion of odd/even pages to begin with.
    std::vector<const xmllite::Node*> sectPrs;
    xmllite::findAll(*documentRoot, "w:sectPr", sectPrs);
    for (const auto* sectPr : sectPrs) {
        for (const auto& c : sectPr->children) {
            bool isHeader = c.tag == "w:headerReference";
            bool isFooter = c.tag == "w:footerReference";
            if (!isHeader && !isFooter) continue;
            const std::string* type = c.attr("w:type");
            if (type && *type != "default") continue;
            const std::string* rid = c.attr("r:id");
            if (!rid) continue;
            auto rel = rels.find(*rid);
            if (rel == rels.end()) continue;
            std::string target = rel->second;
            if (target.rfind("word/", 0) != 0) target = "word/" + target;
            std::vector<Paragraph> paragraphs = readRunningPart(reader, pkg, target);
            if (paragraphs.empty()) continue;
            if (isHeader && doc.header.empty()) doc.header = paragraphs;
            else if (isFooter && doc.footer.empty()) doc.footer = paragraphs;
        }
    }

    // Trailing empty paragraphs are an artefact of how Word ends a document,
    // not content; they would otherwise add a blank page at the bottom.
    while (!doc.blocks.empty() && doc.blocks.back().kind == Block::Kind::Paragraph &&
           isBlankText(doc.blocks.back().paragraph.text()))
        doc.blocks.pop_back();

    return doc;
}
