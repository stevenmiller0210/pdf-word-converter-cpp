#!/usr/bin/env bash
# End-to-end checks for the conversion engine. Everything here runs the real
# binaries against real files and inspects the actual output — there is no
# mocking, because every interesting failure this project has had so far
# (missing glyphs, a PDF with no text layer, a .docx that no word processor
# would open) was invisible until something read the produced file back.
#
# Run with: make test
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()   { printf '  \033[32mOK\033[0m   %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m %s\n' "$1"; }

need() { command -v "$1" >/dev/null 2>&1; }

for tool in pdftotext unzip zip python3; do
    if ! need "$tool"; then
        echo "Hianyzo eszkoz: $tool - a tesztek nem futtathatok."
        exit 2
    fi
done

CLI="$ROOT/cli-test"
SMOKE="$ROOT/style-smoke"
if [ ! -x "$CLI" ] || [ ! -x "$SMOKE" ]; then
    echo "Eloszor: make cli-test style-smoke"
    exit 2
fi

echo "== Word -> PDF: magyar ekezetek =="
if "$CLI" "$ROOT/tests/sample_hu.docx" "$WORK/hu.pdf" >/dev/null; then
    TEXT="$(pdftotext "$WORK/hu.pdf" - 2>/dev/null)"
    # The point of embedding a real font: these five characters are exactly
    # what the old base-14 Helvetica path turned into "?".
    for ch in "ő" "ű" "Á" "ü" "ó"; do
        case "$TEXT" in
            *"$ch"*) ok "a(z) '$ch' megmaradt a PDF szovegretegeben" ;;
            *)       bad "a(z) '$ch' elveszett a PDF-bol" ;;
        esac
    done
    case "$TEXT" in
        *"Árvíztűrő tükörfúrógép"*) ok "a teljes pangram olvashato vissza" ;;
        *)                          bad "a pangram nem nyerheto vissza" ;;
    esac
else
    bad "a sample_hu.docx atalakitasa elszallt"
fi

echo "== Formazas, lista, tablazat: DOCX iras -> PDF =="
if "$SMOKE" "$WORK/rich.docx" >/dev/null && "$CLI" "$WORK/rich.docx" "$WORK/rich.pdf" >/dev/null; then
    ok "a gazdag dokumentum vegigment a teljes lancon"
    TEXT="$(pdftotext -layout "$WORK/rich.pdf" - 2>/dev/null)"
    for needle in "Cim stilus teszt" "Ez felkover, ez dolt" "Felsorolas elem 3" \
                  "Szamozott elem 2" "Programozas" "B/03"; do
        case "$TEXT" in
            *"$needle"*) ok "megvan: $needle" ;;
            *)           bad "hianyzik a PDF-bol: $needle" ;;
        esac
    done
    # A run boundary used to swallow the space between differently styled
    # runs ("Ezfelkover"); this is the regression guard for that.
    case "$TEXT" in
        *"Ezfelkover"*) bad "a stilusvaltasnal elveszett a szokoz" ;;
        *)              ok "a stilusvaltasnal megmarad a szokoz" ;;
    esac
else
    bad "a gazdag dokumentum atalakitasa elszallt"
fi

