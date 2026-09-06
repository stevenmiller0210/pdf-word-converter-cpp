# pdf-word-converter-cpp

Natív, C++ / GTK3 asztali alkalmazás Word (.docx) és PDF fájlok kétirányú
átalakítására — saját GUI-val, kézzel írt DOCX- és PDF-kezeléssel (nincs
Poppler C++ könyvtár, nincs libhpdf, nincs harmadik féltől származó
DOCX/PDF library; az egyetlen könyvtári függőség a zlib és a GTK3).

Az irány automatikusan a kiválasztott fájl kiterjesztéséből dől el:
`.docx` → PDF, `.pdf` → Word.

## Mit visz át, és mit nem

**Word → PDF** (`docx_reader` → `pdf_writer`):

| Megmarad | Hogyan |
| --- | --- |
| Bekezdésszöveg, tetszőleges Unicode | Beágyazott, részhalmazolt TrueType betűtípus CID-fontként (`Identity-H`) |
| Title / Heading 1-3 stílusok | Nagyobb, félkövér szedés, arányos térközökkel |
| Félkövér, dőlt, félkövér-dőlt futamok | Négy külön betűváltozat (Regular/Bold/Italic/BoldItalic) |
| Felsorolt és számozott listák, egymásba ágyazva | A `numbering.xml`-ből olvasva; a szintenkénti jelölő `•` / `◦` / `▪` |
| Táblázatok | Valódi rácsként, a `w:tblGrid` oszlopszélességeivel, kerettel; a fejlécsor kiemelve |
| Képek (PNG, JPEG) | JPEG változatlanul (`DCTDecode`), PNG kitömörítve és újratömörítve (`FlateDecode`); az átlátszóság `/SMask`-ként |
| Oldaltörés | Automatikus; egy táblázatsor sosem törik ketté két oldal között |

Amit **nem** visz át: színek, aláhúzás, betűtípus-választás, pontos
sortörés/oldaltördelés a forrásból, fejléc/lábléc, lábjegyzetek, beágyazott
diagram- és OLE-objektumok, GIF/BMP/TIFF/EMF/WMF képek (ezeket a PDF sem
tudja natívan, átkódolásuk külön munka lenne — a kép ilyenkor kimarad, a
dokumentum többi része hibátlanul elkészül).

**PDF → Word** (`pdf_reader` → `docx_writer`):

A PDF nem tárol bekezdéseket, címsorokat vagy listákat — csak glyphokat
koordinátákkal. Amit tárol, az a geometria, és a `pdftotext -bbox-layout`
ezt adja vissza szavanként, kerethatárokkal. Ebből építi újra a beolvasó:

- **Bekezdések**: az egy blokkon belüli sorokat összefűzi, és ott kezd újat,
  ahol a betűméret változik, az előző sor jóval a hasáb széle előtt ér
  véget, a sor beljebb kezdődik, vagy szokatlanul nagy a függőleges rés.
  A sorvégi kötőjelet **megtartja**: a Word és a LibreOffice alapból nem
  választ el, így ott a kötőjel szinte mindig valódi (egy összetett szó
  kötőjele, ami véletlenül a sor végére esett) — az eldobása
  `Word-bekezdés`-ből `Wordbekezdés`-t csinált. Cserébe egy ténylegesen
  elválasztott PDF-ben (LaTeX, újság) marad egy látható kötőjel a szó
  közepén.
- **Címsorok**: a dokumentum leggyakoribb sormagasságához (a kenyérszöveg
  mérete) viszonyítva; a nagyobb, rövid blokkok lesznek Title / Heading 1-3.
- **Listák**: a sor elejéről lekerülő `•`/`-`/`1.`/`1)` jelölőből.

Amit **nem** lehet visszanyerni: félkövér/dőlt szedés (a kerethatárok nem
mondják meg), táblázatszerkezet (minden cella külön bekezdésként érkezik),
képek, színek. Ez nem hiányzó funkció, hanem a formátum korlátja.

Ha a PDF-ben **egyáltalán nincs kinyerhető szöveg** (szkennelt / kép alapú
PDF), a program ezt külön hibaüzenettel jelzi, ahelyett hogy néma, üres
`.docx`-et adna vissza.

## Két dolog, amit érdemes tudni a belsőkről

### Beágyazott betűtípus, `Identity-H` kódolással

A PDF beépített (nem beágyazott) Helvetica betűtípusának
`WinAnsiEncoding`-ja nem tartalmazza a magyar `ő`/`ű` betűket — de a
probléma ennél sokkal tágabb: **semmi**, ami a Latin-1-en kívül esik (lengyel,
cseh, cirill, görög, `€`, `→`) nem fér bele egy egybájtos kódolásba. A korábbi
megoldás egy egyéni `/Differences` blokk volt, ami pont a magyar ékezeteket
mentette meg, a többit viszont `?`-re cserélte.

