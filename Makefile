# html.gadget - ReAction HTML 4 class for AmigaOS 3.2
# Cross build with bebbo's amiga-gcc (m68k-amigaos-gcc) and NDK 3.2.

PREFIX  ?= /opt/amiga
CC      := $(PREFIX)/bin/m68k-amigaos-gcc
STRIP   := $(PREFIX)/bin/m68k-amigaos-strip
CPU     ?= -m68000
# object and output directories; the 68020 build of htmlttf.gadget uses its own
B       ?= build
O       ?= bin

CFLAGS  := $(CPU) -Os -fno-common -fomit-frame-pointer -fno-toplevel-reorder -fno-builtin \
           -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign -Iinclude -Isrc
LIBFLAGS:= -nostartfiles -nostdlib
DEMOFLAGS := $(CPU) -Os -Wall -fno-common -Iinclude -noixemul

LIBSRC  := src/html_lib.c src/html_class.c src/html_parse.c src/html_layout.c src/html_select.c src/html_clip.c \
           src/html_check.c
LIBOBJ  := $(LIBSRC:src/%.c=$(B)/%.o)

DEMOFILES := $(addprefix bin/,example.html zweite.html hintergrund.html tabellen.html umfluss.html boing.gif farben.iff papier.gif kachel.gif streifen.gif)
FONTFILES := $(patsubst demo/fonts/%,bin/fonts/%,$(wildcard demo/fonts/*))

# --- htmlttf.gadget: same core, FreeType renderer ---------------------------
# FreeType 2.3.8 sources (Aminet: dev/lib/freetype-2.3.8), only the modules
# needed for TrueType are compiled, with our own config and libc shim (ttf/).
FT      ?= $(HOME)/AmiLib/freetype-2.3.8
FTDEFS  := -Ittf/include -I$(FT)/include -I$(FT)/src/base -DFT2_BUILD_LIBRARY \
           '-DFT_CONFIG_OPTIONS_H=<ftoption_html.h>' '-DFT_CONFIG_MODULES_H=<ftmodule_html.h>' \
           '-DFT_CONFIG_STANDARD_LIBRARY_H=<ftlibc.h>'
FTCFLAGS:= $(CPU) -Os -fno-common -fomit-frame-pointer -fno-builtin $(FTDEFS)
FTSRC   := ttf/ftbase_html.c $(FT)/src/base/ftinit.c $(FT)/src/base/ftdebug.c \
           $(FT)/src/base/ftbitmap.c $(FT)/src/base/ftsynth.c $(FT)/src/base/fttype1.c \
           $(FT)/src/truetype/truetype.c $(FT)/src/sfnt/sfnt.c $(FT)/src/autofit/autofit.c \
           $(FT)/src/smooth/smooth.c ttf/ftsystem.c ttf/ftlibc.c
FTOBJ   := $(addprefix $(B)/ft/,$(notdir $(FTSRC:.c=.o)))
TTFOBJ  := $(B)/ttf/html_lib.o $(B)/ttf/htmlttf_class.o $(B)/ttf/htmlttf_render.o $(B)/html_parse.o $(B)/html_layout.o $(B)/html_select.o $(B)/html_clip.o $(B)/html_check.o $(FTOBJ)

all: charcheck bin/html.gadget bin/htmlttf.gadget bin/HTMLDemo $(DEMOFILES) $(FONTFILES)

# htmlttf.gadget for 68020-68060. -mtune=68060 keeps gcc from using the 64 bit
# mul/div (e.g. for x/255) that the 68060 only emulates; -m68020-60 alone does
# not. html.gadget gains nothing, it stays 68000 only.
CPU020  := -m68020-60 -mtune=68060
ifeq ($(O),bin)
all: ttf020
ttf020:
	@$(MAKE) --no-print-directory CPU="$(CPU020)" B=build/68020 O=bin/68020 bin/68020/htmlttf.gadget
endif

bin/fonts/%: demo/fonts/%
	@mkdir -p bin/fonts
	cp $< $@

vpath %.c ttf $(FT)/src/base $(FT)/src/truetype $(FT)/src/sfnt $(FT)/src/autofit $(FT)/src/smooth
# all FreeType objects must see the same configuration
FTCONF  := ttf/include/ftoption_html.h ttf/include/ftmodule_html.h ttf/include/ftlibc.h
$(B)/ft/%.o: %.c $(FTCONF)
	@mkdir -p $(B)/ft
	$(CC) $(FTCFLAGS) -c $< -o $@

$(B)/ttf/html_lib.o: src/html_lib.c src/html_private.h
	@mkdir -p $(B)/ttf
	$(CC) $(CFLAGS) -DHTML_TTF '-DLIBNAME="htmlttf.gadget"' -c $< -o $@

$(B)/ttf/htmlttf_class.o: src/htmlttf_class.c src/htmlttf_render.h src/html_core.h $(FTCONF) src/html_private.h include/gadgets/html.h include/gadgets/htmlttf.h
	@mkdir -p $(B)/ttf
	$(CC) $(CFLAGS) -DHTML_TTF $(FTDEFS) -c $< -o $@

$(B)/ttf/htmlttf_render.o: src/htmlttf_render.c src/htmlttf_render.h src/html_core.h $(FTCONF)
	@mkdir -p $(B)/ttf
	$(CC) $(CFLAGS) $(FTDEFS) -c $< -o $@

$(O)/htmlttf.gadget: $(TTFOBJ)
	@mkdir -p $(O)
	$(CC) $(CPU) $(LIBFLAGS) -o $@.debug $(TTFOBJ) -L$(PREFIX)/m68k-amigaos/libnix/lib -Wl,--start-group -lgcc -lnix20 -Wl,--end-group
	$(STRIP) -o $@ $@.debug

bin/%: demo/%
	@mkdir -p bin
	cp $< $@

# Amiga sources and texts must be ISO-8859-1: refuse UTF-8 sequences.
# Not checked: README*.md (GitHub) and tools/*.py (host only), both UTF-8.
LATIN1  := $(wildcard src/*.c src/*.h include/*/*.h include/fd/*.fd sfd/*.sfd \
           demo/*.c demo/*.html test/*.c test/*.html test/*.expected \
           ttf/*.c ttf/include/*.h doc/*.doc package/* LICENSE Makefile)
charcheck:
	@if LC_ALL=C grep -lP '[\xC2-\xF4][\x80-\xBF]' $(LATIN1); then \
		echo "*** Die Dateien oben enthalten UTF-8, bitte nach ISO-8859-1 wandeln"; exit 1; fi

$(B)/%.o: src/%.c src/html_core.h src/html_private.h include/gadgets/html.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# html_lib.o must come first: it starts with "moveq #-1,d0; rts"
$(O)/html.gadget: $(LIBOBJ)
	@mkdir -p $(O)
	$(CC) $(CPU) $(LIBFLAGS) -o $@.debug $(LIBOBJ) -L$(PREFIX)/m68k-amigaos/libnix/lib -Wl,--start-group -lgcc -lnix20 -Wl,--end-group
	$(STRIP) -o $@ $@.debug

bin/HTMLDemo: demo/htmldemo.c include/gadgets/html.h
	@mkdir -p bin
	$(CC) $(DEMOFLAGS) -o $@ $<
	$(STRIP) $@

# host test of parser and layout engine (Linux/macOS)
test/hosttest: test/hosttest.c src/html_parse.c src/html_layout.c src/html_select.c src/html_core.h
	cc -g -Wall -fsanitize=address,undefined -o $@ test/hosttest.c src/html_parse.c src/html_layout.c src/html_select.c

# host preview of the FreeType renderer: renders a page into preview.ppm
HOSTFTSRC := ttf/ftbase_html.c $(FT)/src/base/ftinit.c $(FT)/src/base/ftsystem.c $(FT)/src/base/ftdebug.c \
             $(FT)/src/base/ftbitmap.c $(FT)/src/base/ftsynth.c $(FT)/src/base/fttype1.c \
             $(FT)/src/truetype/truetype.c $(FT)/src/sfnt/sfnt.c $(FT)/src/autofit/autofit.c $(FT)/src/smooth/smooth.c
HOSTFTDEFS := -Ittf/include -I$(FT)/include -I$(FT)/src/base -DFT2_BUILD_LIBRARY \
              '-DFT_CONFIG_OPTIONS_H=<ftoption_html.h>' '-DFT_CONFIG_MODULES_H=<ftmodule_html.h>'
test/ttfpreview: test/ttfpreview.c src/htmlttf_render.c src/htmlttf_render.h src/html_parse.c src/html_layout.c src/html_select.c src/html_check.c
	cc -g -O1 -w $(HOSTFTDEFS) -o $@ test/ttfpreview.c src/htmlttf_render.c src/html_parse.c src/html_layout.c src/html_select.c src/html_check.c $(HOSTFTSRC)

preview: test/ttfpreview
	./test/ttfpreview demo/example.html 560 preview.ppm demo/fonts Vera 12

# page:width pairs; the full hosttest output must match test/<page>.expected
CHECKS  := demo/example.html:400 demo/tabellen.html:400 demo/umfluss.html:400 \
           test/floats.html:300 test/rowspan.html:400 test/checkbox.html:400

check: test/hosttest
	@mkdir -p build/test; fail=0; \
	for t in $(CHECKS); do f=$${t%:*}; w=$${t#*:}; n=$$(basename $$f .html); \
		if ./test/hosttest $$f $$w > build/test/$$n.out && \
		   diff -u test/$$n.expected build/test/$$n.out; then echo "ok   $$f"; \
		else echo "FAIL $$f"; fail=1; fi; \
	done; exit $$fail

# accept the current layout as reference after an intended change
check-update: test/hosttest
	@for t in $(CHECKS); do f=$${t%:*}; w=$${t#*:}; n=$$(basename $$f .html); \
		./test/hosttest $$f $$w > test/$$n.expected && echo "updated test/$$n.expected"; \
	done

# sample icons of every style (icons/<Style>/*.info) and icons/preview.png
icons:
	python3 tools/mkicons.py

# Aminet archive: dist/html_gadget.lha (+ html_gadget.readme)
dist: all
	FT=$(FT) python3 tools/mkdist.py

clean:
	rm -rf build bin dist test/hosttest test/ttfpreview preview.ppm

.PHONY: all clean check check-update charcheck preview dist icons ttf020