echo "== PDF -> Word: szerkezet visszanyerese =="
if "$CLI" "$WORK/rich.pdf" "$WORK/back.docx" >/dev/null; then
    XML="$(unzip -p "$WORK/back.docx" word/document.xml)"
    printf '%s' "$XML" | python3 -c 'import sys,xml.dom.minidom as m; m.parseString(sys.stdin.buffer.read())' 2>/dev/null \
        && ok "az eloallitott document.xml ervenyes XML" \
        || bad "az eloallitott document.xml nem ervenyes XML"
    for needle in 'w:val="Title"' 'w:val="Heading1"' 'w:numId'; do
        case "$XML" in
            *"$needle"*) ok "visszanyerve: $needle" ;;
            *)           bad "nem nyerte vissza: $needle" ;;
        esac
    done
    # Every wrapped line used to become its own paragraph; a body paragraph
    # should now hold a whole sentence.
    case "$XML" in
        *"Arvizturo tukorfurogep."*) ok "a tordelt sorok egy bekezdesse allnak ossze" ;;
        *)                           bad "a bekezdesek nem alltak ossze" ;;
    esac

    # Character formatting and table structure used to be dropped outright.
    case "$XML" in *"<w:b/>"*) ok "a felkover szedes visszanyerve" ;; *) bad "a felkover szedes elveszett" ;; esac
    case "$XML" in *"<w:i/>"*) ok "a dolt szedes visszanyerve" ;; *) bad "a dolt szedes elveszett" ;; esac
    case "$XML" in *"<w:tbl>"*) ok "a tablazat szerkezete visszanyerve" ;; *) bad "a tablazat nem allt ossze" ;; esac
    case "$XML" in *"<w:tblHeader/>"*) ok "a fejlecsor felismerve" ;; *) bad "a fejlecsor nem lett felismerve" ;; esac
    # PDF stores colours as floats, so a component can come back off by 1/255
    # — match the leading hex digits rather than the exact value.
    case "$XML" in *'w:color w:val="C0392B"'*) ok "a szoveg szine visszanyerve (piros)" ;; *) bad "a szoveg szine elveszett" ;; esac
    COLORS="$(printf '%s' "$XML" | grep -o 'w:color w:val="[0-9A-F]*"' | sort -u | wc -l)"
    [ "$COLORS" -ge 3 ] && ok "mindharom szin kulon maradt ($COLORS db)" \
                        || bad "csak $COLORS kulonbozo szin maradt meg a harombol"
else
    bad "a PDF -> Word irany elszallt"
fi

echo "== Hibautak =="
printf 'nem docx' > "$WORK/fake.docx"
"$CLI" "$WORK/fake.docx" "$WORK/fake.pdf" >/dev/null 2>&1 \
    && bad "a hamis .docx sikeresnek latszott" \
    || ok "a hamis .docx elutasitva"

printf '%%PDF-1.4 szemet' > "$WORK/fake.pdf"
"$CLI" "$WORK/fake.pdf" "$WORK/fake.docx" >/dev/null 2>&1 \
    && bad "a serult PDF sikeresnek latszott" \
    || ok "a serult PDF elutasitva"

echo "== Kepek (opcionalis, soffice kell hozza) =="
if need soffice; then
    python3 - "$WORK" <<'PY'
import base64, struct, sys, zlib
work = sys.argv[1]

def png(path, w, h, mode, pixfn):
    ch = {0: 1, 2: 3, 6: 4}[mode]
    raw = b''
    for y in range(h):
        raw += b'\x00' + bytes(b for x in range(w) for b in pixfn(x, y))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, mode, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9))
            + chunk(b'IEND', b''))
    open(path, 'wb').write(data)
    return data

rgb  = png(work + '/rgb.png',  60, 40, 2, lambda x, y: (x * 4 % 256, y * 6 % 256, 128))
rgba = png(work + '/rgba.png', 40, 40, 6, lambda x, y: (255, 0, 0, 255 if (x + y) % 8 < 4 else 0))
gray = png(work + '/gray.png', 30, 30, 0, lambda x, y: ((x * 8 + y * 8) % 256,))

def uri(mime, data):
    return 'data:%s;base64,%s' % (mime, base64.b64encode(data).decode())

open(work + '/img.html', 'w').write(
    '<html><body><h1>Kepek</h1>'
    '<p><img src="%s" width="120" height="80"></p>'
    '<p><img src="%s" width="80" height="80"></p>'
    '<p><img src="%s" width="60" height="60"></p>'
    '</body></html>' % (uri('image/png', rgb), uri('image/png', rgba), uri('image/png', gray)))
