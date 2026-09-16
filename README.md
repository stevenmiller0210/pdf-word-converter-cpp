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
| Bekezdés-igazítás (balra/középre/jobbra) | `w:jc` szerinti eltolás soronként |
| Szövegszín | `w:color` → PDF kitöltőszín, futamonként visszaállítva |
| Felsorolt és számozott listák, egymásba ágyazva | A `numbering.xml`-ből olvasva; a szintenkénti jelölő `•` / `◦` / `▪` |
| Táblázatok, cellaösszevonással (`gridSpan`/`vMerge`) | Valódi rácsként, a `w:tblGrid` oszlopszélességeivel; minden cella a saját `w:tcBorders`/`w:shd` szerint kap keretet és hátteret |
| Futó fejléc/lábléc | A `word/header*.xml`/`footer*.xml` bekezdései minden oldal margójára kiírva |
| Képek (PNG, JPEG) | JPEG változatlanul (`DCTDecode`), PNG kitömörítve és újratömörítve (`FlateDecode`); az átlátszóság `/SMask`-ként |
| Oldaltörés | Automatikus; egy táblázatsor sosem törik ketté két oldal között |

Amit **nem** visz át: a `w:rFonts`-ban megnevezett betűtípus (a PDF mindig a
beágyazott DejaVu-négyest használja — a `docx_reader` beolvassa és a
`.docx`-modellben tárolja a betűtípus nevét, csak a PDF-be rajzoláskor nem
alkalmazza), aláhúzás, sorkizárás (a `both`/`justify` igazítás balra
zártként jelenik meg — a szóközök szétnyújtásához a sortördelést kellene
szóhatárok mentén újraírni, ami nem éri meg a hasznot), pontos
sortörés/oldaltördelés a forrásból, lábjegyzetek, beágyazott diagram- és
OLE-objektumok, GIF/BMP/TIFF/EMF/WMF képek (ezeket a PDF sem tudja
natívan, átkódolásuk külön munka lenne — a kép ilyenkor kimarad, a
dokumentum többi része hibátlanul elkészül). Egy soron átnyúló
(rowspan-nal összevont) cellánál, ha a tartalma magasabb, mint amennyit az
a sor egymagában igényelne, a tartalom átlóghat a következő sorba — a
magasság-elosztás több sor között külön munka lenne egy ritkán előforduló
esetért.

**PDF → Word** (`pdf_reader` → `docx_writer`):

A PDF nem tárol bekezdéseket, címsorokat vagy táblázatokat — csak
glyphokat koordinátákkal, és vonalakat/kitöltéseket a rajzolt grafikának.
Amit ezekből ki lehet olvasni, azt a `pdftohtml -xml` (a szöveghez) és a
`pdftocairo -svg` (a vektorgrafikához, lásd lent) mind kiadja, és ebből
épül újra a dokumentum:

| Megmarad | Honnan |
| --- | --- |
| Bekezdések | Az egy blokkon belüli sorokat összefűzi, és ott kezd újat, ahol a betűméret változik, az előző sor jóval a hasáb széle előtt ér véget, a sor beljebb kezdődik, szokatlanul nagy a függőleges rés, vagy az előző sor láthatóan margótól messze kezdődött (jobbra/középre zárt) miközben az új sor a margónál kezdődik |
| Címsorok (Title / Heading 1-3) | A dokumentum leggyakoribb betűméretéhez (a kenyérszöveg mérete) viszonyítva, csak rövid blokkoknál |
| Bekezdés-igazítás (balra/középre/jobbra/sorkizárt) | A sorok margóhoz viszonyított helyzetéből. Sorkizártnak az a bekezdés számít, amelynek az utolsó kivételével minden sora ugyanott ér véget — egy egysoros bekezdésnél ez nem dönthető el, az balra zártként jön vissza |
| Felsorolt és számozott listák | A sor elejéről lekerülő `•` / `-` / `1.` / `1)` jelölőből |
| Félkövér és dőlt szedés | A poppler `<b>` / `<i>` jelöléséből, ami a beágyazott betűtípus nevéből jön |
| Szövegszín | A `<fontspec color="…">` értékéből |
| Betűtípus neve | A `<fontspec family="…">`-ből, a részhalmaz-előtag (`BAAAAA+`) és a stílus-utótag (`-Bold`, `MT`, `PS`) levágva, néhány ismert fájlnév (`DejaVuSans` → `DejaVu Sans`) feloldva |
| Táblázatok, **valódi rajzolt vonalakból** | `pdftocairo -svg`-vel kinyert vízszintes/függőleges vonalakból épített rács; egy hiányzó vonal két szomszédos cella között = összevont cella (`gridSpan`/`vMerge`), a cellák saját `w:tcBorders`/`w:shd`-t kapnak a valódi keret/háttérszín szerint |
| Táblázatok vonal nélkül | Ha a PDF nem rajzol vonalat, tartalék: egymást követő sorokból, amelyek ugyanarra a néhány oszlophatárra esnek; a csupa félkövér első sor fejlécsor lesz |
| Futó fejléc/lábléc | A lap tetején/alján (a magasság felső/alsó 10%-ában) ismétlődő, számjegyeire maszkolva azonos sorokból; sosem üresíti ki egyetlen oldal teljes tartalmát sem (lásd lent) |
| Képek | A poppler kicsomagolja őket a helyükkel és méretükkel együtt; egy JPEG JPEG marad, nincs újrakódolás |

