# pdf-word-converter-cpp

Natív, C++ / GTK3 asztali alkalmazás Word (.docx) és PDF fájlok kétirányú
átalakítására — saját GUI-val, függőségek nélküli, kézzel írt DOCX- és
PDF-kezeléssel (nincs Poppler C++ könyvtár, nincs libhpdf, nincs harmadik
féltől származó DOCX/PDF library).

Az irány automatikusan a kiválasztott fájl kiterjesztéséből dől el:
`.docx` → PDF, `.pdf` → Word.

## Miért érdemes tudni, hogyan épül fel

Ez egy szándékosan minimális, "csak szöveg" konverter — **nem** egy
teljes értékű Word/PDF motor pótléka. Konkrétan:

- **Megmarad**: bekezdésszöveg, valamint a Title/Heading1-3 stílusok
  (Word → PDF irányban vastagabb, nagyobb betűmérettel jelennek meg; PDF →
  Word irányban ez az információ egyáltalán nincs a PDF-ben, így nem is
  származtatható vissza).
- **Nem marad meg**: táblázatok szerkezete (minden táblázatcella saját sima
  szövegsorként jelenik meg, üres cellák pedig egyszerűen kimaradnak — nem
  próbáljuk "kitalálni" a táblát), képek, betűtípus-formázás (dőlt,
  aláhúzás, színek), pontos sortörés/oldaltördelés a forrásból.
- PDF → Word irányban a szövegkinyerés a `pdftotext -layout`-ra
  támaszkodik (Poppler, rendszeren telepített parancssori eszköz) — ha a
  PDF-ben egyáltalán nincs kinyerhető szöveg (szkennelt/kép alapú PDF), a
  program ezt egyértelműen jelzi ahelyett, hogy néma, üres .docx-et adna
  vissza.
- Word → PDF irányban a szóközönkénti sortördelés **közelítő
  karakterszélesség-becslésen** alapul, nem a Helvetica valódi AFM
  metrikáján — szándékosan mindig a "szélesebbre becsül" oldalon hibázik,
  hogy garantáltan sose fusson túl az oldalszélen (ehelyett néha egy kicsit
  korábban tör sort a kelleténél).

### Egy érdekesség: magyar ékezetes betűk PDF-ben, beágyazott betűtípus nélkül

A PDF szabvány beépített (nem beágyazott) Helvetica betűtípusának alap
`WinAnsiEncoding`-ja **nem** tartalmazza a magyar hosszú ékezetes `ő`/`ű`
(és nagybetűs `Ő`/`Ű`) karaktereket — ezek nincsenek benne a Windows-1252
kódlapban. A megoldás nem betűtípus-beágyazás (ami sokkal bonyolultabb
lenne), hanem egy egyéni `/Differences` encoding-blokk a PDF betűtípus
objektumban, ami néhány egyébként szabad bájtkódot a szabványos Adobe
glyph-nevekhez (`/ohungarumlaut`, `/uhungarumlaut`, stb.) rendel — ezeket a
glyphokat maga a Helvetica alapbetűtípus is tartalmazza, csak a
WinAnsiEncoding alapértelmezetten nem hivatkozik rájuk. Lásd
`src/pdf_writer.cpp` (`kEncodingDict`, `encodeGlyphByte`).

## Felépítés

```
src/
  doc_model.h       közös, egyszerű dokumentum-modell (bekezdések + stílus)
  xml_lite.{h,cpp}   minimális, függőségmentes XML-parser (word/document.xml-hez)
  process_util.{h,cpp}  biztonságos subprocess-hívás (fork+execvp, NEM shell-en
                     keresztül — sosem értelmezhető parancsinjekcióként)
  docx_reader.{h,cpp}   .docx beolvasása (unzip + xml_lite)
  docx_writer.{h,cpp}   .docx írása (kézzel épített OOXML csomag + zip)
  pdf_reader.{h,cpp}    PDF szövegkinyerés (pdftotext -layout)
  pdf_writer.{h,cpp}    PDF írása (kézzel írt PDF szintaxis, Helvetica alapbetűtípus)
  convert.{h,cpp}       a fenti négy modult összekötő magas szintű API
  main.cpp              GTK3 GUI
tests/
  cli_test.cpp          fejnélküli parancssoros wrapper a convert.h köré
                     (a motor GUI nélkül is tesztelhető vele)
```

## Fordítás

Függőségek (Ubuntu/Debian):

```
sudo apt install -y build-essential libgtk-3-dev poppler-utils zip unzip
```

(A `poppler-utils` adja a `pdftotext`-et, a `zip`/`unzip` pedig szinte
minden Linux desktopon eleve telepítve van.)

```
make            # elkésziti a GUI binárist: ./pdf-word-converter
make cli-test   # elkésziti a fejnélküli teszt binárist: ./cli-test
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
