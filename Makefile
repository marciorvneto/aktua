.PHONY: tctl stdlib clean test

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
