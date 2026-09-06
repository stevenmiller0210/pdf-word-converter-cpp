"""Builds a minimal .docx with a running header/footer across three pages,
by hand — the smallest fixture that can prove header/footer recovery, since
a header/footer only means anything once there's more than one page for it
to repeat on. Used only by tests/run_tests.sh.

Usage: make_header_footer_docx.py <output.docx>
"""
import sys
import zipfile

CONTENT_TYPES = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="xml" ContentType="application/xml"/>'
    '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
    '<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>'
    '<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>'
    '</Types>'
)
ROOT_RELS = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
    '</Relationships>'
)
DOC_RELS = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rIdHdr1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>'
    '<Relationship Id="rIdFtr1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>'
    '</Relationships>'
)
W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
R = 'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'

HEADER = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:hdr ' + W + '>'
    '<w:p><w:r><w:t>PistApp teszt fejlec</w:t></w:r></w:p></w:hdr>'
)
FOOTER = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:ftr ' + W + '>'
    '<w:p><w:r><w:t>PistApp teszt lablec</w:t></w:r></w:p></w:ftr>'
)

PAGE_WORDS = {1: "elso", 2: "masodik", 3: "harmadik"}

body_parts = []
for n in (1, 2, 3):
    # Padding above and below the real sentence, so it sits solidly in the
    # middle of the page and can never be mistaken for a header or footer
    # by virtue of merely sitting near a margin on a short page — a real
    # multi-paragraph document does not look like the earlier, sparser
    # version of this fixture did. Naming the page in words (not the digit
    # the masking strips) keeps each page's padding genuinely distinct, the
    # way real body text naturally is, rather than accidentally repeating
    # verbatim the way the real header/footer deliberately does.
    word = PAGE_WORDS[n]
    for i in range(6):
        body_parts.append(
            '<w:p><w:r><w:t>Toltelek bekezdes a(z) ' + word + ' oldalon, sorszam ' +
            str(i) + '.</w:t></w:r></w:p>'
        )
    body_parts.append(
        '<w:p><w:r><w:t>' + str(n) +
        '. oldal egyedi szovege, elegge hosszu ahhoz hogy legyen mit olvasni rajta.</w:t></w:r></w:p>'
    )
    for i in range(6):
        body_parts.append(
            '<w:p><w:r><w:t>Meg tobb toltelek a(z) ' + word + ' oldalon, sorszam ' +
            str(i) + '.</w:t></w:r></w:p>'
        )
    if n < 3:
        body_parts.append('<w:p><w:r><w:br w:type="page"/></w:r></w:p>')

DOCUMENT = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document ' + W + ' ' + R + '>'
    '<w:body>' + ''.join(body_parts) + '<w:sectPr>'
    '<w:headerReference w:type="default" r:id="rIdHdr1"/>'
    '<w:footerReference w:type="default" r:id="rIdFtr1"/>'
    '<w:pgSz w:w="11906" w:h="16838"/>'
    '<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134"/>'
    '</w:sectPr></w:body></w:document>'
)


def main():
    if len(sys.argv) != 2:
        print("Usage: make_header_footer_docx.py <output.docx>", file=sys.stderr)
        return 2
    with zipfile.ZipFile(sys.argv[1], "w") as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", ROOT_RELS)
        z.writestr("word/document.xml", DOCUMENT)
        z.writestr("word/_rels/document.xml.rels", DOC_RELS)
        z.writestr("word/header1.xml", HEADER)
        z.writestr("word/footer1.xml", FOOTER)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
