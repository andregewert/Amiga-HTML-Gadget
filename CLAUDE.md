# html.gadget – Hinweise für Claude

ReAction-/BOOPSI-Klasse (HTML 4) für AmigaOS 3.2, cross-kompiliert mit bebbos amiga-gcc.
Aufbau, Features und Build sind in `README.de.md` beschrieben.

Gemeinsame Regeln aller Amiga-Projekte (Zeichensatz, Bauen, Testen, Versionen, Aminet-Paket,
Icons) kommen aus dem Submodule `amiga-devkit`:

@amiga-devkit/docs/Amiga-Regeln.md

## Zeichenkodierung: ISO-8859-1

- ISO-8859-1 sind hier: `src/`, `include/`, `sfd/`, `demo/`, `test/`, `ttf/`, `doc/`,
  `package/`, `LICENSE`, `Makefile`. UTF-8: `README*.md`, `CLAUDE.md`, `tools/*.py`, `.claude/`.
- Der Claude-Code-Hook (`.claude/settings.json`) führt `make charcheck` nach jedem Write/Edit aus.

## Bauen und Testen

- `make`: Cross-Build (`/opt/amiga`, FreeType unter `~/AmiLib/freetype-2.3.8`), läuft lokal.
- `make` baut `htmlttf.gadget` zusätzlich für 68020–68060 nach `bin/68020/` (eigene Objekte
  in `build/68020/`, `-m68020-60 -mtune=68060`, siehe Amiga-Regeln).
- `make check`: Parser und Layout auf dem Host (ASan/UBSan). Die vollständige Ausgabe von
  `test/hosttest` wird mit `test/<seite>.expected` verglichen.
- `make check` erzeugt außerdem mit `test/hostprint` (Druck-Engine `src/html_print.c`) von
  jeder Testseite PostScript und PDF und lässt beide von Ghostscript lesen (Fehlermeldungen
  = FAIL). `make print-preview` rendert die PDFs nach `build/print/*.png` zum Ansehen.
  Die Druck-Engine ist plattformunabhängig (Layout in Pixeln zu 1/96 Zoll mit den
  AFM-Breiten aus `src/html_afm.c`, erzeugt von `tools/afm2c.py`); die Amiga-Seite
  (`HTMLM_Export`: Tags, DOS-Ausgabe, Bildpixel) steht in `src/html_export.c`.
- Bitmap-Druck (`HTMLM_PrintBegin/Render/End`): html.gadget zeichnet mit `draw_content()` in
  eine Bitmap des zuletzt benutzten Bildschirms und liest zurück (nur auf dem Amiga testbar);
  htmlttf.gadget: `src/htmlttf_print.c`,
  plattformunabhängig; Layout in 1/96 Zoll (gleiche Seitenumbrüche wie PS/PDF), gezeichnet mit
  `tr_render_strip()` in Druckerpixeln. `make print-ttf` rendert `demo/example.html` mit
  `test/ttfprint` in 64-Zeilen-Streifen nach `build/print/ttf-<n>.ppm`.

## Testen auf dem Amiga

- HTMLDemo setzt seinen Stack selbst und verwendet keine stdio-Funktionen (siehe Amiga-Regeln).
- HTMLDemo öffnet zuerst `gadgets/html.gadget` bzw. `gadgets/htmlttf.gadget` (residente
  Kopie oder `SYS:Classes/Gadgets/`) und erst danach `PROGDIR:`. Eine alte installierte
  oder noch geladene Version überdeckt also den frischen Build in `bin/`
  (prüfen mit `Version gadgets/html.gadget FULL`).
- Bei neuen Features `LIBREVISION` und `LIBDATE` in `src/html_private.h` erhöhen und
  dabei auch `Version:` in `package/html_gadget.readme` und `$VER` in `package/Install`
  anpassen.

## Neue Features und Testseiten

- Jede neue Testseite in die `CHECKS`-Liste im `Makefile` eintragen und mit
  `make check-update` eine `.expected`-Datei dafür erzeugen.
- `test/*.html`: knappe Randfälle für die automatisierten Tests.
- `demo/*.html`: deutschsprachige Vorführseiten für HTMLDemo. Sie dürfen nur vorhandene
  Bilder verwenden (`boing.gif`, `kachel.gif`, `papier.gif`, `streifen.gif`,
  `farben.iff`), müssen von `demo/example.html` aus verlinkt sein und in `DEMOFILES` im
  `Makefile` stehen. `tools/mkdist.py` übernimmt `demo/*.html` automatisch.
- Tags/Attribute neuer Features ggf. auch in `doc/*.doc` dokumentieren.
- Versionsgeschichte in `package/html_gadget.readme` unter der Version aus
  `src/html_private.h`.

## Icons

- `tools/mkicons.py` registriert das Motiv von HTMLDemo (`demo`) in
  `amiga-devkit/tools/amigaicons.py`; alle anderen Motive, die Stile und das Dateiformat
  kommen von dort. `tools/mkdist.py` schreibt damit die Icons des Archivs (klassisch neben
  den Dateien, alle Stile in `Icons/<Stil>/`).
