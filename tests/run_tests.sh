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
        else
            bad "a kepes dokumentum atalakitasa elszallt"
        fi
    else
        skip "a soffice nem tudott .docx-et gyartani"
    fi
else
    skip "soffice nincs telepitve"
fi

echo
echo "Osszesen: $PASS sikeres, $FAIL sikertelen."
[ "$FAIL" -eq 0 ]