PY
    if soffice --headless --infilter="HTML (StarWriter)" \
               --convert-to 'docx:MS Word 2007 XML' --outdir "$WORK" "$WORK/img.html" \
               >/dev/null 2>&1 && [ -f "$WORK/img.docx" ]; then
        if "$CLI" "$WORK/img.docx" "$WORK/img.pdf" >/dev/null; then
            if need pdfimages; then
                # The `enc` column also reads "image", so match the type
                # column by position rather than grepping the whole line.
                COUNT="$(pdfimages -list "$WORK/img.pdf" 2>/dev/null | awk '$3=="image"' | wc -l)"
                [ "$COUNT" -ge 3 ] && ok "mind a harom kep beagyazodott ($COUNT db)" \
                                   || bad "csak $COUNT kep agyazodott be a harombol"
                pdfimages -list "$WORK/img.pdf" 2>/dev/null | awk '$3=="smask"' | grep -q . \
                    && ok "az atlatszo PNG-bol soft mask lett" \
                    || bad "az atlatszo PNG alfa-csatornaja elveszett"
            else
                skip "pdfimages nincs telepitve"
            fi

            # ... and back again: a picture in a PDF should land in the .docx
            # as a real embedded image, not silently disappear.
            if "$CLI" "$WORK/img.pdf" "$WORK/img-back.docx" >/dev/null; then
                MEDIA="$(unzip -l "$WORK/img-back.docx" | grep -c 'word/media/.*\.')"
                [ "$MEDIA" -ge 3 ] && ok "a PDF kepei visszakerultek a .docx-be ($MEDIA db)" \
                                   || bad "csak $MEDIA kep kerult vissza a .docx-be"
            else
                bad "a kepes PDF -> Word irany elszallt"
            fi
        else
            bad "a kepes dokumentum atalakitasa elszallt"
        fi
    else
        skip "a soffice nem tudott .docx-et gyartani"
    fi
else
    skip "soffice nincs telepitve"
fi

echo "== Osszevont cellak, keret, hatterszin, igazitas, betutipus =="
# A ruled table drawn with real borders and shading, plus a right-aligned
# and a left-aligned paragraph sitting right next to each other — the case
# that used to get merged into one paragraph before alignment was one of
# the signals continuesParagraph looks at.
if need soffice; then
    cat > "$WORK/merged.html" <<'HTML'
<html><body>
<h1 style="text-align:center">Kozepre zart cim</h1>
<p style="text-align:right">Jobbra zart bekezdes.</p>
<p>Balra zart, normal bekezdes a tablazat elott.</p>
<table border="1" cellpadding="4" style="border-collapse:collapse">
<tr style="background-color:#dde5f0">
  <th colspan="2">Osszevont fejlec</th><th>Harmadik</th>
</tr>
<tr>
  <td rowspan="2">Ket sorra<br>osszevonva</td><td>B1</td><td>C1</td>
</tr>
<tr>
  <td>B2</td><td style="background-color:#f6dede">C2 szines</td>
</tr>
</table>
</body></html>
HTML
    if soffice --headless --infilter="HTML (StarWriter)" --convert-to 'docx:MS Word 2007 XML' \
               --outdir "$WORK" "$WORK/merged.html" >/dev/null 2>&1 && \
       soffice --headless --convert-to pdf --outdir "$WORK" "$WORK/merged.docx" >/dev/null 2>&1 && \
       "$CLI" "$WORK/merged.pdf" "$WORK/merged_back.docx" >/dev/null; then
        XML="$(unzip -p "$WORK/merged_back.docx" word/document.xml)"
        case "$XML" in *'w:jc w:val="center"'*) ok "kozepre igazitas felismerve" ;; *) bad "kozepre igazitas elveszett" ;; esac
        case "$XML" in *'w:jc w:val="right"'*) ok "jobbra igazitas felismerve" ;; *) bad "jobbra igazitas elveszett" ;; esac
        case "$XML" in
            *"Jobbra zart bekezdes. Balra zart"*) bad "a jobbra es balra zart bekezdes egybeolvadt" ;;
            *)                                    ok "a jobbra es balra zart bekezdes kulon maradt" ;;
        esac
        case "$XML" in *'w:gridSpan w:val="2"'*) ok "az oszlop-osszevonas (colspan) visszanyerve" ;; *) bad "a colspan elveszett" ;; esac
        case "$XML" in *'w:vMerge w:val="restart"'*) ok "a sor-osszevonas (rowspan) visszanyerve" ;; *) bad "a rowspan elveszett" ;; esac
        case "$XML" in *'w:fill="DDE5F0"'*) ok "a fejlec hatterszine visszanyerve" ;; *) bad "a fejlec hatterszine elveszett" ;; esac
        case "$XML" in *'w:fill="F6DEDE"'*) ok "a cella hatterszine visszanyerve" ;; *) bad "a cella hatterszine elveszett" ;; esac
        case "$XML" in *"Liberation Serif"*) ok "a betutipus neve visszanyerve" ;; *) bad "a betutipus neve elveszett" ;; esac

        if soffice --headless --convert-to pdf --outdir "$WORK" "$WORK/merged_back.docx" >/dev/null 2>&1; then
            BACKTEXT="$(pdftotext -layout "$WORK/merged_back.pdf" - 2>/dev/null)"
            case "$BACKTEXT" in
                *"Ket sorra"*"osszevonva"*) ok "az osszevont cella tartalma vegig eljutott" ;;
                *)                          bad "az osszevont cella tartalma elveszett a Word -> PDF utban" ;;
            esac
        else
            skip "nem sikerult visszaalakitani PDF-re a Word -> PDF ut ellenorzesehez"
        fi
    else
        skip "nem sikerult osszevont-cellas teszt-PDF-et gyartani"
    fi