A **sorvégi kötőjelet megtartja**: a Word és a LibreOffice alapból nem
választ el, így ott a kötőjel szinte mindig valódi (egy összetett szó
kötőjele, ami véletlenül a sor végére esett) — az eldobása
`Word-bekezdés`-ből `Wordbekezdés`-t csinált. Cserébe egy ténylegesen
elválasztott PDF-ben (LaTeX, újság) marad egy látható kötőjel a szó
közepén.

**A fejléc/lábléc-felismerésnek van egy biztonsági szelepe**: mivel egy
ismétlődő sor csak a lap margója közelében és csak több oldalon
azonos-a-számjegyek-levágása-után formában számít jelöltnek, egy nagyon
rövid (egy-két bekezdéses) dokumentumon előfordulhatna, hogy a bekezdés
maga esik ebbe a mintába. Ezért mielőtt bármit fejléc/láblécnek
minősítene, a felismerő megnézi: ha az adott oldalon az ÖSSZES jelölt sor
együttes eltávolítása a teljes tartalmat kitörölné arról az oldalról, az
egész zóna jelöltjeit elveti — inkább egy dokumentum, aminek nincs
felismert fejléce, mint egy oldal, aminek nincs tartalma. Ez nem
elméleti eset: az első verzió pontosan ebbe futott bele egy háromoldalas
teszt-dokumentumon, ahol az egyetlen bekezdés minden oldalon a margó
közelében kezdődött.

Amit **nem** lehet visszanyerni: az egysoros sorkizárt bekezdés igazítása
(balra zártként jön vissza), a pontos elrendezés (a PDF-nek nincs bekezdés- vagy
oldalmodellje, amiből ez származna — egy Word-dokumentum újratördelhető,
egy fix elrendezésű PDF nem, ez a formátumok közti alapvető, nem javítható
különbség). A táblázatfelismerés — akár vonalas, akár szöveg-igazításos —
szándékosan óvatos: egy kihagyott táblázat sima bekezdésekké esik szét, egy
tévesen felismert viszont tönkretesz egy egyébként jó szövegrészt.

Egy apróság: a PDF a színt lebegőpontosan tárolja, így egy komponens
1/255-tel eltérhet oda-vissza átalakítás után. Ez nem hiba, csak
kerekítés.

### Ha nincs szövegréteg: OCR

Egy szkennelt oldal csupa képpont — nincs mit kinyerni belőle, hacsak nem
olvassuk vissza a pixeleket. Ilyenkor a program 300 dpi-n kirendereli az
oldalakat (`pdftoppm`), és `tesseract`-tal ismeri fel a szöveget (magyar +
angol, amelyik nyelvi adat telepítve van).

A Tesseract **opcionális**: ha nincs telepítve, a hívó ugyanazt az őszinte
„ebben a PDF-ben nincs szöveg" hibát kapja, mint eddig — sosem egy néma,
üres `.docx`-et. Ugyanez vonatkozik arra az esetre is, amikor a PDF-ben
csak kép van: egy `.docx`, amiben egyetlen oldalkép ül, átalakítottnak
látszik, miközben semmi sem szerkeszthető benne, ezért ez is az OCR-ágra
megy.

Telepítés (Ubuntu/Debian):

```
sudo apt install -y tesseract-ocr tesseract-ocr-hun tesseract-ocr-eng
```

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
  pdf_reader.{h,cpp}    PDF -> dokumentumszerkezet (pdftohtml -xml szoveghez,
                        pdftocairo -svg vonalas tablazat/hatterszin/fejlec-
                        lablec-felismereshez), OCR-tartalek (pdftoppm + tesseract)
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
  fixtures/
    make_header_footer_docx.py  kézzel épített 3 oldalas .docx futó
                        fejléccel/lábléccel — run_tests.sh hívja
```

## Fordítás

Függőségek (Ubuntu/Debian):

```
sudo apt install -y build-essential libgtk-3-dev zlib1g-dev poppler-utils \
                    zip unzip fonts-dejavu
# opcionális, a szkennelt PDF-ek OCR-jéhez:
sudo apt install -y tesseract-ocr tesseract-ocr-hun tesseract-ocr-eng
```

A `poppler-utils` adja a `pdftohtml`-t, a `pdftocairo`-t és a `pdftoppm`-et; a `fonts-dejavu` a
beágyazandó betűkészletet (ha hiányzik, a program `fc-match`-csel keres helyette másik
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
