# html.gadget – Hinweise für Claude

ReAction-/BOOPSI-Klasse (HTML 4) für AmigaOS 3.2, cross-kompiliert mit bebbos amiga-gcc.
Aufbau, Features und Build sind in `README.de.md` beschrieben.

## Zeichenkodierung: ISO-8859-1

- Alle Dateien, die auf den Amiga gehen oder von Amiga-Werkzeugen gelesen werden, sind
  **ISO-8859-1** kodiert: `src/`, `include/`, `sfd/`, `demo/`, `test/`, `ttf/`, `doc/`,
  `package/`, `LICENSE`, `Makefile`. Niemals UTF-8 in diese Dateien schreiben.
- Ausnahmen (UTF-8): `README*.md`, `CLAUDE.md`, `tools/*.py`, `.claude/`.
- Neue Dateien in diesen Verzeichnissen ebenfalls in ISO-8859-1 anlegen. In HTML-Dateien
  bevorzugt Entities (`&auml;`, `&szlig;` …) verwenden. In C-Kommentaren reicht ASCII.
  Wenn Umlaute nötig sind, die Datei z. B. mit Python schreiben
  (`open(p, 'w', encoding='latin-1')`) oder hinterher mit
  `iconv -f UTF-8 -t ISO-8859-1` umwandeln.
- `make charcheck` findet UTF-8-Sequenzen. Ein Claude-Code-Hook (`.claude/settings.json`)
  führt die Prüfung nach jedem Write/Edit aus. Schlägt er an, die genannte Datei sofort
  umwandeln.

## Bauen und Testen

- `make`: Cross-Build (`/opt/amiga`, FreeType unter `~/AmiLib/freetype-2.3.8`), läuft lokal.
- `make` baut `htmlttf.gadget` zusätzlich für 68020–68060 nach `bin/68020/` (eigene Objekte
  in `build/68020/`). Die Optionen `-m68020-60 -mtune=68060` sind Absicht: Ohne
  `-mtune=68060` erzeugt bebbos gcc 64-Bit-`mulu.l`/`divu.l` (z. B. für `x/255`), die der
  68060 nur emuliert. In Pixel- und Glyphen-Schleifen keine `long long`-Arithmetik verwenden.
- `make check`: Parser und Layout auf dem Host (ASan/UBSan). Die vollständige Ausgabe von
  `test/hosttest` wird mit `test/<seite>.expected` verglichen.
- `make check` erzeugt außerdem mit `test/hostprint` (Druck-Engine `src/html_print.c`) von
  jeder Testseite PostScript und PDF und lässt beide von Ghostscript lesen (Fehlermeldungen
  = FAIL). `make print-preview` rendert die PDFs nach `build/print/*.png` zum Ansehen.
  Die Druck-Engine ist plattformunabhängig (Layout in Pixeln zu 1/96 Zoll mit den
  AFM-Breiten aus `src/html_afm.c`, erzeugt von `tools/afm2c.py`); die Amiga-Seite
  (`HTMLM_Export`: Tags, DOS-Ausgabe, Bildpixel) steht in `src/html_export.c`.
- Bei einer gewollten Layoutänderung `make check-update` ausführen und die Änderungen an den
  `.expected`-Dateien im Diff prüfen, bevor sie eingecheckt werden.
- Vor einem Commit: `make` (enthält `charcheck`) und `make check`.

## Testen auf dem Amiga

- HTMLDemo setzt seinen Stack selbst (`__stack`, gelinkt mit `-Wl,-u,___stkinit`), ohne Icon
  gäbe die Workbench nur 4 KB. Keine stdio-Funktionen (`sprintf` …) in HTMLDemo verwenden:
  libnix zieht dann seine Konsolen-Initialisierung für den Workbench-Start nach.

- HTMLDemo öffnet zuerst `gadgets/html.gadget` bzw. `gadgets/htmlttf.gadget` (residente
  Kopie oder `SYS:Classes/Gadgets/`) und erst danach `PROGDIR:`. Eine alte installierte
  oder noch geladene Version überdeckt also den frischen Build in `bin/`.
- Sieht das Host-Layout (`hosttest`) richtig aus, fehlt das Feature auf dem Amiga aber
  komplett, zuerst die geladene Version prüfen (`Version gadgets/html.gadget FULL`) und
  alte Kopien entfernen bzw. mit `Avail FLUSH` aus dem Speicher werfen. Erst dann im Code
  nach Fehlern suchen.
- Bei neuen Features `LIBREVISION` und `LIBDATE` in `src/html_private.h` erhöhen und
  dabei auch `Version:` in `package/html_gadget.readme` und `$VER` in `package/Install`
  anpassen. Nur so lassen sich die Builds unterscheiden, und der Installer ersetzt alte
  Versionen.
- Datumsangaben (`$VER`, `LIBDATE`, `$Id`) immer im Format `TT.MM.JJJJ` schreiben, Tag
  und Monat zweistellig mit führender Null, z. B. `01.10.2026`.

## Neue Features und Testseiten

- Jede neue Testseite in die `CHECKS`-Liste im `Makefile` eintragen und mit
  `make check-update` eine `.expected`-Datei dafür erzeugen.
- `test/*.html`: knappe Randfälle für die automatisierten Tests.
- `demo/*.html`: deutschsprachige Vorführseiten für HTMLDemo. Sie dürfen nur vorhandene
  Bilder verwenden (`boing.gif`, `kachel.gif`, `papier.gif`, `streifen.gif`,
  `farben.iff`), müssen von `demo/example.html` aus verlinkt sein und in `DEMOFILES` im
  `Makefile` stehen. `tools/mkdist.py` übernimmt `demo/*.html` automatisch.
- Neue Features in `README.md` **und** `README.de.md` dokumentieren, Tags/Attribute
  ggf. auch in `doc/*.doc`.
- Für Anwender sichtbare Änderungen knapp in die Versionsgeschichte von
  `package/html_gadget.readme` eintragen, englisch (`History`) **und** deutsch
  (`Versionsgeschichte`), unter der Version aus `src/html_private.h`. Zeilen höchstens
  78 Zeichen. Kein `Replaces:`, solange Archivname und Aminet-Verzeichnis gleich bleiben.

## Icons

- Alle Icons (klassisch, GlowIcons, NewIcons) zeichnet `tools/mkicons.py`; `mkdist.py`
  schreibt damit die Icons des Archivs (klassisch neben den Dateien, alle Stile in
  `Icons/<Stil>/`). Die Dateien in `icons/` sind Beispiele zum Ansehen auf dem Amiga und
  werden mit `make icons` neu erzeugt, nie von Hand bearbeitet.
- NewIcons: Bildbreite 42 und 14 Farben sind Absicht (Paletten- und Zeilenenden fallen
  auf 7-Bit-Grenzen), siehe Kommentare in `mkicons.py`.
