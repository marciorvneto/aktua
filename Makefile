.PHONY: aktua stdlib clean syntax repl \
	      test test-errors test-scripts test-svg test-repl

# These variables are here to support cross-compilation
# make PLATFORM=windows to cross-compile, otherwise it 
# defaults to linux
PLATFORM ?= linux

BUILD ?= debug

# Shared flags
CFLAGS  = -Wall -I./include/
LDFLAGS =
LDLIBS  = -lm


ifeq ($(STATIC),1)
LDFLAGS += -static
endif

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

TEST_RUN = AKTUA_NO_VIEWER=1 WSLENV=AKTUA_NO_VIEWER ./$(AKTUA)

aktua: $(AKTUA) stdlib

repl: build/repl

build/repl: repl.c lineedit.c include/lineedit.h | build
	$(CC) $(CFLAGS) repl.c lineedit.c -o build/repl

$(RES): ./platform-specific/windows/aktua.rc ./platform-specific/windows/aktua.manifest | build
	$(WINDRES) $< -O coff -o $@


$(AKTUA): $(SRC) include/core.h $(RES) | build
	$(CC) $(CFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LDLIBS) 

test: test-scripts test-errors test-svg test-repl
	@echo "All tests passed"

test-scripts: aktua
	@for f in test/*.akt examples/*.akt; do \
		$(TEST_RUN) "$$f" > /dev/null || { echo "FAILED: $$f"; exit 1; }; \
	done;

test-errors: aktua
	@for f in test/errors/*.akt; do \
		$(TEST_RUN) "$$f" > build/error_test.out 2>/dev/null && { echo "SHOULD HAVE FAILED: $$f"; exit 1; }; \
		diff -u --strip-trailing-cr "$${f%.akt}.expected" build/error_test.out || { \
			echo "WRONG OUTPUT: $$f"; \
			exit 1; \
		}; \
	done;

test-svg: aktua
	@rm -rf build/svg; mkdir -p build/svg
	@for f in test/svg/*.akt; do                                        \
		$(TEST_RUN) "$$f" > /dev/null || { echo "FAILED: $$f"; exit 1; }; \
	done;
	@for e in test/svg/*.expected; do                                   \
		out=build/svg/$$(basename "$${e%.expected}").svg;                 \
		diff -u --strip-trailing-cr "$$e" "$$out" || {                    \
			echo "WRONG SVG: $$out";                                        \
			exit 1;                                                         \
		};                                                                \
	done

test-repl: aktua 
	@for f in test/repl/*.akt; do                                                \
		$(TEST_RUN) -q < "$$f" 2> build/repl_test.out > /dev/null;                 \
		diff -u --strip-trailing-cr "$${f%.akt}.expected" build/repl_test.out || { \
			echo "WRONG OUTPUT: $$f";                                                \
			exit 1;                                                                  \
		};                                                                         \
		done;


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

