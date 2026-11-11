.PHONY: aktua stdlib clean test syntax

CFLAGS = -Wall -ggdb
LDLIBS = -lm

aktua: build/aktua stdlib

build/aktua: main.c | build
	gcc $(CFLAGS) main.c -o build/aktua $(LDLIBS)

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
