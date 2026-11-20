.PHONY: aktua stdlib clean test syntax repl

# These variables are here to support cross-compilation
# make PLATFORM=windows to cross-compile, otherwise it 
# defaults to linux
PLATFORM ?= linux

ifeq ($(PLATFORM),windows)
CC      = x86_64-w64-mingw32ucrt-gcc
EXE     = .exe
LDFLAGS = -static
endif

AKTUA = build/aktua$(EXE)
CFLAGS = -Wall -ggdb -I./include/
LDLIBS = -lm

SRC = main.c lineedit.c platform.c $(wildcard native/*.c)

aktua: $(AKTUA) stdlib

repl: build/repl

build/repl: repl.c lineedit.c include/lineedit.h | build
	$(CC) $(CFLAGS) repl.c lineedit.c -o build/repl

$(AKTUA): $(SRC) include/core.h | build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS) 

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