A mostani megoldás a rendszer DejaVu betűtípusát ágyazza be CID-fontként
(`/Type0` + `/Identity-H` + `CIDFontType2` + `/CIDToGIDMap /Identity` +
`FontFile2`), így nincs kódlapkorlát. Két járulékos haszna van:

- A sortördelés a font valódi `hmtx` metrikáit használja, nem becslést —
  a szélességszámítás így pontos, nem "biztonságból szélesebbre".
- A `/ToUnicode` CMap miatt a PDF szövege **kimásolható és kereshető**
  marad; enélkül a lap tökéletesen nézne ki, a belőle kimásolt szöveg mégis
  értelmetlen lenne.

A betűtípus részhalmazolva kerül be (`src/font.cpp`, `subsetFont`): a
`glyf` tábla csak a ténylegesen használt glyphokat tartalmazza, de a
glyph-azonosítók **nem** kapnak új számozást — a `loca` megtartja a teljes
hosszát, üres bejegyzésekkel. Így az összetett glyphok (`á` = `a` + éles
ékezet) továbbra is a helyes komponensekre mutatnak, átszámozás nélkül, és
a PDF használhatja az `/Identity` CID→GID leképezést. Az ára egy ~25 KB-os
`loca` tábla, ami eltörpül a lecserélt ~750 KB-os betűkészlet mellett: a
hétnyelvű tesztdokumentum PDF-je 42 KB.

### Miért sanitizál a DOCX-író minden szöveget

A `.docx` XML-t tartalmaz, az XML 1.0 pedig nem tud ábrázolni bizonyos
vezérlőkaraktereket, és nem fogad el hibás UTF-8-at vagy magányos
surrogate-eket. PDF-ből kinyert szövegben mindhárom előfordul. Korábban
ezek változatlanul átmentek a csomagba, a `zip` pedig boldogan
becsomagolta — az eredmény egy `.docx` volt, amit sem a Word, sem a
LibreOffice nem nyitott meg, miközben a program **sikert jelentett**. Egy
csendben hibás kimeneti fájl a lehető legrosszabb hibamód, ezért a
tisztítás abban az egyetlen függvényben történik, amin minden szöveg
átmegy a csomagba menet (`escapeXml`, `src/docx_writer.cpp`).

## Felépítés

```
src/
  doc_model.h           közös dokumentum-modell (futamok, listák, táblázatok, képek)
  xml_lite.{h,cpp}      minimális, függőségmentes XML-parser
  process_util.{h,cpp}  biztonságos subprocess-hívás (fork+execvp, NEM shellen
                        keresztül — sosem értelmezhető parancsinjekcióként)
  font.{h,cpp}          TrueType beolvasás (cmap 4/12, hmtx, loca, glyf) és
                        részhalmazolás; zlib-tömörítés
  image_codec.{h,cpp}   képek PDF-be ágyazható alakra hozása (JPEG passthrough,
                        PNG kitömörítés + szűrők visszafejtése, alfa -> SMask)
  docx_reader.{h,cpp}   .docx beolvasása (unzip + xml_lite): futamok, listák,
                        táblázatok, beágyazott képek
  docx_writer.{h,cpp}   .docx írása (kézzel épített OOXML csomag + zip)
  pdf_reader.{h,cpp}    PDF -> dokumentumszerkezet (pdftotext -bbox-layout)
  pdf_writer.{h,cpp}    PDF írása: tördelés, oldaltörés, táblázatrajzolás,
                        képelhelyezés, kézzel írt PDF szintaxis
  convert.{h,cpp}       a modulokat összekötő magas szintű API
  main.cpp              GTK3 GUI
tests/
  cli_test.cpp          fejnélküli parancssoros wrapper a convert.h köré
  style_smoke_test.cpp  a DOCX-író azon útjai, amiket a PDF -> Word irány
                        nem tud elérni (futamok, listák, táblázatok)
  run_tests.sh          végponttól végpontig futó ellenőrzések (make test)
  sample_hu.docx        magyar ékezetes mintadokumentum
```

## Fordítás

Függőségek (Ubuntu/Debian):

```
sudo apt install -y build-essential libgtk-3-dev zlib1g-dev poppler-utils \
                    zip unzip fonts-dejavu
```

A `poppler-utils` adja a `pdftotext`-et; a `fonts-dejavu` a beágyazandó
betűkészletet (ha hiányzik, a program `fc-match`-csel keres helyette másik
TrueType fontot, és csak akkor hibázik, ha egyet sem talál).

```
make            # GUI binaris:            ./pdf-word-converter
make cli-test   # fejnelkuli teszt-CLI:   ./cli-test
make test       # a teljes tesztkeszlet lefuttatasa
```

## Használat

GUI: `./pdf-word-converter`, majd "Tallózás..." egy `.docx` vagy `.pdf`
fájlra, "Átalakítás".

Parancssor (teszteléshez / szkriptekhez):

```
./cli-test bemenet.docx kimenet.pdf
./cli-test bemenet.pdf kimenet.docx
```

## Licenc

MIT.