else
    skip "soffice nincs telepitve"
fi

echo "== Szimbolum-betukeszletes felsorolasjel (soffice kell hozza) =="
# LibreOffice (and Word) draw a bullet from the Symbol font: the PDF carries
# it as the Private Use Area glyph U+F0B7, in a text fragment of its own. It
# used to survive as a literal character in an "Open Symbol" run — a box in
# Word — instead of becoming a real list item.
if need soffice; then
    printf '<html><body><ul><li>Elso pont</li><li>Masodik pont</li></ul></body></html>' > "$WORK/bullets.html"
    if soffice --headless --infilter="HTML (StarWriter)" --convert-to 'docx:MS Word 2007 XML' \
               --outdir "$WORK" "$WORK/bullets.html" >/dev/null 2>&1 && \
       soffice --headless --convert-to pdf --outdir "$WORK" "$WORK/bullets.docx" >/dev/null 2>&1 && \
       "$CLI" "$WORK/bullets.pdf" "$WORK/bullets_back.docx" >/dev/null; then
        XML="$(unzip -p "$WORK/bullets_back.docx" word/document.xml)"
        case "$XML" in *$'\xEF\x82\xB7'*) bad "a Symbol-felsorolasjel (U+F0B7) bent maradt a szovegben" ;; *) ok "a Symbol-felsorolasjel nem maradt a szovegben" ;; esac
        case "$XML" in *'<w:numPr>'*'Elso pont'*) ok "a felsorolas valodi listakent jott vissza" ;; *) bad "a felsorolasbol nem lett lista" ;; esac
    else
        bad "a felsorolasos teszt-PDF konverzioja nem sikerult"
    fi
else
    skip "soffice nincs telepitve"
fi
echo

echo "== Fejlec es lablec =="
if need soffice && need python3; then
    if python3 "$ROOT/tests/fixtures/make_header_footer_docx.py" "$WORK/hf.docx" >/dev/null && \
       soffice --headless --convert-to pdf --outdir "$WORK" "$WORK/hf.docx" >/dev/null 2>&1 && \
       "$CLI" "$WORK/hf.pdf" "$WORK/hf_back.docx" >/dev/null; then
        PARTS="$(unzip -l "$WORK/hf_back.docx")"
        case "$PARTS" in *"word/header1.xml"*) ok "a fejlec kulon reszkent visszaallt" ;; *) bad "nem keletkezett fejlec resz" ;; esac
        case "$PARTS" in *"word/footer1.xml"*) ok "a lablec kulon reszkent visszaallt" ;; *) bad "nem keletkezett lablec resz" ;; esac
        HDRTEXT="$(unzip -p "$WORK/hf_back.docx" word/header1.xml 2>/dev/null)"
        case "$HDRTEXT" in *"teszt fejlec"*) ok "a fejlec szovege helyes" ;; *) bad "a fejlec szovege hianyzik vagy rossz" ;; esac
        FTRTEXT="$(unzip -p "$WORK/hf_back.docx" word/footer1.xml 2>/dev/null)"
        case "$FTRTEXT" in *"teszt lablec"*) ok "a lablec szovege helyes" ;; *) bad "a lablec szovege hianyzik vagy rossz" ;; esac
        BODYTEXT="$(unzip -p "$WORK/hf_back.docx" word/document.xml 2>/dev/null)"
        case "$BODYTEXT" in
            *"fejlec"*|*"lablec"*) bad "a fejlec/lablec szovege a torzsszovegben is megjelent (nem kulon reszkent kezelve)" ;;
            *)                     ok "a fejlec/lablec nem szivargott bele a torzsszovegbe" ;;
        esac
        if "$CLI" "$WORK/hf_back.docx" "$WORK/hf_back.pdf" >/dev/null 2>&1; then
            # pdf_writer paginates independently of Word/LibreOffice, so the
            # page count is whatever it is — the header just has to appear
            # once per page it actually produced, not a fixed count.
            PAGES="$(pdfinfo "$WORK/hf_back.pdf" 2>/dev/null | awk '/^Pages:/ {print $2}')"
            HDRCOUNT="$(pdftotext "$WORK/hf_back.pdf" - 2>/dev/null | grep -c "teszt fejlec")"
            [ -n "$PAGES" ] && [ "$HDRCOUNT" = "$PAGES" ] \
                && ok "a fejlec minden oldalon megjelent a Word -> PDF utban ($HDRCOUNT/$PAGES oldal)" \
                || bad "a fejlec nem jelent meg minden oldalon ($HDRCOUNT/$PAGES oldal)"
        else
            skip "nem sikerult visszaalakitani PDF-re a Word -> PDF ut ellenorzesehez"
        fi
    else
        skip "nem sikerult fejleces teszt-fixturat gyartani"
    fi
