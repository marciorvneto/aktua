.PHONY: tctl stdlib clean test syntax

tctl: build/tctl stdlib

build/tctl: main.c | build
	gcc -ggdb main.c -lm -o build/tctl

test: tctl
	@for f in test/*.tc examples/*.tc; \
		do ./build/tctl $$f > /dev/null || { echo "FAILED: $$f"; exit 1; }; \
		done; \
		echo "All tests passed"

build:
	mkdir -p build

stdlib: | build
	rm -rf build/libs
	cp -r libs build/

clean:
	rm -rf build

syntax: build/tctl
	@names=$$(./build/tctl --builtins | tr '\n' ' ' | sed 's/ $$//'); \
	sed -i "s/^\(  syntax keyword tinyctrlBuiltin\).*/\1 $$names/" editors/nvim/syntax/tinyctrl.lua
