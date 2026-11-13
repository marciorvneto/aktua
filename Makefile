.PHONY: aktua stdlib clean test syntax repl

CFLAGS = -Wall -ggdb -I./include/
LDLIBS = -lm

SRC = main.c $(wildcard native/*.c)

aktua: build/aktua stdlib

repl: build/repl

build/repl: repl.c | build
	$(CC) $(CFLAGS) repl.c -o build/repl

build/aktua: $(SRC) include/core.h | build
	$(CC) $(CFLAGS) $(SRC) -o build/aktua $(LDLIBS)

test: aktua
	@for f in test/*.akt examples/*.akt; \
		do ./build/aktua $$f > /dev/null || { echo "FAILED: $$f"; exit 1; }; \
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