else
    skip "soffice/python3 nincs telepitve"
fi

echo "== Szkennelt PDF: OCR-tartalek =="
# An image-only PDF: render a real page to a bitmap, then wrap the bitmap
# back into a PDF. There is no text layer left, so the only way to recover
# anything is to read the pixels.
if need soffice && need pdftoppm && [ -f "$WORK/rich.pdf" ]; then
    pdftoppm -png -r 150 -f 1 -l 1 "$WORK/rich.pdf" "$WORK/scan" >/dev/null 2>&1
    SCAN="$(ls "$WORK"/scan*.png 2>/dev/null | head -1)"
    if [ -n "$SCAN" ] && soffice --headless --convert-to pdf --outdir "$WORK" "$SCAN" >/dev/null 2>&1; then
        SCANPDF="$(ls "$WORK"/scan*.pdf 2>/dev/null | head -1)"
        if [ -z "$(pdftotext "$SCANPDF" - 2>/dev/null | tr -d '[:space:]')" ]; then
            ok "a teszt-PDF tenyleg szovegreteg nelkuli"
        else
            skip "a teszt-PDF-ben maradt szoveg, nem valodi szkennelt eset"
        fi
        if need tesseract; then
            if "$CLI" "$SCANPDF" "$WORK/scan.docx" >/dev/null 2>&1; then
                TEXT="$(unzip -p "$WORK/scan.docx" word/document.xml)"
                case "$TEXT" in
                    *"stilus"*|*"teszt"*) ok "az OCR kiolvasta a szoveget a kepbol" ;;
                    *)                    bad "az OCR lefutott, de nem talalta meg a szoveget" ;;
                esac
            else
                bad "az OCR-tartalek nem futott le"
            fi
        else
            # Without Tesseract the honest outcome is the explicit "no text
            # here" error, never an empty .docx.
            if "$CLI" "$SCANPDF" "$WORK/scan.docx" >"$WORK/scan.log" 2>&1; then
                bad "tesseract nelkul is sikeresnek latszott a szkennelt PDF"
            elif grep -q "NO_TEXT_EXTRACTED" "$WORK/scan.log"; then
                ok "tesseract nelkul helyesen NO_TEXT_EXTRACTED a valasz"
            else
                bad "tesseract nelkul rossz hibauzenet jott"
            fi
            skip "tesseract nincs telepitve, az OCR maga nem tesztelheto"
        fi
    else
        skip "nem sikerult szkennelt teszt-PDF-et gyartani"
    fi
else
    skip "soffice/pdftoppm nincs telepitve"
fi

echo
echo "Osszesen: $PASS sikeres, $FAIL sikertelen."
[ "$FAIL" -eq 0 ]
