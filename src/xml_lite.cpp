#include "xml_lite.h"
#include <stdexcept>
#include <cctype>

namespace xmllite {

std::string unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '&') {
            size_t semi = in.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = in.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out += '&'; i = semi; continue; }
                if (ent == "lt") { out += '<'; i = semi; continue; }
                if (ent == "gt") { out += '>'; i = semi; continue; }
                if (ent == "quot") { out += '"'; i = semi; continue; }
                if (ent == "apos") { out += '\''; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    long code = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                        code = strtol(ent.c_str() + 2, nullptr, 16);
                    else
                        code = strtol(ent.c_str() + 1, nullptr, 10);
                    // Re-encode the code point as UTF-8.
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else if (code < 0x10000) {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (code >> 18));
                        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    i = semi;
                    continue;
                }
            }
        }
        out += in[i];
    }
    return out;
}

namespace {

struct Parser {
    const std::string& s;
    size_t pos = 0;
    explicit Parser(const std::string& src) : s(src) {}

    bool eof() const { return pos >= s.size(); }
    char peek() const { return s[pos]; }

    void skipWs() {
        while (!eof() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }

    // Skips <?xml ... ?>, <!-- ... -->, <!DOCTYPE ...> as siblings, in any order.
    void skipMisc() {
        for (;;) {
            skipWs();
            if (s.compare(pos, 2, "<?") == 0) {
                size_t end = s.find("?>", pos);
                if (end == std::string::npos) throw std::runtime_error("unterminated <? ... ?>");
                pos = end + 2;
            } else if (s.compare(pos, 4, "<!--") == 0) {
                size_t end = s.find("-->", pos);
                if (end == std::string::npos) throw std::runtime_error("unterminated comment");
                pos = end + 3;
            } else if (s.compare(pos, 2, "<!") == 0) {
                size_t end = s.find('>', pos);
                if (end == std::string::npos) throw std::runtime_error("unterminated <! ... >");
                pos = end + 1;
            } else {
                break;
            }
        }
    }

    std::string parseName() {
        size_t start = pos;
        while (!eof() && s[pos] != '>' && s[pos] != '/' && s[pos] != ' ' &&
               s[pos] != '\t' && s[pos] != '\n' && s[pos] != '\r')
            ++pos;
        return s.substr(start, pos - start);
    }

    std::vector<std::pair<std::string, std::string>> parseAttrs() {
        std::vector<std::pair<std::string, std::string>> attrs;
        for (;;) {
            skipWs();
            if (eof() || s[pos] == '>' || s[pos] == '/') break;
            size_t nameStart = pos;
            while (!eof() && s[pos] != '=' && !std::isspace(static_cast<unsigned char>(s[pos])))
                ++pos;
            std::string name = s.substr(nameStart, pos - nameStart);
            skipWs();
            std::string value;
            if (!eof() && s[pos] == '=') {
                ++pos;
                skipWs();
                if (!eof() && (s[pos] == '"' || s[pos] == '\'')) {
                    char quote = s[pos++];
                    size_t valStart = pos;
                    while (!eof() && s[pos] != quote) ++pos;
                    value = unescape(s.substr(valStart, pos - valStart));
                    if (!eof()) ++pos; // closing quote
                }
            }
            attrs.emplace_back(name, value);
        }
        return attrs;
    }

    Node parseElement() {
        // assumes s[pos] == '<' and not a comment/pi/doctype
        ++pos; // consume '<'
        Node node;
        node.tag = parseName();
        node.attrs = parseAttrs();
        skipWs();
        if (!eof() && s[pos] == '/') {
            // self-closing
            ++pos;
            if (!eof() && s[pos] == '>') ++pos;
            return node;
        }
        if (!eof() && s[pos] == '>') ++pos;

        // children until matching close tag
        for (;;) {
            if (eof()) throw std::runtime_error("unexpected end of XML inside <" + node.tag + ">");
            if (s.compare(pos, 4, "<!--") == 0) {
                size_t end = s.find("-->", pos);
                if (end == std::string::npos) throw std::runtime_error("unterminated comment");
                pos = end + 3;
                continue;
            }
            if (s[pos] == '<' && pos + 1 < s.size() && s[pos + 1] == '/') {
                size_t end = s.find('>', pos);
                if (end == std::string::npos) throw std::runtime_error("unterminated closing tag");
                pos = end + 1;
                break;
            }
            if (s[pos] == '<') {
                node.children.push_back(parseElement());
            } else {
                size_t textStart = pos;
                while (!eof() && s[pos] != '<') ++pos;
                std::string raw = s.substr(textStart, pos - textStart);
                Node textNode;
                textNode.text = unescape(raw);
                node.children.push_back(std::move(textNode));
            }
        }
        return node;
    }
};

} // namespace

std::vector<Node> parse(const std::string& xml) {
    Parser p(xml);
    std::vector<Node> roots;
    p.skipMisc();
    while (!p.eof()) {
        p.skipWs();
        if (p.eof()) break;
        if (p.s.compare(p.pos, 4, "<!--") == 0) {
            size_t end = xml.find("-->", p.pos);
            if (end == std::string::npos) throw std::runtime_error("unterminated comment");
            p.pos = end + 3;
            continue;
        }
        if (p.peek() != '<') throw std::runtime_error("expected '<' at top level");
        roots.push_back(p.parseElement());
    }
    return roots;
}

void findAll(const Node& root, const std::string& tag, std::vector<const Node*>& out) {
    if (root.tag == tag) out.push_back(&root);
    for (const auto& c : root.children) findAll(c, tag, out);
}

} // namespace xmllite
