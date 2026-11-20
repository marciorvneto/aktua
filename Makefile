.PHONY: aktua stdlib clean test syntax repl

# These variables are here to support cross-compilation
# make PLATFORM=windows to cross-compile, otherwise it 
# defaults to linux
PLATFORM ?= linux

BUILD ?= debug

# Shared flags
CFLAGS  = -Wall -I./include/
LDFLAGS =
LDLIBS  = -lm


ifeq ($(BUILD),release)
CFLAGS  += -O2
LDFLAGS += -s
else
CFLAGS  += -ggdb
endif

ifeq ($(PLATFORM),windows)
CC       = x86_64-w64-mingw32ucrt-gcc
EXE      = .exe
LDFLAGS += -static

# Resources (manifest file)
WINDRES = x86_64-w64-mingw32ucrt-windres
RES     = build/aktua_res.o
endif

AKTUA  = build/aktua$(EXE)

SRC = main.c lineedit.c platform.c $(wildcard native/*.c)

aktua: $(AKTUA) stdlib

repl: build/repl

build/repl: repl.c lineedit.c include/lineedit.h | build
	$(CC) $(CFLAGS) repl.c lineedit.c -o build/repl

$(RES): ./platform-specific/windows/aktua.rc ./platform-specific/windows/aktua.manifest | build
	$(WINDRES) $< -O coff -o $@


$(AKTUA): $(SRC) include/core.h $(RES) | build
	$(CC) $(CFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LDLIBS) 

test: aktua
	@for f in test/*.akt examples/*.akt; \
		do ./$(AKTUA) $$f > /dev/null || { echo "FAILED: $$f"; exit 1; }; \
		done; \
		echo "All tests passed"

build:
	mkdir -p build

stdlib: | build
	rm -rf build/libs
	cp -r libs build/

clean:
	rm -rf build

syntax: build/aktua
	@names=$$(./build/aktua --builtins | tr '\n' ' ' | sed 's/ $$//'); \
	sed -i "s/^\(  syntax keyword aktuaBuiltin\).*/\1 $$names/" editors/nvim/syntax/aktua.lua
